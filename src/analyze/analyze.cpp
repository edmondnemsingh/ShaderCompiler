#include "analyze/analyze.hpp"

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/hash.hpp"
#include "infer/infer.hpp"

namespace sp {

namespace {

// Shader set identity of a PSO (which shaders, in which stages).
uint64_t shader_key(const PsoDesc& d) {
    ByteWriter w;
    for (const auto& s : d.subobjects)
        if (is_shader_subobject(s.type)) {
            w.u32(s.type);
            w.u64(s.blob);
        }
    return hash64(w.data().data(), w.data().size());
}

// Canonical form for comparison: root signature blob hash replaced by its RTS0 key.
PsoDesc canonical(const PsoDesc& d, const PsoDb& db) {
    PsoDesc c = d;
    if (Subobject* rs = c.find(SO_ROOT_SIGNATURE)) {
        if (const Blob* b = db.blob(rs->blob))
            if (uint64_t key = root_signature_key(b->data)) rs->blob = key;
    }
    c.api = PsoApi::Stream;
    return c;
}

std::vector<uint32_t> differing_types(const PsoDesc& a, const PsoDesc& b) {
    std::vector<uint32_t> types;
    for (const auto& s : a.subobjects) types.push_back(s.type);
    for (const auto& s : b.subobjects) types.push_back(s.type);
    std::sort(types.begin(), types.end());
    types.erase(std::unique(types.begin(), types.end()), types.end());
    std::vector<uint32_t> diff;
    for (uint32_t t : types) {
        const Subobject* x = a.find(t);
        const Subobject* y = b.find(t);
        if (!x || !y || !(*x == *y)) diff.push_back(t);
    }
    return diff;
}

}  // namespace

CoverageReport analyze_coverage(const PsoDb& truth, const PsoDb& prepared, float slowMs) {
    CoverageReport r;
    std::unordered_set<uint64_t> preparedHashes;
    std::unordered_map<uint64_t, std::vector<PsoDesc>> preparedByShaders;
    for (const auto& e : prepared.psos()) {
        PsoDesc c = canonical(e.desc, prepared);
        preparedHashes.insert(c.hash());
        preparedByShaders[shader_key(c)].push_back(std::move(c));
    }

    for (const auto& e : truth.psos()) {
        r.truthPsos++;
        bool slow = e.maxCreateMs >= slowMs;
        if (slow) r.slowTruth++;
        PsoDesc c = canonical(e.desc, truth);
        if (preparedHashes.count(c.hash())) {
            r.exactHits++;
            if (slow) r.slowCovered++;
            continue;
        }
        bool shadersFound = true;
        for (const auto& s : c.subobjects)
            if (is_shader_subobject(s.type) && s.blob && !prepared.blob(s.blob)) shadersFound = false;
        if (!shadersFound) {
            r.missShaderNotFound++;
            continue;
        }
        auto it = preparedByShaders.find(shader_key(c));
        if (it == preparedByShaders.end()) {
            r.missNoPairing++;
            continue;
        }
        r.missStateMismatch++;
        std::vector<uint32_t> best;
        bool first = true;
        for (const auto& cand : it->second) {
            auto diff = differing_types(c, cand);
            if (first || diff.size() < best.size()) {
                best = std::move(diff);
                first = false;
            }
        }
        for (uint32_t t : best) r.mismatchFields[subobject_name(t)]++;
    }
    return r;
}

std::string CoverageReport::to_string() const {
    char buf[512];
    std::string s;
    std::snprintf(buf, sizeof(buf),
                  "PSOs the game created:     %u\n"
                  "Pre-compiled exactly:      %u (%.1f%%)\n"
                  "Slow creations (stutter):  %u, of which pre-compiled: %u\n"
                  "Misses:\n"
                  "  shader never found:      %u\n"
                  "  shaders found, unpaired: %u\n"
                  "  state guessed wrong:     %u\n",
                  truthPsos, exactHits, 100.0 * hit_rate(), slowTruth, slowCovered, missShaderNotFound,
                  missNoPairing, missStateMismatch);
    s += buf;
    if (!mismatchFields.empty()) {
        s += "  fields that differed in the closest guess:\n";
        std::vector<std::pair<std::string, uint32_t>> v(mismatchFields.begin(), mismatchFields.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        for (auto& [name, n] : v) {
            std::snprintf(buf, sizeof(buf), "    %-22s %u\n", name.c_str(), n);
            s += buf;
        }
    }
    return s;
}

}  // namespace sp
