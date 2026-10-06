// shaderprep-warm.exe: safe-mode pre-compiler. Runs as its own process, never touches the game.
//
//   shaderprep-warm [--threads N] [--adapter N] <db.psodb>...
//       Compiles every known and inferred pipeline on the chosen GPU, so the driver's shader cache
//       is warm before the game starts. Only helps if the driver shares its cache between
//       executables; check that with --cache-test.
//
//   shaderprep-warm --cache-test [--seed S] [--count N] [--adapter N]
//       Milestone M0 experiment. Compiles N unique compute pipelines and times them:
//         1) run it twice with the same seed: the 2nd run should be much faster (disk cache works)
//         2) copy this exe to another name, run with the same seed again: still fast means the
//            cache is shared across executables -> safe mode is viable on this driver.
#include "win/d3d12.hpp"
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/psodb.hpp"
#include "infer/infer.hpp"
#include "win/log.hpp"
#include "win/warm.hpp"

using namespace sp;
using namespace sp::win;

namespace {

IDXGIAdapter1* pick_adapter(int index) {
    IDXGIFactory1* f1 = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f1)))) return nullptr;
    IDXGIAdapter1* chosen = nullptr;
    IDXGIFactory6* f6 = nullptr;
    if (index < 0 && SUCCEEDED(f1->QueryInterface(IID_PPV_ARGS(&f6)))) {
        // The game will almost always run on the high-performance GPU.
        f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&chosen));
        f6->Release();
    }
    if (!chosen) {
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; f1->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d;
            a->GetDesc1(&d);
            bool hw = !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
            if ((index < 0 && hw) || static_cast<int>(i) == index) {
                chosen = a;
                break;
            }
            a->Release();
        }
    }
    f1->Release();
    return chosen;
}

ID3D12Device* create_device(int adapterIndex) {
    IDXGIAdapter1* adapter = pick_adapter(adapterIndex);
    if (!adapter) {
        std::fprintf(stderr, "error: no suitable GPU found\n");
        return nullptr;
    }
    DXGI_ADAPTER_DESC1 d;
    adapter->GetDesc1(&d);
    std::printf("GPU: %s\n", narrow(d.Description).c_str());
    ID3D12Device* device = nullptr;
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    adapter->Release();
    if (FAILED(hr)) {
        std::fprintf(stderr, "error: D3D12CreateDevice failed (0x%08lx)\n", static_cast<unsigned long>(hr));
        return nullptr;
    }
    return device;
}

int warm(const std::vector<std::string>& files, uint32_t threads, int adapter) {
    auto db = std::make_shared<PsoDb>();
    for (const auto& f : files) {
        PsoDb part;
        std::string err;
        if (!PsoDb::load(f, part, &err)) {
            std::fprintf(stderr, "warning: %s\n", err.c_str());
            continue;
        }
        db->merge(part);
    }
    db->drop_dangling();
    StyleModel style;
    style.learn(*db);
    PsoDb cands;
    infer_candidates(*db, style, cands);
    db->merge(cands);
    std::printf("%zu pipelines to compile (%zu inferred)\n", db->psos().size(), cands.psos().size());
    if (db->psos().empty()) {
        std::printf("nothing to do: no recordings or shaders with enough information yet.\n");
        return 0;
    }
    ID3D12Device* device = create_device(adapter);
    if (!device) return 1;
    ULONGLONG t0 = GetTickCount64();
    {
        Warmer w(device, threads, false);
        w.submit(db);
        while (!w.wait_idle(1000)) {
            const auto& s = w.stats();
            std::printf("\r  %llu / %llu", (unsigned long long)s.done.load(), (unsigned long long)s.queued.load());
            std::fflush(stdout);
        }
        const auto& s = w.stats();
        std::printf("\rdone in %.1f s: %llu compiled, %llu rejected guesses, %llu failed\n",
                    (GetTickCount64() - t0) / 1000.0, (unsigned long long)s.created.load(),
                    (unsigned long long)s.rejected.load(), (unsigned long long)s.failed.load());
    }
    device->Release();
    return 0;
}

int cache_test(uint32_t seed, uint32_t count, int adapter) {
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? reinterpret_cast<pD3DCompile>(reinterpret_cast<void*>(GetProcAddress(compiler, "D3DCompile"))) : nullptr;
    if (!compile) {
        std::fprintf(stderr, "error: d3dcompiler_47.dll not available\n");
        return 1;
    }
    ID3D12Device* device = create_device(adapter);
    if (!device) return 1;

    // Heavy-ish unrolled math so the driver compile takes long enough to measure.
    std::vector<ID3DBlob*> shaders;
    for (uint32_t i = 0; i < count; ++i) {
        char src[1024];
        std::snprintf(src, sizeof(src),
                      "RWByteAddressBuffer o : register(u0);\n"
                      "[numthreads(64,1,1)] void main(uint3 id : SV_DispatchThreadID) {\n"
                      "  float x = id.x * %u.0 + %u.0;\n"
                      "  [unroll] for (int k = 0; k < 256; ++k) { x = sin(x) * cos(x * %u.5) + sqrt(abs(x)) + k; }\n"
                      "  o.Store(id.x * 4, asuint(x));\n}\n",
                      seed, i, (seed * 31 + i) % 97);
        ID3DBlob* code = nullptr;
        ID3DBlob* err = nullptr;
        if (FAILED(compile(src, std::strlen(src), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err))) {
            std::fprintf(stderr, "error: HLSL compile failed: %s\n",
                         err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            return 1;
        }
        shaders.push_back(code);
    }
    // One root UAV, matching the shader's raw buffer.
    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters = &param;
    ID3DBlob* rsBlob = nullptr;
    ID3DBlob* rsErr = nullptr;
    ID3D12RootSignature* rs = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr)) ||
        FAILED(device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rs)))) {
        std::fprintf(stderr, "error: cannot create root signature\n");
        return 1;
    }
    rsBlob->Release();

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    uint32_t ok = 0;
    for (ID3DBlob* code : shaders) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = rs;
        desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        ID3D12PipelineState* pso = nullptr;
        if (SUCCEEDED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)))) {
            ok++;
            pso->Release();
        }
    }
    QueryPerformanceCounter(&t1);
    double ms = 1000.0 * double(t1.QuadPart - t0.QuadPart) / double(freq.QuadPart);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::printf("exe:  %s\nseed: %u\ncreated %u/%u pipelines in %.1f ms (%.2f ms each)\n", narrow(exe).c_str(), seed, ok,
                count, ms, ok ? ms / ok : 0.0);
    std::printf("Compare runs: same seed + same exe fast => disk cache works; same seed + renamed exe fast =>\n"
                "cache is shared across executables (safe mode can help on this driver).\n");
    // Give the driver a moment to flush its cache to disk before the process exits.
    Sleep(2000);
    for (auto* s : shaders) s->Release();
    rs->Release();
    device->Release();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> files;
    uint32_t threads = 0, seed = 1, count = 200;
    int adapter = -1;
    bool test = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](uint32_t& v) {
            if (i + 1 < argc) v = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        };
        if (a == "--threads") next(threads);
        else if (a == "--seed") next(seed);
        else if (a == "--count") next(count);
        else if (a == "--adapter" && i + 1 < argc) adapter = std::atoi(argv[++i]);
        else if (a == "--cache-test") test = true;
        else if (a == "-h" || a == "--help") {
            std::puts("usage: shaderprep-warm [--threads N] [--adapter N] <db.psodb>...\n"
                      "       shaderprep-warm --cache-test [--seed S] [--count N] [--adapter N]");
            return 0;
        } else files.push_back(a);
    }
    if (test) return cache_test(seed, count, adapter);
    if (files.empty()) {
        std::fprintf(stderr, "error: no .psodb files given (try --help)\n");
        return 2;
    }
    return warm(files, threads, adapter);
}
