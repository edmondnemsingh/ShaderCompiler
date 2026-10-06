// In-process memory harvest (full mode only). Walks the game's committed, readable, private or
// mapped memory and feeds it to the Harvester. Most games decompress/decrypt their shader archives
// into memory at boot, so this finds shaders without knowing the engine's file formats.
#pragma once

#include <atomic>
#include <cstdint>

#include "harvest/harvest.hpp"

namespace sp::win {

struct MemScanStats {
    uint64_t regions = 0;
    uint64_t bytes = 0;
    uint64_t ms = 0;
};

MemScanStats scan_own_memory(Harvester& h, const std::atomic<bool>* stop = nullptr);

}  // namespace sp::win
