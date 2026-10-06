#include "dxbc/dxbc.hpp"

#include <cstring>

namespace sp::dxbc {

namespace {

constexpr uint32_t kDXIL = fourcc('D', 'X', 'I', 'L');
constexpr uint32_t kSHEX = fourcc('S', 'H', 'E', 'X');
constexpr uint32_t kSHDR = fourcc('S', 'H', 'D', 'R');
constexpr uint32_t kRTS0 = fourcc('R', 'T', 'S', '0');
constexpr uint32_t kISGN = fourcc('I', 'S', 'G', 'N');
constexpr uint32_t kOSGN = fourcc('O', 'S', 'G', 'N');
constexpr uint32_t kOSG5 = fourcc('O', 'S', 'G', '5');
constexpr uint32_t kPCSG = fourcc('P', 'C', 'S', 'G');
constexpr uint32_t kISG1 = fourcc('I', 'S', 'G', '1');
constexpr uint32_t kOSG1 = fourcc('O', 'S', 'G', '1');
constexpr uint32_t kPSG1 = fourcc('P', 'S', 'G', '1');

// Part fourccs are always four ASCII letters/digits. Requiring that for every part (instead of a
// fixed allow-list) keeps false positives from random bytes low without rejecting shaders that
// carry part types newer than this code.
bool plausible_part(uint32_t f) {
    for (int i = 0; i < 4; ++i) {
        unsigned char c = static_cast<unsigned char>(f >> (8 * i));
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!ok) return false;
    }
    return true;
}

Stage stage_from_kind(uint32_t kind) {
    switch (kind) {
        case 0: return Stage::Pixel;
        case 1: return Stage::Vertex;
        case 2: return Stage::Geometry;
        case 3: return Stage::Hull;
        case 4: return Stage::Domain;
        case 5: return Stage::Compute;
        case 6: return Stage::Library;
        case 13: return Stage::Mesh;
        case 14: return Stage::Amplification;
        default: return Stage::Unknown;
    }
}

bool parse_signature(const uint8_t* part, size_t size, uint32_t fcc, std::vector<SigElement>& out) {
    if (size < 8) return false;
    uint32_t count = load_u32(part);
    uint32_t offset = load_u32(part + 4);
    size_t stride;
    bool hasStream = false, hasMinPrecision = false;
    if (fcc == kISG1 || fcc == kOSG1 || fcc == kPSG1) {
        stride = 32;
        hasStream = hasMinPrecision = true;
    } else if (fcc == kOSG5) {
        stride = 28;
        hasStream = true;
    } else {
        stride = 24;
    }
    if (count > 1024 || offset > size || (size - offset) / stride < count) return false;
    out.clear();
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = part + offset + i * stride;
        SigElement el;
        size_t o = 0;
        if (hasStream) { el.stream = load_u32(e); o = 4; }
        uint32_t nameOff = load_u32(e + o);
        el.semanticIndex = load_u32(e + o + 4);
        el.systemValue = load_u32(e + o + 8);
        el.componentType = load_u32(e + o + 12);
        el.reg = load_u32(e + o + 16);
        el.mask = e[o + 20];
        el.rwMask = e[o + 21];
        if (hasMinPrecision) el.minPrecision = load_u32(e + o + 24);
        if (nameOff >= size) return false;
        const char* name = reinterpret_cast<const char*>(part + nameOff);
        size_t maxLen = size - nameOff;
        size_t len = strnlen(name, maxLen);
        if (len == maxLen) return false;
        el.semantic.assign(name, len);
        out.push_back(std::move(el));
    }
    return true;
}

void put_u32(Bytes& b, uint32_t v) {
    uint8_t t[4];
    std::memcpy(t, &v, 4);
    b.insert(b.end(), t, t + 4);
}

}  // namespace

const char* stage_name(Stage s) {
    switch (s) {
        case Stage::Pixel: return "ps";
        case Stage::Vertex: return "vs";
        case Stage::Geometry: return "gs";
        case Stage::Hull: return "hs";
        case Stage::Domain: return "ds";
        case Stage::Compute: return "cs";
        case Stage::Library: return "lib";
        case Stage::Mesh: return "ms";
        case Stage::Amplification: return "as";
        case Stage::None: return "none";
        case Stage::Unknown: return "unknown";
    }
    return "unknown";
}

size_t probe(const uint8_t* p, size_t avail) {
    if (avail < kHeaderSize || load_u32(p) != kMagic) return 0;
    // Bytes 4..19 are the checksum (zero for unsigned DXIL); bytes 20..23 the version 1.0.
    if (load_u16(p + 20) != 1 || load_u16(p + 22) != 0) return 0;
    uint32_t size = load_u32(p + 24);
    uint32_t count = load_u32(p + 28);
    if (count == 0 || count > kMaxParts) return 0;
    if (size > kMaxContainerSize || size > avail) return 0;
    if (size < kHeaderSize + 4ull * count) return 0;
    bool hasPayload = false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t off = load_u32(p + kHeaderSize + 4 * i);
        if (off < kHeaderSize + 4ull * count || off > size - 8) return 0;
        uint32_t fcc = load_u32(p + off);
        uint32_t psize = load_u32(p + off + 4);
        if (psize > size - off - 8) return 0;
        if (!plausible_part(fcc)) return 0;
        if (fcc == kDXIL || fcc == kSHEX || fcc == kSHDR || fcc == kRTS0) hasPayload = true;
    }
    return hasPayload ? size : 0;
}

std::pair<const uint8_t*, size_t> find_part(const uint8_t* p, size_t size, uint32_t fcc) {
    if (size < kHeaderSize) return {nullptr, 0};
    uint32_t count = load_u32(p + 28);
    for (uint32_t i = 0; i < count && kHeaderSize + 4ull * (i + 1) <= size; ++i) {
        uint32_t off = load_u32(p + kHeaderSize + 4 * i);
        if (off + 8ull > size) continue;
        uint32_t psize = load_u32(p + off + 4);
        if (off + 8ull + psize > size) continue;
        if (load_u32(p + off) == fcc) return {p + off + 8, psize};
    }
    return {nullptr, 0};
}

bool parse(const uint8_t* p, size_t size, ContainerInfo& out, std::string* error) {
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        return false;
    };
    size_t total = probe(p, size);
    if (!total) return fail("not a valid DXBC container");
    out = ContainerInfo{};
    out.size = static_cast<uint32_t>(total);
    uint32_t count = load_u32(p + 28);
    bool program = false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t off = load_u32(p + kHeaderSize + 4 * i);
        uint32_t fcc = load_u32(p + off);
        uint32_t psize = load_u32(p + off + 4);
        const uint8_t* part = p + off + 8;
        out.partFourccs.push_back(fcc);
        if (fcc == kDXIL) {
            if (psize < 24) return fail("truncated DXIL part");
            uint32_t ver = load_u32(part);
            if (load_u32(part + 8) != kDXIL) return fail("bad DXIL bitcode header");
            out.dxil = true;
            out.stage = stage_from_kind(ver >> 16);
            out.smMajor = (ver >> 4) & 0xF;
            out.smMinor = ver & 0xF;
            program = true;
        } else if (fcc == kSHEX || fcc == kSHDR) {
            if (psize < 8) return fail("truncated SHEX part");
            if (program && out.dxil) continue;  // prefer the DXIL description
            uint32_t ver = load_u32(part);
            out.stage = stage_from_kind(ver >> 16);
            out.smMajor = (ver >> 4) & 0xF;
            out.smMinor = ver & 0xF;
            program = true;
        } else if (fcc == kRTS0) {
            out.hasRootSignature = true;
            out.rootSignaturePart.assign(part, part + psize);
        } else if (fcc == kISGN || fcc == kISG1) {
            if (!parse_signature(part, psize, fcc, out.inputs)) return fail("bad input signature");
        } else if (fcc == kOSGN || fcc == kOSG5 || fcc == kOSG1) {
            if (!parse_signature(part, psize, fcc, out.outputs)) return fail("bad output signature");
        } else if (fcc == kPCSG || fcc == kPSG1) {
            if (!parse_signature(part, psize, fcc, out.patchConstants))
                return fail("bad patch constant signature");
        }
    }
    if (program) {
        out.kind = ContainerKind::Shader;
    } else if (out.hasRootSignature) {
        out.kind = ContainerKind::RootSignature;
        out.stage = Stage::None;
    } else {
        out.kind = ContainerKind::Other;
    }
    return true;
}

Bytes build_container(const std::vector<std::pair<uint32_t, Bytes>>& parts) {
    Bytes out;
    put_u32(out, kMagic);
    out.insert(out.end(), 16, 0);  // checksum
    out.push_back(1); out.push_back(0); out.push_back(0); out.push_back(0);  // version 1.0
    size_t sizePos = out.size();
    put_u32(out, 0);
    put_u32(out, static_cast<uint32_t>(parts.size()));
    size_t tablePos = out.size();
    out.insert(out.end(), 4 * parts.size(), 0);
    for (size_t i = 0; i < parts.size(); ++i) {
        uint32_t off = static_cast<uint32_t>(out.size());
        std::memcpy(out.data() + tablePos + 4 * i, &off, 4);
        put_u32(out, parts[i].first);
        put_u32(out, static_cast<uint32_t>(parts[i].second.size()));
        out.insert(out.end(), parts[i].second.begin(), parts[i].second.end());
        while (out.size() % 4) out.push_back(0);
    }
    uint32_t total = static_cast<uint32_t>(out.size());
    std::memcpy(out.data() + sizePos, &total, 4);
    return out;
}

Bytes build_signature_part(const std::vector<SigElement>& elements, int version) {
    size_t stride = version == 1 ? 32 : 24;
    Bytes out;
    put_u32(out, static_cast<uint32_t>(elements.size()));
    put_u32(out, 8);
    size_t tableEnd = 8 + stride * elements.size();
    out.resize(tableEnd, 0);
    for (size_t i = 0; i < elements.size(); ++i) {
        const SigElement& el = elements[i];
        uint32_t nameOff = static_cast<uint32_t>(out.size());
        out.insert(out.end(), el.semantic.begin(), el.semantic.end());
        out.push_back(0);
        uint8_t* e = out.data() + 8 + i * stride;
        size_t o = 0;
        auto w = [&](size_t at, uint32_t v) { std::memcpy(e + at, &v, 4); };
        if (version == 1) { w(0, el.stream); o = 4; }
        w(o, nameOff);
        w(o + 4, el.semanticIndex);
        w(o + 8, el.systemValue);
        w(o + 12, el.componentType);
        w(o + 16, el.reg);
        e[o + 20] = el.mask;
        e[o + 21] = el.rwMask;
        if (version == 1) w(o + 24, el.minPrecision);
    }
    while (out.size() % 4) out.push_back(0);
    return out;
}

Bytes build_shex_part(Stage stage, uint32_t major, uint32_t minor) {
    Bytes out;
    put_u32(out, (uint32_t(stage) << 16) | (major << 4) | minor);
    put_u32(out, 2);  // length in dwords
    return out;
}

Bytes build_dxil_part(Stage stage, uint32_t major, uint32_t minor) {
    Bytes out;
    put_u32(out, (uint32_t(stage) << 16) | (major << 4) | minor);
    put_u32(out, 6);           // size in uint32 including this header
    put_u32(out, kDXIL);       // bitcode header magic
    put_u32(out, 0x100);       // DXIL version 1.0
    put_u32(out, 16);          // bitcode offset (from bitcode header)
    put_u32(out, 0);           // bitcode size
    return out;
}

}  // namespace sp::dxbc
