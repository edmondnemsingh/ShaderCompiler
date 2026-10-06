// Candidate PSO inference.
//
// A harvested shader is useless on its own: the driver only reuses a compile when the *whole*
// pipeline description matches. Every game reuses a small set of root signatures, input layouts,
// render-target formats and blend/depth/raster states, so we learn that "style" from the PSOs the
// game really created (recorded at boot/menu, or imported from other players) and combine it with
// harvested shaders. Wrong combinations are cheap: the D3D12 runtime rejects shader/root-signature
// or input-layout mismatches before the driver compiles anything.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/psodb.hpp"
#include "dxbc/dxbc.hpp"

namespace sp {

struct InferOptions {
    uint32_t maxRootSigsPerCs = 8;
    uint32_t maxVsPerPs = 4;
    uint32_t maxTemplatesPerPair = 4;
    uint32_t maxCandidates = 200000;
};

struct InferStats {
    uint32_t shadersConsidered = 0;
    uint32_t computeExact = 0;      // CS with embedded root signature
    uint32_t computeGuessed = 0;    // CS x learned root signature
    uint32_t graphicsGuessed = 0;
    uint32_t pixelShadersNoVs = 0;  // no compatible vertex shader found
    uint32_t pixelShadersNoTemplate = 0;
    uint32_t skippedStages = 0;     // GS/HS/DS/MS/AS/lib: only covered by recordings
    bool truncated = false;
};

// Identity of a root signature by its RTS0 payload, so a standalone serialized root signature and
// one embedded in a shader compare equal. Returns 0 if the blob carries no root signature.
uint64_t root_signature_key(const Bytes& blob);

class StyleModel {
public:
    // Learns from every Recorded/Imported PSO in `db`.
    void learn(const PsoDb& db);

    struct Template {
        PsoDesc style;          // PSO without shaders
        uint32_t count = 0;     // distinct PSOs using this style
        uint32_t withPs = 0;    // ...of which had a pixel shader
        uint32_t withoutPs = 0; // ...depth-only / no pixel shader
    };
    const std::vector<Template>& graphics_templates() const { return graphics_; }
    const std::vector<Template>& compute_templates() const { return compute_; }
    // Graphics template indices sorted by count, most frequent first.
    const std::vector<size_t>& graphics_by_frequency() const { return graphicsOrder_; }

    // Graphics templates observed together with a given shader hash, most frequent first.
    std::vector<size_t> templates_for_shader(uint64_t shader) const;
    uint32_t shader_use_count(uint64_t shader) const;
    // A recorded root-signature blob whose RTS0 payload has the given key (0 if none).
    uint64_t root_sig_blob_for_key(uint64_t key) const;

private:
    static size_t intern(std::vector<Template>& list, std::unordered_map<uint64_t, size_t>& index,
                         const PsoDesc& style);
    std::vector<Template> graphics_, compute_;
    std::unordered_map<uint64_t, size_t> graphicsIndex_, computeIndex_;
    std::vector<size_t> graphicsOrder_;
    std::unordered_map<uint64_t, std::vector<std::pair<size_t, uint32_t>>> byShader_;
    std::unordered_map<uint64_t, uint64_t> rsByKey_;
};

// Adds Inferred candidate PSOs to `out` for every shader in `db` not already covered by a PSO.
// `out` receives the needed blobs too, so it can be saved and replayed on its own.
InferStats infer_candidates(const PsoDb& db, const StyleModel& style, PsoDb& out,
                            const InferOptions& opts = {});

// Signature helpers (exposed for tests).
bool vs_feeds_ps(const dxbc::ContainerInfo& vs, const dxbc::ContainerInfo& ps);
bool layout_covers_vs(const std::vector<InputElement>& layout, const dxbc::ContainerInfo& vs);
uint32_t ps_render_target_count(const dxbc::ContainerInfo& ps);

}  // namespace sp
