// Background pipeline compilation ("warming"): creates PSOs on worker threads and releases them
// straight away. The point is the side effect: the driver stores each compiled pipeline in its
// on-disk shader cache, so when the game asks for the same pipeline later it is a cache hit
// instead of a mid-frame compile.
#pragma once

#include "win/d3d12.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "core/psodb.hpp"
#include "win/bridge.hpp"

namespace sp::win {

// Set while shaderprep itself calls D3D12, so the hooks don't record our own work.
inline thread_local bool t_internalCall = false;
struct InternalScope {
    bool prev;
    InternalScope() : prev(t_internalCall) { t_internalCall = true; }
    ~InternalScope() { t_internalCall = prev; }
};

struct WarmStats {
    std::atomic<uint64_t> queued{0};
    std::atomic<uint64_t> done{0};
    std::atomic<uint64_t> created{0};
    std::atomic<uint64_t> rejected{0};  // E_INVALIDARG: a wrong guess, rejected before compiling
    std::atomic<uint64_t> failed{0};
    std::atomic<uint64_t> busyMs{0};
};

class Warmer {
public:
    Warmer(ID3D12Device* device, uint32_t threads, bool lowPriority);
    ~Warmer();
    Warmer(const Warmer&) = delete;
    Warmer& operator=(const Warmer&) = delete;

    // Queues every PSO in `db` that has not been warmed yet (recorded/imported before inferred).
    size_t submit(std::shared_ptr<const PsoDb> db);
    bool already_warmed(uint64_t psoHash);
    // Waits until the queue is empty (or timeout). Returns true if idle.
    bool wait_idle(DWORD timeoutMs);
    void set_low_priority(bool low);
    const WarmStats& stats() const { return stats_; }

private:
    struct Item {
        std::shared_ptr<const PsoDb> db;
        size_t index;
    };
    void worker();

    ID3D12Device* device_;
    RootSigCache rootSigs_;
    std::vector<std::thread> threads_;
    std::mutex mu_;
    std::condition_variable cv_, idle_;
    std::deque<Item> queue_;
    std::unordered_set<uint64_t> warmed_;
    size_t active_ = 0;
    bool stop_ = false;
    std::atomic<bool> lowPriority_;
    WarmStats stats_;
};

}  // namespace sp::win
