#include "core/pso.hpp"

#include <algorithm>

#include "core/hash.hpp"
#include "core/pod_layout.hpp"

namespace sp {

bool is_shader_subobject(uint32_t type) {
    switch (type) {
        case SO_VS: case SO_PS: case SO_DS: case SO_HS: case SO_GS:
        case SO_CS: case SO_AS: case SO_MS:
            return true;
        default:
            return false;
    }
}

void sanitize_pod(uint32_t type, Bytes& pod) {
    auto zero = [&](size_t off, size_t n) {
        for (size_t i = off; i < off + n && i < pod.size(); ++i) pod[i] = 0;
    };
    switch (type) {
        case SO_BLEND:
            // 8 x D3D12_RENDER_TARGET_BLEND_DESC (40 bytes) after two BOOLs; byte 36 is the write
            // mask, 37..39 are padding.
            for (size_t rt = 0; rt < 8; ++rt) zero(8 + rt * 40 + 37, 3);
            break;
        case SO_DEPTH_STENCIL:
        case SO_DEPTH_STENCIL1:
            zero(18, 2);  // after StencilReadMask/StencilWriteMask
            break;
        case SO_DEPTH_STENCIL2:
            zero(34, 2);  // FrontFace masks padding
            zero(54, 2);  // BackFace masks padding
            break;
        default:
            break;
    }
}

const char* subobject_name(uint32_t type) {
    switch (type) {
        case SO_ROOT_SIGNATURE: return "root_signature";
        case SO_VS: return "vs";
        case SO_PS: return "ps";
        case SO_DS: return "ds";
        case SO_HS: return "hs";
        case SO_GS: return "gs";
        case SO_CS: return "cs";
        case SO_STREAM_OUTPUT: return "stream_output";
        case SO_BLEND: return "blend";
        case SO_SAMPLE_MASK: return "sample_mask";
        case SO_RASTERIZER: return "rasterizer";
        case SO_DEPTH_STENCIL: return "depth_stencil";
        case SO_INPUT_LAYOUT: return "input_layout";
        case SO_IB_STRIP_CUT_VALUE: return "ib_strip_cut";
        case SO_PRIMITIVE_TOPOLOGY: return "topology";
        case SO_RENDER_TARGET_FORMATS: return "rt_formats";
        case SO_DEPTH_STENCIL_FORMAT: return "ds_format";
        case SO_SAMPLE_DESC: return "sample_desc";
        case SO_NODE_MASK: return "node_mask";
        case SO_CACHED_PSO: return "cached_pso";
        case SO_FLAGS: return "flags";
        case SO_DEPTH_STENCIL1: return "depth_stencil1";
        case SO_VIEW_INSTANCING: return "view_instancing";
        case SO_AS: return "as";
        case SO_MS: return "ms";
        case SO_DEPTH_STENCIL2: return "depth_stencil2";
        case SO_RASTERIZER1: return "rasterizer1";
        case SO_RASTERIZER2: return "rasterizer2";
        case SO_SERIALIZED_ROOT_SIGNATURE: return "serialized_root_signature";
        default: return "unknown";
    }
}

const char* source_name(PsoSource s) {
    switch (s) {
        case PsoSource::Recorded: return "recorded";
        case PsoSource::Imported: return "imported";
        case PsoSource::Inferred: return "inferred";
        case PsoSource::Engine: return "engine";
    }
    return "unknown";
}

void PsoDesc::normalize() {
    // The cached-PSO blob is a driver lookup accelerator, not part of the pipeline's identity.
    subobjects.erase(std::remove_if(subobjects.begin(), subobjects.end(),
                                    [](const Subobject& s) { return s.type == SO_CACHED_PSO; }),
                     subobjects.end());
    std::stable_sort(subobjects.begin(), subobjects.end(),
                     [](const Subobject& a, const Subobject& b) { return a.type < b.type; });
    // Last writer wins for duplicate types, mirroring how the runtime treats repeated subobjects.
    std::vector<Subobject> unique;
    for (auto& s : subobjects) {
        if (!unique.empty() && unique.back().type == s.type)
            unique.back() = std::move(s);
        else
            unique.push_back(std::move(s));
    }
    subobjects = std::move(unique);
}

void PsoDesc::encode(ByteWriter& w) const {
    w.u8(static_cast<uint8_t>(api));
    w.u32(static_cast<uint32_t>(subobjects.size()));
    for (const auto& s : subobjects) {
        w.u32(s.type);
        w.u64(s.blob);
        w.blob(s.pod);
        w.u32(static_cast<uint32_t>(s.inputLayout.size()));
        for (const auto& e : s.inputLayout) {
            w.str(e.semantic);
            w.u32(e.semanticIndex);
            w.u32(e.format);
            w.u32(e.inputSlot);
            w.u32(e.alignedByteOffset);
            w.u32(e.slotClass);
            w.u32(e.instanceStepRate);
        }
        const auto& so = s.streamOutput;
        w.u32(static_cast<uint32_t>(so.entries.size()));
        for (const auto& e : so.entries) {
            w.u32(e.stream);
            w.str(e.semantic);
            w.u32(e.semanticIndex);
            w.u8(e.startComponent);
            w.u8(e.componentCount);
            w.u8(e.outputSlot);
        }
        w.u32(static_cast<uint32_t>(so.strides.size()));
        for (uint32_t v : so.strides) w.u32(v);
        w.u32(so.rasterizedStream);
        const auto& vi = s.viewInstancing;
        w.u32(static_cast<uint32_t>(vi.viewportIndex.size()));
        for (size_t i = 0; i < vi.viewportIndex.size(); ++i) {
            w.u32(vi.viewportIndex[i]);
            w.u32(i < vi.rtArrayIndex.size() ? vi.rtArrayIndex[i] : 0);
        }
        w.u32(vi.flags);
    }
}

PsoDesc PsoDesc::decode(ByteReader& r) {
    PsoDesc d;
    uint8_t api = r.u8();
    if (api > static_cast<uint8_t>(PsoApi::Stream)) throw FormatError("bad pso api");
    d.api = static_cast<PsoApi>(api);
    uint32_t n = r.u32();
    if (n > 64) throw FormatError("too many subobjects");
    d.subobjects.resize(n);
    for (auto& s : d.subobjects) {
        s.type = r.u32();
        s.blob = r.u64();
        s.pod = r.blob();
        uint32_t ne = r.u32();
        if (ne > 4096) throw FormatError("input layout too large");
        s.inputLayout.resize(ne);
        for (auto& e : s.inputLayout) {
            e.semantic = r.str();
            e.semanticIndex = r.u32();
            e.format = r.u32();
            e.inputSlot = r.u32();
            e.alignedByteOffset = r.u32();
            e.slotClass = r.u32();
            e.instanceStepRate = r.u32();
        }
        auto& so = s.streamOutput;
        uint32_t nso = r.u32();
        if (nso > 4096) throw FormatError("stream output too large");
        so.entries.resize(nso);
        for (auto& e : so.entries) {
            e.stream = r.u32();
            e.semantic = r.str();
            e.semanticIndex = r.u32();
            e.startComponent = r.u8();
            e.componentCount = r.u8();
            e.outputSlot = r.u8();
        }
        uint32_t nstr = r.u32();
        if (nstr > 64) throw FormatError("too many strides");
        so.strides.resize(nstr);
        for (auto& v : so.strides) v = r.u32();
        so.rasterizedStream = r.u32();
        auto& vi = s.viewInstancing;
        uint32_t nvi = r.u32();
        if (nvi > 64) throw FormatError("too many view instances");
        vi.viewportIndex.resize(nvi);
        vi.rtArrayIndex.resize(nvi);
        for (uint32_t i = 0; i < nvi; ++i) {
            vi.viewportIndex[i] = r.u32();
            vi.rtArrayIndex[i] = r.u32();
        }
        vi.flags = r.u32();
    }
    return d;
}

uint64_t PsoDesc::hash() const {
    // The api is deliberately excluded: the same pipeline created through a classic desc or a
    // stream should count as one entry for coverage purposes.
    ByteWriter w;
    PsoDesc copy = *this;
    copy.api = PsoApi::Stream;
    copy.encode(w);
    return hash64(w.data().data(), w.data().size());
}

const Subobject* PsoDesc::find(uint32_t type) const {
    for (const auto& s : subobjects)
        if (s.type == type) return &s;
    return nullptr;
}

Subobject* PsoDesc::find(uint32_t type) {
    for (auto& s : subobjects)
        if (s.type == type) return &s;
    return nullptr;
}

Subobject& PsoDesc::get_or_add(uint32_t type) {
    if (auto* s = find(type)) return *s;
    Subobject s;
    s.type = type;
    auto pos = std::upper_bound(subobjects.begin(), subobjects.end(), type,
                                [](uint32_t t, const Subobject& o) { return t < o.type; });
    return *subobjects.insert(pos, std::move(s));
}

uint64_t PsoDesc::shader(uint32_t type) const {
    const Subobject* s = find(type);
    return s ? s->blob : 0;
}

bool PsoDesc::is_compute() const { return shader(SO_CS) != 0; }

bool PsoDesc::is_graphics() const {
    return shader(SO_VS) != 0 || shader(SO_MS) != 0;
}

PsoDesc PsoDesc::without_shaders() const {
    PsoDesc d;
    d.api = api;
    for (const auto& s : subobjects)
        if (!is_shader_subobject(s.type)) d.subobjects.push_back(s);
    return d;
}

uint64_t PsoDesc::style_hash() const { return without_shaders().hash(); }

uint32_t PsoDesc::num_render_targets() const {
    const Subobject* s = find(SO_RENDER_TARGET_FORMATS);
    if (!s || s->pod.size() < pod::kRtFormatArraySize) return 0;
    return load_u32(s->pod.data() + pod::kRtFormatArrayCountOffset);
}

uint32_t PsoDesc::render_target_format(uint32_t i) const {
    const Subobject* s = find(SO_RENDER_TARGET_FORMATS);
    if (!s || s->pod.size() < pod::kRtFormatArraySize || i >= 8) return 0;
    return load_u32(s->pod.data() + 4 * i);
}

uint32_t PsoDesc::depth_stencil_format() const {
    const Subobject* s = find(SO_DEPTH_STENCIL_FORMAT);
    if (!s || s->pod.size() < 4) return 0;
    return load_u32(s->pod.data());
}

}  // namespace sp
