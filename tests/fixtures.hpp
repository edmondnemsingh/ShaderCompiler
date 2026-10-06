// Synthetic shader containers for tests. Real compilers are not available on CI Linux, so tests
// build structurally valid containers with the dxbc builders.
#pragma once

#include <string>
#include <vector>

#include "core/pod_layout.hpp"
#include "core/pso.hpp"
#include "dxbc/dxbc.hpp"

namespace fx {

using sp::dxbc::SigElement;
using sp::dxbc::Stage;

inline SigElement el(const std::string& name, uint32_t index, uint32_t reg, uint32_t sv = 0, uint32_t type = 3) {
    SigElement e;
    e.semantic = name;
    e.semanticIndex = index;
    e.reg = reg;
    e.systemValue = sv;
    e.componentType = type;
    e.mask = 0xF;
    return e;
}

// `salt` makes otherwise identical shaders distinct blobs.
inline sp::Bytes shader(Stage stage, const std::vector<SigElement>& in, const std::vector<SigElement>& out,
                        bool dxil, uint32_t salt, const sp::Bytes* rts0 = nullptr) {
    std::vector<std::pair<uint32_t, sp::Bytes>> parts;
    int sigVersion = dxil ? 1 : 0;
    parts.emplace_back(sp::fourcc('I', 'S', 'G', dxil ? '1' : 'N'), sp::dxbc::build_signature_part(in, sigVersion));
    parts.emplace_back(sp::fourcc('O', 'S', 'G', dxil ? '1' : 'N'), sp::dxbc::build_signature_part(out, sigVersion));
    if (rts0) parts.emplace_back(sp::fourcc('R', 'T', 'S', '0'), *rts0);
    sp::Bytes stat(8, 0);
    stat[0] = static_cast<uint8_t>(salt);
    stat[1] = static_cast<uint8_t>(salt >> 8);
    stat[2] = static_cast<uint8_t>(salt >> 16);
    parts.emplace_back(sp::fourcc('S', 'T', 'A', 'T'), stat);
    if (dxil)
        parts.emplace_back(sp::fourcc('D', 'X', 'I', 'L'), sp::dxbc::build_dxil_part(stage, 6, 0));
    else
        parts.emplace_back(sp::fourcc('S', 'H', 'E', 'X'), sp::dxbc::build_shex_part(stage, 5, 1));
    return sp::dxbc::build_container(parts);
}

inline sp::Bytes root_sig(uint32_t salt) {
    sp::Bytes rts0(24, 0);
    rts0[0] = 2;  // version 1.1
    rts0[4] = static_cast<uint8_t>(salt);
    return sp::dxbc::build_container({{sp::fourcc('R', 'T', 'S', '0'), rts0}});
}

inline sp::Bytes rts0_payload(uint32_t salt) {
    sp::Bytes rts0(24, 0);
    rts0[0] = 2;
    rts0[4] = static_cast<uint8_t>(salt);
    return rts0;
}

inline sp::Bytes u32_pod(uint32_t v) {
    sp::Bytes b(4);
    std::memcpy(b.data(), &v, 4);
    return b;
}

inline sp::Bytes rt_formats(std::vector<uint32_t> formats) {
    sp::Bytes b(sp::pod::kRtFormatArraySize, 0);
    for (size_t i = 0; i < formats.size() && i < 8; ++i) std::memcpy(b.data() + 4 * i, &formats[i], 4);
    uint32_t n = static_cast<uint32_t>(formats.size());
    std::memcpy(b.data() + sp::pod::kRtFormatArrayCountOffset, &n, 4);
    return b;
}

inline sp::InputElement ie(const std::string& name, uint32_t index, uint32_t format, uint32_t offset) {
    sp::InputElement e;
    e.semantic = name;
    e.semanticIndex = index;
    e.format = format;
    e.alignedByteOffset = offset;
    return e;
}

// A graphics PSO the way the Windows bridge records a classic desc (subset of fields is enough).
inline sp::PsoDesc graphics_pso(uint64_t rs, uint64_t vs, uint64_t ps, std::vector<sp::InputElement> layout,
                                std::vector<uint32_t> rts, uint32_t blendSalt = 0) {
    sp::PsoDesc d;
    d.api = sp::PsoApi::Graphics;
    d.get_or_add(sp::SO_ROOT_SIGNATURE).blob = rs;
    d.get_or_add(sp::SO_VS).blob = vs;
    if (ps) d.get_or_add(sp::SO_PS).blob = ps;
    d.get_or_add(sp::SO_INPUT_LAYOUT).inputLayout = std::move(layout);
    d.get_or_add(sp::SO_RENDER_TARGET_FORMATS).pod = rt_formats(rts);
    d.get_or_add(sp::SO_DEPTH_STENCIL_FORMAT).pod = u32_pod(sp::pod::DXGI_FORMAT_D32_FLOAT);
    sp::Bytes blend(sp::pod::kBlendDescSize, 0);
    blend[8] = static_cast<uint8_t>(blendSalt);
    d.get_or_add(sp::SO_BLEND).pod = blend;
    d.get_or_add(sp::SO_PRIMITIVE_TOPOLOGY).pod = u32_pod(sp::pod::kTopologyTriangle);
    return d;
}

inline sp::PsoDesc compute_pso(uint64_t rs, uint64_t cs) {
    sp::PsoDesc d;
    d.api = sp::PsoApi::Compute;
    d.get_or_add(sp::SO_ROOT_SIGNATURE).blob = rs;
    d.get_or_add(sp::SO_CS).blob = cs;
    d.get_or_add(sp::SO_NODE_MASK).pod = u32_pod(0);
    d.get_or_add(sp::SO_FLAGS).pod = u32_pod(0);
    return d;
}

}  // namespace fx
