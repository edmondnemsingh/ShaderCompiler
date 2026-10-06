// Portable parser for the "DXBC" container that wraps every D3D12 shader (both legacy DXBC
// bytecode and DXIL) and every serialized root signature. This is what makes discovery engine
// agnostic: whatever archive format a game uses, shaders end up as these containers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/bytes.hpp"

namespace sp::dxbc {

constexpr uint32_t kMagic = fourcc('D', 'X', 'B', 'C');
constexpr size_t kHeaderSize = 32;
constexpr size_t kMaxContainerSize = 64u << 20;
constexpr uint32_t kMaxParts = 64;

// Values match DXIL::ShaderKind / the D3D10 program type token.
enum class Stage : uint8_t {
    Pixel = 0,
    Vertex = 1,
    Geometry = 2,
    Hull = 3,
    Domain = 4,
    Compute = 5,
    Library = 6,
    Mesh = 13,
    Amplification = 14,
    None = 254,  // container without a program (e.g. a standalone root signature)
    Unknown = 255,
};
const char* stage_name(Stage s);

struct SigElement {
    std::string semantic;
    uint32_t semanticIndex = 0;
    uint32_t systemValue = 0;    // D3D_NAME
    uint32_t componentType = 0;  // D3D_REGISTER_COMPONENT_TYPE: 1 uint, 2 sint, 3 float
    uint32_t reg = 0;
    uint8_t mask = 0;
    uint8_t rwMask = 0;
    uint32_t stream = 0;
    uint32_t minPrecision = 0;
};

enum class ContainerKind : uint8_t { Shader, RootSignature, Other };

struct ContainerInfo {
    ContainerKind kind = ContainerKind::Other;
    Stage stage = Stage::Unknown;
    bool dxil = false;
    uint32_t smMajor = 0, smMinor = 0;
    uint32_t size = 0;
    std::vector<SigElement> inputs;
    std::vector<SigElement> outputs;
    std::vector<SigElement> patchConstants;
    bool hasRootSignature = false;
    Bytes rootSignaturePart;  // raw RTS0 part payload (version + params...), if present
    std::vector<uint32_t> partFourccs;
};

// Validates the container header + part table at `p`. Returns the container's total size, or 0
// if this is not a structurally valid container. Never reads beyond `avail` bytes.
size_t probe(const uint8_t* p, size_t avail);

// Full parse of a container already known to be valid (probe() != 0).
bool parse(const uint8_t* p, size_t size, ContainerInfo& out, std::string* error = nullptr);

// Returns the payload of the first part with the given fourcc, or {nullptr, 0}.
std::pair<const uint8_t*, size_t> find_part(const uint8_t* p, size_t size, uint32_t fourcc);

// Builders (used by tests and to wrap parts into containers).
Bytes build_container(const std::vector<std::pair<uint32_t, Bytes>>& parts);
// version 0 = ISGN/OSGN style (24-byte elements), 1 = ISG1/OSG1/PSG1 style (32-byte elements)
Bytes build_signature_part(const std::vector<SigElement>& elements, int version);
// SHEX token stream with only a version token and length (enough for stage detection).
Bytes build_shex_part(Stage stage, uint32_t major, uint32_t minor);
// DXIL program header with an empty bitcode payload.
Bytes build_dxil_part(Stage stage, uint32_t major, uint32_t minor);

// Common D3D_NAME values.
constexpr uint32_t D3D_NAME_UNDEFINED = 0;
constexpr uint32_t D3D_NAME_POSITION = 1;
constexpr uint32_t D3D_NAME_VERTEX_ID = 6;
constexpr uint32_t D3D_NAME_PRIMITIVE_ID = 7;
constexpr uint32_t D3D_NAME_INSTANCE_ID = 8;
constexpr uint32_t D3D_NAME_IS_FRONT_FACE = 9;
constexpr uint32_t D3D_NAME_SAMPLE_INDEX = 10;
constexpr uint32_t D3D_NAME_TARGET = 64;
constexpr uint32_t D3D_NAME_DEPTH = 65;
constexpr uint32_t D3D_NAME_COVERAGE = 66;

}  // namespace sp::dxbc
