#include "win/bridge.hpp"

#include <cstring>
#include <vector>

#include "core/pod_layout.hpp"

namespace sp::win {

static_assert(sizeof(void*) == 8, "shaderprep targets 64-bit games only");
static_assert(sizeof(D3D12_RT_FORMAT_ARRAY) == pod::kRtFormatArraySize);
static_assert(offsetof(D3D12_RT_FORMAT_ARRAY, NumRenderTargets) == pod::kRtFormatArrayCountOffset);
static_assert(sizeof(DXGI_SAMPLE_DESC) == pod::kSampleDescSize);
static_assert(sizeof(D3D12_BLEND_DESC) == pod::kBlendDescSize);
static_assert(sizeof(D3D12_RASTERIZER_DESC) == pod::kRasterizerDescSize);
static_assert(sizeof(D3D12_DEPTH_STENCIL_DESC) == pod::kDepthStencilDescSize);
static_assert(offsetof(D3D12_BLEND_DESC, RenderTarget) == 8 && sizeof(D3D12_RENDER_TARGET_BLEND_DESC) == 40);
static_assert(offsetof(D3D12_DEPTH_STENCIL_DESC, FrontFace) == 20);
static_assert(offsetof(D3D12_DEPTH_STENCIL_DESC2, FrontFace) == 16 && sizeof(D3D12_DEPTH_STENCILOP_DESC1) == 20);

namespace {

struct TypeInfo {
    size_t size = 0;
    size_t align = 0;
};

template <typename T>
TypeInfo info_of() {
    return {sizeof(T), alignof(T)};
}

bool type_info(uint32_t type, TypeInfo& ti) {
    switch (type) {
        case SO_ROOT_SIGNATURE: ti = info_of<ID3D12RootSignature*>(); return true;
        case SO_VS: case SO_PS: case SO_DS: case SO_HS: case SO_GS: case SO_CS: case SO_AS: case SO_MS:
            ti = info_of<D3D12_SHADER_BYTECODE>(); return true;
        case SO_STREAM_OUTPUT: ti = info_of<D3D12_STREAM_OUTPUT_DESC>(); return true;
        case SO_BLEND: ti = info_of<D3D12_BLEND_DESC>(); return true;
        case SO_SAMPLE_MASK: ti = info_of<UINT>(); return true;
        case SO_RASTERIZER: ti = info_of<D3D12_RASTERIZER_DESC>(); return true;
        case SO_DEPTH_STENCIL: ti = info_of<D3D12_DEPTH_STENCIL_DESC>(); return true;
        case SO_INPUT_LAYOUT: ti = info_of<D3D12_INPUT_LAYOUT_DESC>(); return true;
        case SO_IB_STRIP_CUT_VALUE: ti = info_of<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(); return true;
        case SO_PRIMITIVE_TOPOLOGY: ti = info_of<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(); return true;
        case SO_RENDER_TARGET_FORMATS: ti = info_of<D3D12_RT_FORMAT_ARRAY>(); return true;
        case SO_DEPTH_STENCIL_FORMAT: ti = info_of<DXGI_FORMAT>(); return true;
        case SO_SAMPLE_DESC: ti = info_of<DXGI_SAMPLE_DESC>(); return true;
        case SO_NODE_MASK: ti = info_of<UINT>(); return true;
        case SO_CACHED_PSO: ti = info_of<D3D12_CACHED_PIPELINE_STATE>(); return true;
        case SO_FLAGS: ti = info_of<D3D12_PIPELINE_STATE_FLAGS>(); return true;
        case SO_DEPTH_STENCIL1: ti = info_of<D3D12_DEPTH_STENCIL_DESC1>(); return true;
        case SO_VIEW_INSTANCING: ti = info_of<D3D12_VIEW_INSTANCING_DESC>(); return true;
        case SO_DEPTH_STENCIL2: ti = info_of<D3D12_DEPTH_STENCIL_DESC2>(); return true;
        case SO_RASTERIZER1: ti = info_of<D3D12_RASTERIZER_DESC1>(); return true;
        case SO_RASTERIZER2: ti = info_of<D3D12_RASTERIZER_DESC2>(); return true;
        case SO_SERIALIZED_ROOT_SIGNATURE: ti = info_of<D3D12_SERIALIZED_ROOT_SIGNATURE_DESC>(); return true;
        default: return false;
    }
}

size_t align_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

bool fail(std::string* why, const char* msg) {
    if (why) *why = msg;
    return false;
}

bool add_shader(PsoDesc& d, uint32_t type, const D3D12_SHADER_BYTECODE& bc, const BlobSink& sink) {
    if (!bc.pShaderBytecode || !bc.BytecodeLength) return true;  // stage unused
    Subobject s;
    s.type = type;
    s.blob = sink(BlobKind::Shader, bc.pShaderBytecode, bc.BytecodeLength);
    d.subobjects.push_back(std::move(s));
    return true;
}

bool add_root_sig(PsoDesc& d, ID3D12RootSignature* rs, const RootSigLookup& lookup, std::string* why) {
    if (!rs) return true;  // root signature embedded in the shaders
    uint64_t h = lookup(rs);
    if (!h) return fail(why, "root signature created before shaderprep was loaded");
    Subobject s;
    s.type = SO_ROOT_SIGNATURE;
    s.blob = h;
    d.subobjects.push_back(std::move(s));
    return true;
}

template <typename T>
void add_pod(PsoDesc& d, uint32_t type, const T& v) {
    Subobject s;
    s.type = type;
    s.pod.assign(reinterpret_cast<const uint8_t*>(&v), reinterpret_cast<const uint8_t*>(&v) + sizeof(T));
    sanitize_pod(type, s.pod);
    d.subobjects.push_back(std::move(s));
}

void add_input_layout(PsoDesc& d, const D3D12_INPUT_LAYOUT_DESC& il) {
    Subobject s;
    s.type = SO_INPUT_LAYOUT;
    for (UINT i = 0; i < il.NumElements && il.pInputElementDescs; ++i) {
        const auto& e = il.pInputElementDescs[i];
        InputElement x;
        x.semantic = e.SemanticName ? e.SemanticName : "";
        x.semanticIndex = e.SemanticIndex;
        x.format = e.Format;
        x.inputSlot = e.InputSlot;
        x.alignedByteOffset = e.AlignedByteOffset;
        x.slotClass = e.InputSlotClass;
        x.instanceStepRate = e.InstanceDataStepRate;
        s.inputLayout.push_back(std::move(x));
    }
    d.subobjects.push_back(std::move(s));
}

void add_stream_output(PsoDesc& d, const D3D12_STREAM_OUTPUT_DESC& so) {
    if (!so.NumEntries) return;
    Subobject s;
    s.type = SO_STREAM_OUTPUT;
    for (UINT i = 0; i < so.NumEntries && so.pSODeclaration; ++i) {
        const auto& e = so.pSODeclaration[i];
        SoDeclEntry x;
        x.stream = e.Stream;
        x.semantic = e.SemanticName ? e.SemanticName : "";
        x.semanticIndex = e.SemanticIndex;
        x.startComponent = e.StartComponent;
        x.componentCount = e.ComponentCount;
        x.outputSlot = e.OutputSlot;
        s.streamOutput.entries.push_back(std::move(x));
    }
    for (UINT i = 0; i < so.NumStrides && so.pBufferStrides; ++i) s.streamOutput.strides.push_back(so.pBufferStrides[i]);
    s.streamOutput.rasterizedStream = so.RasterizedStream;
    d.subobjects.push_back(std::move(s));
}

D3D12_RT_FORMAT_ARRAY rt_array(const DXGI_FORMAT* formats, UINT n) {
    D3D12_RT_FORMAT_ARRAY a = {};
    a.NumRenderTargets = n > 8 ? 8 : n;
    // Entries past NumRenderTargets are ignored by the runtime; zero them so hashes are stable.
    for (UINT i = 0; i < a.NumRenderTargets; ++i) a.RTFormats[i] = formats[i];
    return a;
}

}  // namespace

bool capture_graphics(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& g, const BlobSink& sink,
                      const RootSigLookup& rootSigs, PsoDesc& out, std::string* why) {
    PsoDesc d;
    d.api = PsoApi::Graphics;
    if (!add_root_sig(d, g.pRootSignature, rootSigs, why)) return false;
    add_shader(d, SO_VS, g.VS, sink);
    add_shader(d, SO_PS, g.PS, sink);
    add_shader(d, SO_DS, g.DS, sink);
    add_shader(d, SO_HS, g.HS, sink);
    add_shader(d, SO_GS, g.GS, sink);
    add_stream_output(d, g.StreamOutput);
    add_pod(d, SO_BLEND, g.BlendState);
    add_pod(d, SO_SAMPLE_MASK, g.SampleMask);
    add_pod(d, SO_RASTERIZER, g.RasterizerState);
    add_pod(d, SO_DEPTH_STENCIL, g.DepthStencilState);
    add_input_layout(d, g.InputLayout);
    add_pod(d, SO_IB_STRIP_CUT_VALUE, g.IBStripCutValue);
    add_pod(d, SO_PRIMITIVE_TOPOLOGY, g.PrimitiveTopologyType);
    add_pod(d, SO_RENDER_TARGET_FORMATS, rt_array(g.RTVFormats, g.NumRenderTargets));
    add_pod(d, SO_DEPTH_STENCIL_FORMAT, g.DSVFormat);
    add_pod(d, SO_SAMPLE_DESC, g.SampleDesc);
    add_pod(d, SO_NODE_MASK, g.NodeMask);
    add_pod(d, SO_FLAGS, g.Flags);
    d.normalize();
    out = std::move(d);
    return true;
}

bool capture_compute(const D3D12_COMPUTE_PIPELINE_STATE_DESC& c, const BlobSink& sink,
                     const RootSigLookup& rootSigs, PsoDesc& out, std::string* why) {
    PsoDesc d;
    d.api = PsoApi::Compute;
    if (!add_root_sig(d, c.pRootSignature, rootSigs, why)) return false;
    add_shader(d, SO_CS, c.CS, sink);
    add_pod(d, SO_NODE_MASK, c.NodeMask);
    add_pod(d, SO_FLAGS, c.Flags);
    d.normalize();
    out = std::move(d);
    return true;
}

bool capture_stream(const D3D12_PIPELINE_STATE_STREAM_DESC& sd, const BlobSink& sink,
                    const RootSigLookup& rootSigs, PsoDesc& out, std::string* why) {
    PsoDesc d;
    d.api = PsoApi::Stream;
    auto* base = static_cast<const uint8_t*>(sd.pPipelineStateSubobjectStream);
    size_t size = sd.SizeInBytes;
    size_t off = 0;
    while (off < size) {
        if (size - off < sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)) return fail(why, "truncated stream");
        uint32_t type;
        std::memcpy(&type, base + off, 4);
        TypeInfo ti;
        if (!type_info(type, ti)) return fail(why, "unknown pipeline stream subobject type");
        size_t inner = align_up(off + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE), ti.align);
        if (inner + ti.size > size) return fail(why, "truncated stream subobject");
        const uint8_t* p = base + inner;
        switch (type) {
            case SO_ROOT_SIGNATURE: {
                ID3D12RootSignature* rs;
                std::memcpy(&rs, p, sizeof(rs));
                if (!add_root_sig(d, rs, rootSigs, why)) return false;
                break;
            }
            case SO_VS: case SO_PS: case SO_DS: case SO_HS: case SO_GS: case SO_CS: case SO_AS: case SO_MS: {
                D3D12_SHADER_BYTECODE bc;
                std::memcpy(&bc, p, sizeof(bc));
                add_shader(d, type, bc, sink);
                break;
            }
            case SO_STREAM_OUTPUT: {
                D3D12_STREAM_OUTPUT_DESC so;
                std::memcpy(&so, p, sizeof(so));
                add_stream_output(d, so);
                break;
            }
            case SO_INPUT_LAYOUT: {
                D3D12_INPUT_LAYOUT_DESC il;
                std::memcpy(&il, p, sizeof(il));
                add_input_layout(d, il);
                break;
            }
            case SO_RENDER_TARGET_FORMATS: {
                D3D12_RT_FORMAT_ARRAY a;
                std::memcpy(&a, p, sizeof(a));
                add_pod(d, type, rt_array(a.RTFormats, a.NumRenderTargets));
                break;
            }
            case SO_CACHED_PSO:
                break;  // driver cache blob, not part of the pipeline's identity
            case SO_VIEW_INSTANCING: {
                D3D12_VIEW_INSTANCING_DESC vi;
                std::memcpy(&vi, p, sizeof(vi));
                Subobject s;
                s.type = type;
                s.viewInstancing.flags = vi.Flags;
                for (UINT i = 0; i < vi.ViewInstanceCount && vi.pViewInstanceLocations; ++i) {
                    s.viewInstancing.viewportIndex.push_back(vi.pViewInstanceLocations[i].ViewportArrayIndex);
                    s.viewInstancing.rtArrayIndex.push_back(vi.pViewInstanceLocations[i].RenderTargetArrayIndex);
                }
                d.subobjects.push_back(std::move(s));
                break;
            }
            case SO_SERIALIZED_ROOT_SIGNATURE: {
                D3D12_SERIALIZED_ROOT_SIGNATURE_DESC srs;
                std::memcpy(&srs, p, sizeof(srs));
                if (srs.pSerializedBlob && srs.SerializedBlobSizeInBytes) {
                    Subobject s;
                    s.type = type;
                    s.blob = sink(BlobKind::RootSignature, srs.pSerializedBlob, srs.SerializedBlobSizeInBytes);
                    d.subobjects.push_back(std::move(s));
                }
                break;
            }
            default: {
                Subobject s;
                s.type = type;
                s.pod.assign(p, p + ti.size);
                sanitize_pod(type, s.pod);
                d.subobjects.push_back(std::move(s));
                break;
            }
        }
        off = align_up(inner + ti.size, sizeof(void*));
    }
    d.normalize();
    out = std::move(d);
    return true;
}

ID3D12RootSignature* RootSigCache::get(ID3D12Device* device, uint64_t hash, const PsoDb& blobs) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = cache_.find(hash);
    if (it != cache_.end()) return it->second;
    if (failed_.count(hash)) return nullptr;
    const Blob* b = blobs.blob(hash);
    ID3D12RootSignature* rs = nullptr;
    if (!b || FAILED(device->CreateRootSignature(0, b->data.data(), b->data.size(), IID_PPV_ARGS(&rs)))) {
        failed_[hash] = true;
        return nullptr;
    }
    cache_[hash] = rs;
    return rs;
}

void RootSigCache::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [h, rs] : cache_) rs->Release();
    cache_.clear();
    failed_.clear();
}

namespace {

// Owns everything the D3D12 structs point at while a replay call is in flight.
struct ReplayStorage {
    std::vector<std::vector<D3D12_INPUT_ELEMENT_DESC>> layouts;
    std::vector<std::vector<D3D12_SO_DECLARATION_ENTRY>> soDecls;
    std::vector<std::vector<D3D12_VIEW_INSTANCE_LOCATION>> viewLocs;
};

D3D12_SHADER_BYTECODE bytecode(const PsoDb& blobs, uint64_t h) {
    D3D12_SHADER_BYTECODE bc = {};
    if (const Blob* b = blobs.blob(h)) {
        bc.pShaderBytecode = b->data.data();
        bc.BytecodeLength = b->data.size();
    }
    return bc;
}

D3D12_INPUT_LAYOUT_DESC input_layout(const Subobject& s, ReplayStorage& st) {
    auto& v = st.layouts.emplace_back();
    for (const auto& e : s.inputLayout)
        v.push_back({e.semantic.c_str(), e.semanticIndex, static_cast<DXGI_FORMAT>(e.format), e.inputSlot,
                     e.alignedByteOffset, static_cast<D3D12_INPUT_CLASSIFICATION>(e.slotClass), e.instanceStepRate});
    return {v.empty() ? nullptr : v.data(), static_cast<UINT>(v.size())};
}

D3D12_STREAM_OUTPUT_DESC stream_output(const Subobject& s, ReplayStorage& st) {
    auto& v = st.soDecls.emplace_back();
    for (const auto& e : s.streamOutput.entries)
        v.push_back({e.stream, e.semantic.empty() ? nullptr : e.semantic.c_str(), e.semanticIndex, e.startComponent,
                     e.componentCount, e.outputSlot});
    D3D12_STREAM_OUTPUT_DESC so = {};
    so.pSODeclaration = v.empty() ? nullptr : v.data();
    so.NumEntries = static_cast<UINT>(v.size());
    so.pBufferStrides = s.streamOutput.strides.empty() ? nullptr : s.streamOutput.strides.data();
    so.NumStrides = static_cast<UINT>(s.streamOutput.strides.size());
    so.RasterizedStream = s.streamOutput.rasterizedStream;
    return so;
}

template <typename T>
bool pod_as(const Subobject& s, T& out) {
    if (s.pod.size() != sizeof(T)) return false;
    std::memcpy(&out, s.pod.data(), sizeof(T));
    return true;
}

HRESULT replay_stream(ID3D12Device* device, const PsoDesc& desc, const PsoDb& blobs, RootSigCache& rsc,
                      ID3D12PipelineState** out) {
    ID3D12Device2* dev2 = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dev2)))) return E_NOINTERFACE;
    ReplayStorage st;
    std::vector<uint64_t> storage;  // 8-byte aligned backing store for the stream
    size_t used = 0;
    auto append = [&](uint32_t type, const void* inner, const TypeInfo& ti) {
        size_t start = align_up(used, sizeof(void*));
        size_t innerOff = align_up(start + 4, ti.align);
        size_t end = align_up(innerOff + ti.size, sizeof(void*));
        storage.resize((end + 7) / 8, 0);
        auto* bytes = reinterpret_cast<uint8_t*>(storage.data());
        std::memcpy(bytes + start, &type, 4);
        std::memcpy(bytes + innerOff, inner, ti.size);
        used = end;
    };
    HRESULT hr = S_OK;
    for (const auto& s : desc.subobjects) {
        TypeInfo ti;
        if (!type_info(s.type, ti)) {
            hr = E_INVALIDARG;
            break;
        }
        switch (s.type) {
            case SO_ROOT_SIGNATURE: {
                ID3D12RootSignature* rs = rsc.get(device, s.blob, blobs);
                if (!rs) hr = E_INVALIDARG;
                append(s.type, &rs, ti);
                break;
            }
            case SO_VS: case SO_PS: case SO_DS: case SO_HS: case SO_GS: case SO_CS: case SO_AS: case SO_MS: {
                D3D12_SHADER_BYTECODE bc = bytecode(blobs, s.blob);
                if (!bc.pShaderBytecode) hr = E_INVALIDARG;
                append(s.type, &bc, ti);
                break;
            }
            case SO_STREAM_OUTPUT: {
                auto so = stream_output(s, st);
                append(s.type, &so, ti);
                break;
            }
            case SO_INPUT_LAYOUT: {
                auto il = input_layout(s, st);
                append(s.type, &il, ti);
                break;
            }
            case SO_VIEW_INSTANCING: {
                auto& v = st.viewLocs.emplace_back();
                for (size_t i = 0; i < s.viewInstancing.viewportIndex.size(); ++i)
                    v.push_back({s.viewInstancing.viewportIndex[i], s.viewInstancing.rtArrayIndex[i]});
                D3D12_VIEW_INSTANCING_DESC vi = {static_cast<UINT>(v.size()), v.empty() ? nullptr : v.data(),
                                                 static_cast<D3D12_VIEW_INSTANCING_FLAGS>(s.viewInstancing.flags)};
                append(s.type, &vi, ti);
                break;
            }
            case SO_SERIALIZED_ROOT_SIGNATURE: {
                const Blob* b = blobs.blob(s.blob);
                if (!b) hr = E_INVALIDARG;
                D3D12_SERIALIZED_ROOT_SIGNATURE_DESC srs = {b ? b->data.data() : nullptr, b ? b->data.size() : 0};
                append(s.type, &srs, ti);
                break;
            }
            default:
                if (s.pod.size() != ti.size) {
                    hr = E_INVALIDARG;
                    break;
                }
                append(s.type, s.pod.data(), ti);
                break;
        }
        if (FAILED(hr)) break;
    }
    if (SUCCEEDED(hr)) {
        D3D12_PIPELINE_STATE_STREAM_DESC sd = {used, storage.data()};
        ID3D12PipelineState* pso = nullptr;
        hr = dev2->CreatePipelineState(&sd, IID_PPV_ARGS(&pso));
        if (SUCCEEDED(hr)) {
            if (out) *out = pso;
            else pso->Release();
        }
    }
    dev2->Release();
    return hr;
}

}  // namespace

HRESULT replay(ID3D12Device* device, const PsoDesc& desc, const PsoDb& blobs, RootSigCache& rsc,
               ID3D12PipelineState** out) {
    if (desc.api == PsoApi::Stream) return replay_stream(device, desc, blobs, rsc, out);

    ReplayStorage st;
    ID3D12RootSignature* rs = nullptr;
    if (const Subobject* s = desc.find(SO_ROOT_SIGNATURE)) {
        rs = rsc.get(device, s->blob, blobs);
        if (!rs) return E_INVALIDARG;
    }
    auto shader = [&](uint32_t type) {
        const Subobject* s = desc.find(type);
        return s ? bytecode(blobs, s->blob) : D3D12_SHADER_BYTECODE{};
    };
    ID3D12PipelineState* pso = nullptr;
    HRESULT hr;
    if (desc.api == PsoApi::Compute) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC c = {};
        c.pRootSignature = rs;
        c.CS = shader(SO_CS);
        if (const Subobject* s = desc.find(SO_NODE_MASK)) pod_as(*s, c.NodeMask);
        if (const Subobject* s = desc.find(SO_FLAGS)) pod_as(*s, c.Flags);
        hr = device->CreateComputePipelineState(&c, IID_PPV_ARGS(&pso));
    } else {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC g = {};
        g.pRootSignature = rs;
        g.VS = shader(SO_VS);
        g.PS = shader(SO_PS);
        g.DS = shader(SO_DS);
        g.HS = shader(SO_HS);
        g.GS = shader(SO_GS);
        if (const Subobject* s = desc.find(SO_STREAM_OUTPUT)) g.StreamOutput = stream_output(*s, st);
        if (const Subobject* s = desc.find(SO_BLEND)) pod_as(*s, g.BlendState);
        if (const Subobject* s = desc.find(SO_SAMPLE_MASK)) pod_as(*s, g.SampleMask);
        if (const Subobject* s = desc.find(SO_RASTERIZER)) pod_as(*s, g.RasterizerState);
        if (const Subobject* s = desc.find(SO_DEPTH_STENCIL)) pod_as(*s, g.DepthStencilState);
        if (const Subobject* s = desc.find(SO_INPUT_LAYOUT)) g.InputLayout = input_layout(*s, st);
        if (const Subobject* s = desc.find(SO_IB_STRIP_CUT_VALUE)) pod_as(*s, g.IBStripCutValue);
        if (const Subobject* s = desc.find(SO_PRIMITIVE_TOPOLOGY)) pod_as(*s, g.PrimitiveTopologyType);
        if (const Subobject* s = desc.find(SO_RENDER_TARGET_FORMATS)) {
            D3D12_RT_FORMAT_ARRAY a = {};
            pod_as(*s, a);
            g.NumRenderTargets = a.NumRenderTargets;
            for (int i = 0; i < 8; ++i) g.RTVFormats[i] = a.RTFormats[i];
        }
        if (const Subobject* s = desc.find(SO_DEPTH_STENCIL_FORMAT)) pod_as(*s, g.DSVFormat);
        if (const Subobject* s = desc.find(SO_SAMPLE_DESC)) pod_as(*s, g.SampleDesc);
        if (const Subobject* s = desc.find(SO_NODE_MASK)) pod_as(*s, g.NodeMask);
        if (const Subobject* s = desc.find(SO_FLAGS)) pod_as(*s, g.Flags);
        hr = device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&pso));
    }
    if (SUCCEEDED(hr)) {
        if (out) *out = pso;
        else pso->Release();
    }
    return hr;
}

}  // namespace sp::win
