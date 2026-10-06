#include "win/memscan.hpp"

#include <windows.h>

namespace sp::win {

namespace {

bool scannable(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT) return false;
    // Executable images hold code, not shader data; skip them.
    if (mbi.Type != MEM_PRIVATE && mbi.Type != MEM_MAPPED) return false;
    DWORD p = mbi.Protect;
    if (p & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    // Write-combined / uncached memory is typically a GPU upload heap: reading it from the CPU is
    // extremely slow and it never holds shader containers.
    if (p & (PAGE_WRITECOMBINE | PAGE_NOCACHE)) return false;
    DWORD base = p & 0xFF;
    return base == PAGE_READONLY || base == PAGE_READWRITE || base == PAGE_WRITECOPY ||
           base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE;
}

}  // namespace

MemScanStats scan_own_memory(Harvester& h, const std::atomic<bool>* stop) {
    MemScanStats st;
    ULONGLONG t0 = GetTickCount64();
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    auto* addr = static_cast<uint8_t*>(si.lpMinimumApplicationAddress);
    auto* end = static_cast<uint8_t*>(si.lpMaximumApplicationAddress);
    HANDLE self = GetCurrentProcess();
    while (addr < end) {
        if (stop && stop->load()) break;
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        auto* regionBase = static_cast<uint8_t*>(mbi.BaseAddress);
        size_t regionSize = mbi.RegionSize;
        if (scannable(mbi)) {
            st.regions++;
            st.bytes += regionSize;
            // ReadProcessMemory on our own process instead of dereferencing: memory freed or
            // re-protected by the game while we scan makes the read fail instead of crashing.
            h.scan_stream(regionSize, [&](uint64_t off, uint8_t* dst, size_t n) -> size_t {
                SIZE_T got = 0;
                if (!ReadProcessMemory(self, regionBase + off, dst, n, &got)) return static_cast<size_t>(got);
                return static_cast<size_t>(got);
            }, 8u << 20);
        }
        addr = regionBase + regionSize;
    }
    st.ms = GetTickCount64() - t0;
    return st;
}

}  // namespace sp::win
