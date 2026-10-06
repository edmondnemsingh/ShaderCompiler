#include "win/warm.hpp"

#include <algorithm>

namespace sp::win {

Warmer::Warmer(ID3D12Device* device, uint32_t threads, bool lowPriority)
    : device_(device), lowPriority_(lowPriority) {
    device_->AddRef();
    if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency() - 1);
    for (uint32_t i = 0; i < threads; ++i) threads_.emplace_back([this] { worker(); });
}

Warmer::~Warmer() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
    rootSigs_.clear();
    device_->Release();
}

size_t Warmer::submit(std::shared_ptr<const PsoDb> db) {
    std::vector<size_t> order(db->psos().size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    // Exact pipelines first (recorded, imported), guesses last.
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return static_cast<int>(db->psos()[a].source) < static_cast<int>(db->psos()[b].source);
    });
    size_t added = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (size_t i : order) {
            if (!warmed_.insert(db->psos()[i].hash).second) continue;
            queue_.push_back({db, i});
            ++added;
        }
    }
    stats_.queued += added;
    cv_.notify_all();
    return added;
}

bool Warmer::already_warmed(uint64_t psoHash) {
    std::lock_guard<std::mutex> lock(mu_);
    return warmed_.count(psoHash) != 0;
}

bool Warmer::wait_idle(DWORD timeoutMs) {
    std::unique_lock<std::mutex> lock(mu_);
    return idle_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                          [&] { return queue_.empty() && active_ == 0; });
}

void Warmer::set_low_priority(bool low) { lowPriority_ = low; }

void Warmer::worker() {
    InternalScope internal;
    int currentPriority = THREAD_PRIORITY_NORMAL;
    for (;;) {
        Item item;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            item = std::move(queue_.front());
            queue_.pop_front();
            ++active_;
        }
        // Below-normal while the game is running so compiles never steal its main thread's time.
        int wanted = lowPriority_ ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL;
        if (wanted != currentPriority) {
            SetThreadPriority(GetCurrentThread(), wanted);
            currentPriority = wanted;
        }
        const PsoEntry& e = item.db->psos()[item.index];
        ULONGLONG t0 = GetTickCount64();
        HRESULT hr = replay(device_, e.desc, *item.db, rootSigs_);
        stats_.busyMs += GetTickCount64() - t0;
        if (SUCCEEDED(hr)) stats_.created++;
        else if (hr == E_INVALIDARG) stats_.rejected++;
        else stats_.failed++;
        stats_.done++;
        {
            std::lock_guard<std::mutex> lock(mu_);
            --active_;
            if (queue_.empty() && active_ == 0) idle_.notify_all();
        }
    }
}

}  // namespace sp::win
