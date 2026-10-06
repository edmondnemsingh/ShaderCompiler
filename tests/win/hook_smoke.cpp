// End-to-end smoke test of the proxy d3d12.dll, run next to it (CI uses WARP; also works under
// Wine + vkd3d). It plays the role of a game:
//
//   hook_smoke record   creates a few pipelines (classic graphics, stream, compute) and also
//                       compiles one extra pixel shader that it never uses, then waits for the
//                       proxy's memory harvest + recording flush.
//   hook_smoke check    verifies what the proxy wrote: recorded pipelines, the harvested unused
//                       shader, and that a candidate pipeline was inferred for it.
//   hook_smoke warm     starts again; the proxy must pre-compile the recorded pipelines.
#include "win/d3d12.hpp"
#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/psodb.hpp"
#include "dxbc/dxbc.hpp"

namespace {

int fail(const char* msg) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

pD3DCompile g_compile = nullptr;

ID3DBlob* compile(const char* src, const char* target) {
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(g_compile(src, std::strlen(src), nullptr, nullptr, nullptr, "main", target, 0, 0, &code, &err))) {
        std::fprintf(stderr, "compile error: %s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return nullptr;
    }
    return code;
}

struct Ctx {
    ID3D12Device* device = nullptr;
    ID3D12RootSignature* rs = nullptr;
};

bool setup(Ctx& c) {
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");  // resolves to the proxy next to this exe
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!d3d12 || !compiler) return false;
    g_compile = reinterpret_cast<pD3DCompile>(reinterpret_cast<void*>(GetProcAddress(compiler, "D3DCompile")));
    auto create = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice")));
    auto serialize = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(
        reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")));
    if (!g_compile || !create || !serialize) return false;
    if (FAILED(create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&c.device)))) {
        std::fprintf(stderr, "D3D12CreateDevice failed\n");
        return false;
    }
    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) return false;
    return SUCCEEDED(c.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&c.rs)));
}

const char* kVs = "float4 main(float3 p : POSITION) : SV_Position { return float4(p, 1.0); }";
std::string ps_src(int i) {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "float4 main(float4 p : SV_Position) : SV_Target { return float4(%d.0, 0.25, 0.5, 1.0); }", i);
    return buf;
}
const char* kCs = "[numthreads(8, 1, 1)] void main(uint3 id : SV_DispatchThreadID) { }";

D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics_desc(ID3D12RootSignature* rs, ID3DBlob* vs, ID3DBlob* ps,
                                                 const D3D12_INPUT_ELEMENT_DESC* il) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC g = {};
    g.pRootSignature = rs;
    g.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    g.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    g.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    g.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    g.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    g.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    g.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    g.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    g.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    g.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    g.SampleMask = 0xFFFFFFFF;
    g.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    g.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    g.RasterizerState.DepthClipEnable = TRUE;
    g.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    g.InputLayout = {il, 1};
    g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    g.NumRenderTargets = 1;
    g.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    g.SampleDesc.Count = 1;
    return g;
}

int run_record() {
    // Harvest quickly instead of waiting minutes like a real game session.
    CreateDirectoryW(L"shaderprep", nullptr);
    std::ofstream("shaderprep/profile.toml") << "[harvest]\nscan_delays_s = [3]\n";
    Ctx c;
    if (!setup(c)) return fail("setup");
    ID3DBlob* vs = compile(kVs, "vs_5_0");
    ID3DBlob* cs = compile(kCs, "cs_5_0");
    if (!vs || !cs) return fail("compile");
    D3D12_INPUT_ELEMENT_DESC il = {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    int created = 0;
    for (int i = 0; i < 3; ++i) {
        ID3DBlob* ps = compile(ps_src(i).c_str(), "ps_5_0");
        if (!ps) return fail("compile ps");
        auto g = graphics_desc(c.rs, vs, ps, &il);
        ID3D12PipelineState* pso = nullptr;
        if (SUCCEEDED(c.device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&pso)))) {
            created++;
            pso->Release();
        }
    }
    // Stream API (mesh-shader era entry point) with the same shaders and a different PS.
    ID3D12Device2* dev2 = nullptr;
    if (SUCCEEDED(c.device->QueryInterface(IID_PPV_ARGS(&dev2)))) {
        ID3DBlob* ps = compile(ps_src(10).c_str(), "ps_5_0");
        struct alignas(void*) Rs { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE; ID3D12RootSignature* v; };
        struct alignas(void*) Sh { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t; D3D12_SHADER_BYTECODE v; };
        struct alignas(void*) Il { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT; D3D12_INPUT_LAYOUT_DESC v; };
        struct alignas(void*) Rt { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS; D3D12_RT_FORMAT_ARRAY v; };
        struct Stream { Rs rs; Sh vs; Sh ps; Il il; Rt rt; } s;
        s.rs.v = c.rs;
        s.vs = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, {vs->GetBufferPointer(), vs->GetBufferSize()}};
        s.ps = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, {ps->GetBufferPointer(), ps->GetBufferSize()}};
        s.il.v = {&il, 1};
        s.rt.v = {};
        s.rt.v.NumRenderTargets = 1;
        s.rt.v.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        D3D12_PIPELINE_STATE_STREAM_DESC sd = {sizeof(s), &s};
        ID3D12PipelineState* pso = nullptr;
        if (SUCCEEDED(dev2->CreatePipelineState(&sd, IID_PPV_ARGS(&pso)))) {
            created++;
            pso->Release();
        } else {
            std::fprintf(stderr, "note: stream PSO creation failed on this driver\n");
        }
        dev2->Release();
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
    cd.pRootSignature = c.rs;
    cd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    ID3D12PipelineState* pso = nullptr;
    if (SUCCEEDED(c.device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&pso)))) {
        created++;
        pso->Release();
    }
    // A shader that sits in memory but is never used: the harvest should find it.
    ID3DBlob* unused = compile(ps_src(77).c_str(), "ps_5_0");
    if (!unused) return fail("compile unused");
    std::printf("created %d pipelines; waiting for harvest and flush...\n", created);
    Sleep(14000);
    unused->Release();
    return created >= 4 ? 0 : fail("too few pipelines created");
}

int run_check() {
    sp::PsoDb rec, harvest;
    std::string err;
    if (!sp::PsoDb::load("shaderprep/recorded.psodb", rec, &err)) return fail(err.c_str());
    std::printf("recorded: %zu pipelines, %zu shaders\n", rec.psos().size(), rec.count_blobs(sp::BlobKind::Shader));
    if (rec.psos().size() < 4) return fail("expected at least 4 recorded pipelines");
    for (const auto& e : rec.psos())
        if (e.source != sp::PsoSource::Recorded) return fail("unexpected source");
    if (!sp::PsoDb::load("shaderprep/harvest.psodb", harvest, &err)) return fail(err.c_str());
    // The unused PS must be among harvested blobs but not among recorded ones.
    size_t newShaders = 0;
    for (const auto& [h, b] : harvest.blobs())
        if (b.kind == sp::BlobKind::Shader && !rec.blob(h)) newShaders++;
    std::printf("harvested from memory: %zu shaders, %zu not used by any recorded pipeline\n",
                harvest.count_blobs(sp::BlobKind::Shader), newShaders);
    if (newShaders == 0) return fail("memory harvest did not find the unused shader");
    std::ifstream log("shaderprep/log.txt");
    std::stringstream ss;
    ss << log.rdbuf();
    std::string text = ss.str();
    if (text.find("inferred") == std::string::npos) return fail("no inference in log");
    std::printf("ok\n");
    return 0;
}

int run_warm() {
    Ctx c;
    if (!setup(c)) return fail("setup");
    Sleep(6000);
    std::ifstream log("shaderprep/log.txt");
    std::stringstream ss;
    ss << log.rdbuf();
    std::string text = ss.str();
    size_t pos = text.rfind("warming started: ");
    if (pos == std::string::npos) return fail("warming did not start");
    unsigned queued = 0;
    std::sscanf(text.c_str() + pos, "warming started: %u", &queued);
    std::printf("warm queued %u pipelines at startup\n", queued);
    if (queued < 4) return fail("expected recorded pipelines to be queued");
    pos = text.rfind("startup pre-compile finished");
    if (pos == std::string::npos) return fail("pre-compile did not finish");
    unsigned compiled = 0, rejected = 0, failed = 0;
    const char* p = std::strstr(text.c_str() + pos, ": ");
    if (p) std::sscanf(p, ": %u compiled, %u rejected guesses, %u failed", &compiled, &rejected, &failed);
    std::printf("pre-compiled %u, rejected %u, failed %u\n", compiled, rejected, failed);
    // Every recorded pipeline must replay; inferred guesses may be rejected.
    if (compiled < 4 || failed) return fail("recorded pipelines did not replay");
    std::printf("ok\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "record") return run_record();
    if (mode == "check") return run_check();
    if (mode == "warm") return run_warm();
    std::fprintf(stderr, "usage: hook_smoke record|check|warm\n");
    return 2;
}
