// Engine-agnostic shader discovery: finds DXBC/DXIL containers in arbitrary bytes (files on disk,
// or the game's own process memory after it has unpacked its archives) and adds them to a PsoDb.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/psodb.hpp"

namespace sp {

struct HarvestStats {
    uint64_t files = 0;
    uint64_t bytesScanned = 0;
    uint64_t containers = 0;      // valid containers seen (including duplicates)
    uint64_t newShaders = 0;
    uint64_t newRootSignatures = 0;
    uint64_t duplicates = 0;
};

struct DiskScanOptions {
    uint64_t maxFileSize = 0;               // 0 = unlimited
    std::vector<std::string> skipExtensions = {
        // Media and audio never contain shaders; skipping them saves most of the I/O.
        ".mp4", ".webm", ".bk2", ".bik", ".usm", ".wem", ".bnk", ".ogg", ".wav", ".mp3",
        ".opus", ".avi", ".mkv", ".wmv", ".png", ".jpg", ".jpeg",
    };
    std::function<void(const std::string& file, uint64_t done, uint64_t total)> progress;
};

class Harvester {
public:
    Harvester(PsoDb& db, BlobOrigin origin) : db_(db), origin_(origin) {}

    // Scans `n` bytes. Containers that straddle the end are not reported; returns the offset of the
    // first such truncated header (or n), so streaming callers can resume from there.
    size_t scan_buffer(const uint8_t* p, size_t n);

    bool scan_file(const std::string& path);
    void scan_directory(const std::string& dir, const DiskScanOptions& opts = {});

    const HarvestStats& stats() const { return stats_; }

private:
    void add_container(const uint8_t* p, size_t size);

    PsoDb& db_;
    BlobOrigin origin_;
    HarvestStats stats_;
    std::unordered_set<uint64_t> seen_;
};

}  // namespace sp
