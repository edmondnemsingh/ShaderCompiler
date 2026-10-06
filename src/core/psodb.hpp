// .psodb: the project's single interchange file. Holds a game's identity, a content-addressed blob
// store (shader bytecode + serialized root signatures) and a list of PSO descriptions.
// Recordings, scan results, inferred candidates and shared downloads all use this format, so they
// can be merged freely.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/bytes.hpp"
#include "core/pso.hpp"

namespace sp {

enum class BlobKind : uint8_t { Shader = 0, RootSignature = 1 };

// Where a blob was found. Lets reports show how much the generic disk/memory harvest contributes.
enum class BlobOrigin : uint8_t { Recorded = 0, Disk = 1, Memory = 2, Engine = 3 };
const char* origin_name(BlobOrigin o);

struct Blob {
    BlobKind kind = BlobKind::Shader;
    BlobOrigin origin = BlobOrigin::Recorded;
    Bytes data;
};

struct PsoEntry {
    PsoDesc desc;
    uint64_t hash = 0;
    PsoSource source = PsoSource::Recorded;
    uint32_t seen = 1;         // number of times observed (summed on merge)
    float maxCreateMs = 0.0f;  // slowest observed creation; >~2ms means it was a driver compile
};

struct GameIdentity {
    std::string exe;       // executable file name, lower-case
    uint64_t buildId = 0;  // changes when the game is patched (see identify_game)
    std::string label;     // human readable, free form
    bool operator==(const GameIdentity& o) const { return exe == o.exe && buildId == o.buildId; }
};

class PsoDb {
public:
    GameIdentity game;

    uint64_t add_blob(BlobKind kind, BlobOrigin origin, Bytes data);
    const Blob* blob(uint64_t hash) const;
    const std::unordered_map<uint64_t, Blob>& blobs() const { return blobs_; }

    // Returns true when the PSO was not already present.
    bool add_pso(PsoDesc desc, PsoSource source, float createMs = 0.0f, uint32_t seen = 1);
    bool has_pso(uint64_t hash) const { return index_.count(hash) != 0; }
    const PsoEntry* pso(uint64_t hash) const;
    const std::vector<PsoEntry>& psos() const { return psos_; }

    // Adds everything from `other`. When `as` is set, merged PSOs are re-tagged with that source.
    void merge(const PsoDb& other, const PsoSource* as = nullptr);

    // Blobs referenced by any PSO must exist; returns the number of PSOs dropped for missing blobs.
    size_t drop_dangling();

    Bytes serialize() const;
    static PsoDb deserialize(const Bytes& data);
    bool save(const std::string& path) const;
    static bool load(const std::string& path, PsoDb& out, std::string* error = nullptr);

    size_t count_blobs(BlobKind kind) const;

private:
    std::unordered_map<uint64_t, Blob> blobs_;
    std::vector<PsoEntry> psos_;
    std::unordered_map<uint64_t, size_t> index_;
};

// Identity of a game build from its executable: name + size + hash of the first and last MiB.
GameIdentity identify_game(const std::string& exePath);

}  // namespace sp
