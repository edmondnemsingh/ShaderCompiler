// Measures how well a set of pre-compiled PSOs (scan + inference + imports) covered what a game
// actually created, and explains each miss so the inference heuristics can be tuned.
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "core/psodb.hpp"

namespace sp {

struct CoverageReport {
    uint32_t truthPsos = 0;
    uint32_t exactHits = 0;
    uint32_t slowTruth = 0;      // PSOs whose creation took >= slowMs (real stutter events)
    uint32_t slowCovered = 0;
    uint32_t missShaderNotFound = 0;  // some shader of the PSO was never harvested
    uint32_t missNoPairing = 0;       // shaders found but never combined into a candidate
    uint32_t missStateMismatch = 0;   // a candidate with the same shaders differed in state
    // For state mismatches: which subobject differed in the closest candidate, and how often.
    std::map<std::string, uint32_t> mismatchFields;

    double hit_rate() const { return truthPsos ? double(exactHits) / truthPsos : 0.0; }
    std::string to_string() const;
};

// `truth`: a recording of what the game created. `prepared`: what was pre-compiled.
// Root signatures are compared by their RTS0 payload, not by blob hash.
CoverageReport analyze_coverage(const PsoDb& truth, const PsoDb& prepared, float slowMs = 2.0f);

}  // namespace sp
