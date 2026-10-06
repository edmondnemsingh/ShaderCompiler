// Portable, pointer-free model of a D3D12 pipeline state description.
//
// Every PSO the game creates (classic graphics/compute desc or pipeline-state stream) is converted
// to a list of subobjects keyed by D3D12_PIPELINE_STATE_SUBOBJECT_TYPE. Pointers are externalised:
// shader bytecode and root signatures become blob hashes (bytes live in the PsoDb blob store),
// input layouts / stream output / view instancing become owned vectors. Plain-old-data state
// (blend, rasterizer, formats...) is kept as the raw Windows x64 struct bytes, so the Windows side
// can memcpy it straight back without this code needing to understand every field.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/bytes.hpp"

namespace sp {

// Values match D3D12_PIPELINE_STATE_SUBOBJECT_TYPE.
enum SubobjectType : uint32_t {
    SO_ROOT_SIGNATURE = 0,
    SO_VS = 1,
    SO_PS = 2,
    SO_DS = 3,
    SO_HS = 4,
    SO_GS = 5,
    SO_CS = 6,
    SO_STREAM_OUTPUT = 7,
    SO_BLEND = 8,
    SO_SAMPLE_MASK = 9,
    SO_RASTERIZER = 10,
    SO_DEPTH_STENCIL = 11,
    SO_INPUT_LAYOUT = 12,
    SO_IB_STRIP_CUT_VALUE = 13,
    SO_PRIMITIVE_TOPOLOGY = 14,
    SO_RENDER_TARGET_FORMATS = 15,
    SO_DEPTH_STENCIL_FORMAT = 16,
    SO_SAMPLE_DESC = 17,
    SO_NODE_MASK = 18,
    SO_CACHED_PSO = 19,
    SO_FLAGS = 20,
    SO_DEPTH_STENCIL1 = 21,
    SO_VIEW_INSTANCING = 22,
    SO_AS = 24,
    SO_MS = 25,
    SO_DEPTH_STENCIL2 = 26,
    SO_RASTERIZER1 = 27,
    SO_RASTERIZER2 = 28,
    SO_SERIALIZED_ROOT_SIGNATURE = 29,
};

bool is_shader_subobject(uint32_t type);

// Zeroes struct padding inside a POD payload so identical states always hash identically.
void sanitize_pod(uint32_t type, Bytes& pod);
const char* subobject_name(uint32_t type);

// How the game originally created the PSO; replay uses the same entry point.
enum class PsoApi : uint8_t { Graphics = 0, Compute = 1, Stream = 2 };

// Where an entry came from. Ordered by trust: recorded > imported > learned/inferred.
enum class PsoSource : uint8_t { Recorded = 0, Imported = 1, Inferred = 2, Engine = 3 };
const char* source_name(PsoSource s);

struct InputElement {
    std::string semantic;
    uint32_t semanticIndex = 0;
    uint32_t format = 0;  // DXGI_FORMAT
    uint32_t inputSlot = 0;
    uint32_t alignedByteOffset = 0;
    uint32_t slotClass = 0;  // D3D12_INPUT_CLASSIFICATION
    uint32_t instanceStepRate = 0;
    bool operator==(const InputElement&) const = default;
};

struct SoDeclEntry {
    uint32_t stream = 0;
    std::string semantic;  // empty == gap (null SemanticName)
    uint32_t semanticIndex = 0;
    uint8_t startComponent = 0;
    uint8_t componentCount = 0;
    uint8_t outputSlot = 0;
    bool operator==(const SoDeclEntry&) const = default;
};

struct StreamOutput {
    std::vector<SoDeclEntry> entries;
    std::vector<uint32_t> strides;
    uint32_t rasterizedStream = 0;
    bool operator==(const StreamOutput&) const = default;
};

struct ViewInstancing {
    std::vector<uint32_t> viewportIndex;
    std::vector<uint32_t> rtArrayIndex;
    uint32_t flags = 0;
    bool operator==(const ViewInstancing&) const = default;
};

struct Subobject {
    uint32_t type = 0;
    uint64_t blob = 0;  // shaders, root signatures (0 == null)
    Bytes pod;          // raw x64 struct bytes for POD subobjects
    std::vector<InputElement> inputLayout;
    StreamOutput streamOutput;
    ViewInstancing viewInstancing;
    bool operator==(const Subobject&) const = default;
};

struct PsoDesc {
    PsoApi api = PsoApi::Stream;
    std::vector<Subobject> subobjects;  // kept sorted by type, at most one of each
    bool operator==(const PsoDesc&) const = default;

    // Canonicalises (sorts, drops CACHED_PSO) - call after building a desc by hand.
    void normalize();
    uint64_t hash() const;
    void encode(ByteWriter& w) const;
    static PsoDesc decode(ByteReader& r);

    const Subobject* find(uint32_t type) const;
    Subobject* find(uint32_t type);
    Subobject& get_or_add(uint32_t type);
    uint64_t shader(uint32_t type) const;  // blob hash or 0
    bool is_compute() const;
    bool is_graphics() const;

    // Same desc with every shader subobject removed: the "style" of the pipeline.
    PsoDesc without_shaders() const;
    uint64_t style_hash() const;

    // Convenience accessors over the POD payloads (Windows x64 layout, see pod_layout.hpp).
    uint32_t num_render_targets() const;
    uint32_t render_target_format(uint32_t i) const;
    uint32_t depth_stencil_format() const;
};

}  // namespace sp
