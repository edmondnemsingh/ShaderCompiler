// Full-mode runtime inside the game process:
//   1. RECORD every pipeline the game creates (and how long the driver took: >2 ms = stutter).
//   2. WARM: compile everything known (recordings, imports, disk-scan candidates) on background
//      threads as soon as the device exists, optionally holding the game at boot until done.
//   3. HARVEST: a few times during the first minutes, scan the game's memory for shaders it has
//      unpacked, learn its pipeline "style" from what it has created so far, infer candidate
//      pipelines for every new shader and warm those too.
#include "hook/hooks.hpp"

#include "win/d3d12.hpp"
#include <dxgi1_4.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "core/hash.hpp"
#include "core/psodb.hpp"
#include "harvest/harvest.hpp"
#include "infer/infer.hpp"
#include "profile/profile.hpp"
#include "win/bridge.hpp"
#include "win/log.hpp"
#include "win/memscan.hpp"
#include "win/vtable_index.h"
#include "win/warm.hpp"

namespace sp::hook {

using namespace sp::win;
namespace fs = std::filesystem;

namespace {

HMODULE g_self = nullptr;

// ---------------------------------------------------------------------------------------------
// vtable patching. Each COM class has one vtable shared by all its objects; a patched slot points
// at our hook, which looks up the original for the vtable of the object it was called on (a
// debug-layer device and a plain device have different vtables and different originals).

struct SlotOriginal {
    void** vtbl;
    int slot;
    void* original;
};
constexpr int kMaxOriginals = 128;
SlotOriginal g_originals[kMaxOriginals];
std::atomic<int> g_originalCount{0};
std::mutex g_patchMu;

void* find_original(void* object, int slot) {
    void** vtbl = *static_cast<void***>(object);
    int n = g_originalCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (g_originals[i].vtbl == vtbl && g_originals[i].slot == slot) return g_originals[i].original;
    // Object whose vtable we never patched but that reached our hook: impossible unless another
    // tool copied our pointer into its own vtable. Fall back to whatever is in the slot... which
    // would be us; so report failure instead.
    return nullptr;
}

bool patch(void* object, int slot, void* hook) {
    std::lock_guard<std::mutex> lock(g_patchMu);
    void** vtbl = *static_cast<void***>(object);
    int n = g_originalCount.load();
    for (int i = 0; i < n; ++i)
        if (g_originals[i].vtbl == vtbl && g_originals[i].slot == slot) return true;
    if (n >= kMaxOriginals) return false;
    if (vtbl[slot] == hook) return true;
    DWORD old;
    if (!VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    g_originals[n] = {vtbl, slot, vtbl[slot]};
    g_originalCount.store(n + 1, std::memory_order_release);
    vtbl[slot] = hook;
    VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), &vtbl[slot], sizeof(void*));
    return true;
}

template <typename F>
F original(void* object, int slot) {
    return reinterpret_cast<F>(find_original(object, slot));
}

double now_ms() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return 1000.0 * double(t.QuadPart) / double(freq.QuadPart);
}

// ---------------------------------------------------------------------------------------------

struct Runtime {
    std::once_flag initOnce;
    bool ok = false;
    fs::path dataDir;
    Profile profile;

    std::mutex mu;  // guards everything below that is not atomic
    PsoDb recorded;
    PsoDb harvested;
    bool dirty = false;
    std::unordered_map<ID3D12RootSignature*, uint64_t> rootSigs;
    std::shared_ptr<PsoDb> base;  // everything known at startup

    std::unique_ptr<Warmer> warmer;
    LUID warmLuid{};
    std::atomic<bool> harvestStarted{false};

    std::atomic<uint64_t> created{0}, slow{0}, slowWarmed{0}, captureFailures{0};
    std::atomic<uint64_t> stallMs{0};
};

Runtime& rt() {
    static Runtime* r = new Runtime();  // intentionally leaked: never torn down during process exit
    return *r;
}

fs::path data_path(const char* name) { return rt().dataDir / name; }

bool load_db(const fs::path& p, PsoDb& out) {
    if (!fs::exists(p)) return false;
    std::string err;
    if (!PsoDb::load(path_u8(p), out, &err)) {
        logf("warning: %s", err.c_str());
        return false;
    }
    return true;
}

void init_runtime() {
    Runtime& r = rt();
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(g_self, buf, MAX_PATH);
    r.dataDir = fs::path(buf).parent_path() / L"shaderprep";
    std::error_code ec;
    fs::create_directories(r.dataDir, ec);
    log_open((r.dataDir / L"log.txt").wstring());
    logf("---- shaderprep proxy loaded (pid %lu) ----", GetCurrentProcessId());

    std::string err;
    if (fs::exists(data_path("profile.toml")) && !load_profile(path_u8(data_path("profile.toml")), r.profile, &err))
        logf("warning: profile: %s (using defaults)", err.c_str());

    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    GameIdentity game = identify_game(narrow(buf));
    logf("game: %s build %s", game.exe.c_str(), hex64(game.buildId).c_str());

    // Recordings are per game build: a patch changes shaders, so start a new file and keep the old.
    if (load_db(data_path("recorded.psodb"), r.recorded) && !(r.recorded.game == game)) {
        std::string old = "recorded-" + hex64(r.recorded.game.buildId) + ".psodb";
        fs::rename(data_path("recorded.psodb"), data_path(old.c_str()), ec);
        logf("game build changed; previous recording kept as %s", old.c_str());
        r.recorded = PsoDb();
    }
    r.recorded.game = game;
    load_db(data_path("harvest.psodb"), r.harvested);
    r.harvested.game = game;

    auto base = std::make_shared<PsoDb>();
    base->game = game;
    PsoDb tmp;
    if (load_db(data_path("disk.psodb"), tmp)) base->merge(tmp);
    base->merge(r.recorded);
    base->merge(r.harvested);
    PsoSource imported = PsoSource::Imported;
    if (fs::exists(r.dataDir / "import", ec)) {
        for (auto& e : fs::directory_iterator(r.dataDir / "import", ec)) {
            PsoDb imp;
            if (e.path().extension() == ".psodb" && load_db(e.path(), imp)) {
                if (!imp.game.exe.empty() && !(imp.game == game))
                    logf("warning: %s is for a different game build; using it anyway", path_u8(e.path().filename()).c_str());
                base->merge(imp, &imported);
            }
        }
    }
    base->drop_dangling();
    StyleModel style;
    style.learn(*base);
    PsoDb cands;
    InferStats st = infer_candidates(*base, style, cands, r.profile.infer);
    logf("startup: %zu known PSOs, %zu shaders known; inferred %zu candidates (compute exact %u, guessed %u, graphics %u)",
         base->psos().size(), base->count_blobs(BlobKind::Shader), cands.psos().size(), st.computeExact,
         st.computeGuessed, st.graphicsGuessed);
    base->merge(cands);
    r.base = std::move(base);
    r.ok = true;
}

// --- recording ----------------------------------------------------------------------------------

BlobSink recorded_sink() {
    return [](BlobKind kind, const void* p, size_t n) {
        auto* b = static_cast<const uint8_t*>(p);
        return rt().recorded.add_blob(kind, BlobOrigin::Recorded, Bytes(b, b + n));
    };
}

RootSigLookup root_sig_lookup() {
    return [](ID3D12RootSignature* rs) -> uint64_t {
        auto it = rt().rootSigs.find(rs);
        return it == rt().rootSigs.end() ? 0 : it->second;
    };
}

template <typename CaptureFn>
void record(double ms, CaptureFn capture) {
    Runtime& r = rt();
    if (!r.ok) return;
    r.created++;
    bool isSlow = ms >= r.profile.slowMs;
    if (isSlow) {
        r.slow++;
        r.stallMs += static_cast<uint64_t>(ms);
    }
    if (!r.profile.record && !isSlow) return;
    std::lock_guard<std::mutex> lock(r.mu);
    PsoDesc d;
    std::string why;
    if (!capture(d, why)) {
        if (r.captureFailures++ < 10) logf("could not record a pipeline: %s", why.c_str());
        return;
    }
    uint64_t h = d.hash();
    if (isSlow) {
        bool warmed = r.warmer && r.warmer->already_warmed(h);
        if (warmed) r.slowWarmed++;
        logf("slow pipeline %.1f ms (%s)%s", ms, d.is_compute() ? "compute" : "graphics",
             warmed ? " - was pre-compiled, driver cache missed" : "");
    }
    if (r.profile.record) {
        r.recorded.add_pso(std::move(d), PsoSource::Recorded, static_cast<float>(ms));
        r.dirty = true;
    }
}

using PFN_CreateGraphics = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using PFN_CreateCompute = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
using PFN_CreateStream = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
using PFN_CreateRootSig = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
using PFN_CreateLibrary = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device1*, const void*, SIZE_T, REFIID, void**);
using PFN_LoadGraphics = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary*, LPCWSTR, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using PFN_LoadCompute = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary*, LPCWSTR, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
using PFN_LoadPipeline = HRESULT(STDMETHODCALLTYPE*)(ID3D12PipelineLibrary1*, LPCWSTR, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
using PFN_FactoryCreateDevice = HRESULT(STDMETHODCALLTYPE*)(ID3D12DeviceFactory*, IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

HRESULT STDMETHODCALLTYPE hook_create_graphics(ID3D12Device* self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
                                               REFIID riid, void** out) {
    auto orig = original<PFN_CreateGraphics>(self, SP_VT_DEVICE_CREATE_GRAPHICS_PSO);
    if (!orig) return E_FAIL;
    if (t_internalCall || !desc) return orig(self, desc, riid, out);
    double t0 = now_ms();
    HRESULT hr = orig(self, desc, riid, out);
    double ms = now_ms() - t0;
    if (SUCCEEDED(hr) && out && *out)
        record(ms, [&](PsoDesc& d, std::string& why) { return capture_graphics(*desc, recorded_sink(), root_sig_lookup(), d, &why); });
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_compute(ID3D12Device* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                              REFIID riid, void** out) {
    auto orig = original<PFN_CreateCompute>(self, SP_VT_DEVICE_CREATE_COMPUTE_PSO);
    if (!orig) return E_FAIL;
    if (t_internalCall || !desc) return orig(self, desc, riid, out);
    double t0 = now_ms();
    HRESULT hr = orig(self, desc, riid, out);
    double ms = now_ms() - t0;
    if (SUCCEEDED(hr) && out && *out)
        record(ms, [&](PsoDesc& d, std::string& why) { return capture_compute(*desc, recorded_sink(), root_sig_lookup(), d, &why); });
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_stream(ID3D12Device2* self, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                             REFIID riid, void** out) {
    auto orig = original<PFN_CreateStream>(self, SP_VT_DEVICE2_CREATE_PIPELINE_STATE);
    if (!orig) return E_FAIL;
    if (t_internalCall || !desc) return orig(self, desc, riid, out);
    double t0 = now_ms();
    HRESULT hr = orig(self, desc, riid, out);
    double ms = now_ms() - t0;
    if (SUCCEEDED(hr) && out && *out)
        record(ms, [&](PsoDesc& d, std::string& why) { return capture_stream(*desc, recorded_sink(), root_sig_lookup(), d, &why); });
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_root_sig(ID3D12Device* self, UINT nodeMask, const void* blob, SIZE_T size,
                                               REFIID riid, void** out) {
    auto orig = original<PFN_CreateRootSig>(self, SP_VT_DEVICE_CREATE_ROOT_SIGNATURE);
    if (!orig) return E_FAIL;
    HRESULT hr = orig(self, nodeMask, blob, size, riid, out);
    if (t_internalCall || FAILED(hr) || !out || !*out || !blob || !size || !rt().ok) return hr;
    // The object may be returned as any interface; key the map by its ID3D12RootSignature pointer.
    ID3D12RootSignature* rs = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&rs)))) {
        std::lock_guard<std::mutex> lock(rt().mu);
        auto* b = static_cast<const uint8_t*>(blob);
        rt().rootSigs[rs] = rt().recorded.add_blob(BlobKind::RootSignature, BlobOrigin::Recorded, Bytes(b, b + size));
        rs->Release();
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_load_graphics(ID3D12PipelineLibrary* self, LPCWSTR name,
                                             const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** out) {
    auto orig = original<PFN_LoadGraphics>(self, SP_VT_PIPELINE_LIBRARY_LOAD_GRAPHICS);
    if (!orig) return E_FAIL;
    double t0 = now_ms();
    HRESULT hr = orig(self, name, desc, riid, out);
    if (!t_internalCall && desc && SUCCEEDED(hr) && out && *out)
        record(now_ms() - t0, [&](PsoDesc& d, std::string& why) { return capture_graphics(*desc, recorded_sink(), root_sig_lookup(), d, &why); });
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_load_compute(ID3D12PipelineLibrary* self, LPCWSTR name,
                                            const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** out) {
    auto orig = original<PFN_LoadCompute>(self, SP_VT_PIPELINE_LIBRARY_LOAD_COMPUTE);
    if (!orig) return E_FAIL;
    double t0 = now_ms();
    HRESULT hr = orig(self, name, desc, riid, out);
    if (!t_internalCall && desc && SUCCEEDED(hr) && out && *out)
        record(now_ms() - t0, [&](PsoDesc& d, std::string& why) { return capture_compute(*desc, recorded_sink(), root_sig_lookup(), d, &why); });
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_load_pipeline(ID3D12PipelineLibrary1* self, LPCWSTR name,
                                             const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** out) {
    auto orig = original<PFN_LoadPipeline>(self, SP_VT_PIPELINE_LIBRARY1_LOAD_PIPELINE);
    if (!orig) return E_FAIL;
    double t0 = now_ms();
    HRESULT hr = orig(self, name, desc, riid, out);
    if (!t_internalCall && desc && SUCCEEDED(hr) && out && *out)
        record(now_ms() - t0, [&](PsoDesc& d, std::string& why) { return capture_stream(*desc, recorded_sink(), root_sig_lookup(), d, &why); });
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_create_library(ID3D12Device1* self, const void* blob, SIZE_T size, REFIID riid, void** out) {
    auto orig = original<PFN_CreateLibrary>(self, SP_VT_DEVICE1_CREATE_PIPELINE_LIBRARY);
    if (!orig) return E_FAIL;
    HRESULT hr = orig(self, blob, size, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        ID3D12PipelineLibrary* lib = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&lib)))) {
            patch(lib, SP_VT_PIPELINE_LIBRARY_LOAD_GRAPHICS, reinterpret_cast<void*>(&hook_load_graphics));
            patch(lib, SP_VT_PIPELINE_LIBRARY_LOAD_COMPUTE, reinterpret_cast<void*>(&hook_load_compute));
            lib->Release();
        }
        ID3D12PipelineLibrary1* lib1 = nullptr;
        if (SUCCEEDED(static_cast<IUnknown*>(*out)->QueryInterface(IID_PPV_ARGS(&lib1)))) {
            patch(lib1, SP_VT_PIPELINE_LIBRARY1_LOAD_PIPELINE, reinterpret_cast<void*>(&hook_load_pipeline));
            lib1->Release();
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_factory_create_device(ID3D12DeviceFactory* self, IUnknown* adapter,
                                                     D3D_FEATURE_LEVEL level, REFIID riid, void** out) {
    auto orig = original<PFN_FactoryCreateDevice>(self, SP_VT_DEVICE_FACTORY_CREATE_DEVICE);
    if (!orig) return E_FAIL;
    HRESULT hr = orig(self, adapter, level, riid, out);
    if (SUCCEEDED(hr) && out && *out) on_device_created(static_cast<IUnknown*>(*out));
    return hr;
}

// --- warming & harvesting -----------------------------------------------------------------------

LUID adapter_luid(ID3D12Device* device) {
#if defined(_MSC_VER) || !defined(_WIN32)
    return device->GetAdapterLuid();
#else
    LUID luid;  // MinGW declares struct-returning COM methods with an explicit out-pointer
    device->GetAdapterLuid(&luid);
    return luid;
#endif
}

// True for a real GPU (not WARP / Basic Render Driver): only warm caches that the game will use.
bool hardware_adapter(LUID luid) {
    // Test hook: CI machines only have WARP.
    if (GetEnvironmentVariableW(L"SHADERPREP_ALLOW_SOFTWARE_ADAPTER", nullptr, 0) > 0) return true;
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) return true;
    auto create = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
    if (!create) return true;
    IDXGIFactory4* factory = nullptr;
    if (FAILED(create(IID_PPV_ARGS(&factory)))) return true;
    bool hw = true;
    IDXGIAdapter1* adapter = nullptr;
    if (SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(adapter->GetDesc1(&desc))) hw = !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
        adapter->Release();
    }
    factory->Release();
    return hw;
}

void save_now() {
    Runtime& r = rt();
    std::lock_guard<std::mutex> lock(r.mu);
    if (r.dirty && r.recorded.save(path_u8(data_path("recorded.psodb")))) r.dirty = false;
}

void flusher_thread() {
    Runtime& r = rt();
    uint64_t ticks = 0;
    for (;;) {
        Sleep(10000);
        save_now();
        if (++ticks % 6 == 0) {
            const WarmStats* ws = r.warmer ? &r.warmer->stats() : nullptr;
            logf("session: %llu pipelines created by the game, %llu slow (%llu ms stalled, %llu of them pre-compiled); "
                 "warm: %llu/%llu done, %llu compiled, %llu rejected guesses",
                 (unsigned long long)r.created.load(), (unsigned long long)r.slow.load(),
                 (unsigned long long)r.stallMs.load(), (unsigned long long)r.slowWarmed.load(),
                 ws ? (unsigned long long)ws->done.load() : 0ull, ws ? (unsigned long long)ws->queued.load() : 0ull,
                 ws ? (unsigned long long)ws->created.load() : 0ull, ws ? (unsigned long long)ws->rejected.load() : 0ull);
        }
    }
}

void harvest_thread() {
    Runtime& r = rt();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    uint32_t elapsed = 0;
    for (uint32_t delay : r.profile.scanDelaysS) {
        if (delay > elapsed) Sleep((delay - elapsed) * 1000);
        elapsed = std::max(elapsed, delay);

        PsoDb found;
        Harvester h(found, BlobOrigin::Memory);
        MemScanStats ms = scan_own_memory(h);
        logf("memory scan at %us: %.1f GiB in %llu regions, %llu ms: %llu shaders, %llu root signatures found",
             elapsed, ms.bytes / double(1ull << 30), (unsigned long long)ms.regions, (unsigned long long)ms.ms,
             (unsigned long long)h.stats().newShaders, (unsigned long long)h.stats().newRootSignatures);

        // Learn the game's style from everything it has created so far (including this session),
        // then infer candidates for every shader we know of.
        PsoDb all;
        {
            std::lock_guard<std::mutex> lock(r.mu);
            r.harvested.merge(found);
            r.harvested.save(path_u8(data_path("harvest.psodb")));
            all.merge(*r.base);
            all.merge(r.recorded);
            all.merge(r.harvested);
        }
        StyleModel style;
        style.learn(all);
        auto cands = std::make_shared<PsoDb>();
        InferStats st = infer_candidates(all, style, *cands, r.profile.infer);
        size_t queued = r.warmer ? r.warmer->submit(cands) : 0;
        logf("inferred %zu candidates (compute exact %u, guessed %u, graphics %u; %u PS without VS, %u without style); "
             "%zu new queued for compiling",
             cands->psos().size(), st.computeExact, st.computeGuessed, st.graphicsGuessed, st.pixelShadersNoVs,
             st.pixelShadersNoTemplate, queued);
    }
    logf("memory harvest finished");
}

LRESULT CALLBACK progress_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER) {
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        SetTextColor(dc, RGB(230, 230, 230));
        SetBkMode(dc, TRANSPARENT);
        const WarmStats& s = rt().warmer->stats();
        wchar_t text[256];
        wsprintfW(text, L"shaderprep: compiling shaders before the game starts\n%u / %u",
                  static_cast<unsigned>(s.done.load()), static_cast<unsigned>(s.queued.load()));
        InflateRect(&rc, -16, -16);
        DrawTextW(dc, text, -1, &rc, DT_LEFT | DT_WORDBREAK);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Small always-on-top window shown while the game is held at boot.
void progress_window_thread(std::atomic<bool>* done) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = progress_wndproc;
    wc.hInstance = g_self;
    wc.lpszClassName = L"shaderprep_progress";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    int w = 460, h = 90;
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"shaderprep",
                                WS_POPUP | WS_VISIBLE, (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                                (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, nullptr, nullptr, g_self, nullptr);
    if (!hwnd) return;
    SetTimer(hwnd, 1, 250, nullptr);
    MSG msg;
    while (!done->load()) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
    }
    DestroyWindow(hwnd);
}

void start_warming(ID3D12Device* device) {
    Runtime& r = rt();
    LUID luid = adapter_luid(device);
    std::unique_lock<std::mutex> lock(r.mu);
    if (r.warmer) return;  // one warmer per process: the driver cache is per GPU, not per device
    {
        InternalScope internal;
        if (!hardware_adapter(luid)) {
            logf("device on a software adapter; not warming");
            return;
        }
    }
    r.warmLuid = luid;
    r.warmer = std::make_unique<Warmer>(device, r.profile.threads, !r.profile.blockAtBoot);
    size_t queued = r.warmer->submit(r.base);
    lock.unlock();
    logf("warming started: %zu pipelines queued", queued);

    if (r.profile.blockAtBoot && queued) {
        std::atomic<bool> done{false};
        std::thread window(progress_window_thread, &done);
        double t0 = now_ms();
        bool finished = r.warmer->wait_idle(r.profile.blockTimeoutS * 1000);
        done = true;
        window.join();
        const WarmStats& s = r.warmer->stats();
        logf("boot hold %s after %.1f s: %llu compiled, %llu rejected guesses, %llu failed",
             finished ? "finished" : "timed out", (now_ms() - t0) / 1000.0, (unsigned long long)s.created.load(),
             (unsigned long long)s.rejected.load(), (unsigned long long)s.failed.load());
        r.warmer->set_low_priority(true);
    }
    if (queued && !r.profile.blockAtBoot) {
        std::thread([] {
            Runtime& rr = rt();
            double t0 = now_ms();
            rr.warmer->wait_idle(INFINITE);
            const WarmStats& s = rr.warmer->stats();
            logf("startup pre-compile finished in %.1f s: %llu compiled, %llu rejected guesses, %llu failed",
                 (now_ms() - t0) / 1000.0, (unsigned long long)s.created.load(), (unsigned long long)s.rejected.load(),
                 (unsigned long long)s.failed.load());
        }).detach();
    }
    if (r.profile.memoryScan && !r.harvestStarted.exchange(true)) std::thread(harvest_thread).detach();
}

}  // namespace

void set_module(HMODULE self) { g_self = self; }

void on_device_created(IUnknown* unknown) {
    Runtime& r = rt();
    std::call_once(r.initOnce, [] {
        try {
            init_runtime();
            std::thread(flusher_thread).detach();
        } catch (const std::exception& e) {
            logf("init failed: %s", e.what());
        }
    });
    if (!r.ok) return;

    ID3D12Device* device = nullptr;
    if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&device)))) return;
    bool hooked = patch(device, SP_VT_DEVICE_CREATE_GRAPHICS_PSO, reinterpret_cast<void*>(&hook_create_graphics)) &&
                  patch(device, SP_VT_DEVICE_CREATE_COMPUTE_PSO, reinterpret_cast<void*>(&hook_create_compute)) &&
                  patch(device, SP_VT_DEVICE_CREATE_ROOT_SIGNATURE, reinterpret_cast<void*>(&hook_create_root_sig));
    ID3D12Device1* dev1 = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dev1)))) {
        patch(dev1, SP_VT_DEVICE1_CREATE_PIPELINE_LIBRARY, reinterpret_cast<void*>(&hook_create_library));
        dev1->Release();
    }
    ID3D12Device2* dev2 = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dev2)))) {
        patch(dev2, SP_VT_DEVICE2_CREATE_PIPELINE_STATE, reinterpret_cast<void*>(&hook_create_stream));
        dev2->Release();
    }
    logf("device created; hooks %s", hooked ? "installed" : "FAILED");
    if (hooked) start_warming(device);
    device->Release();
}

void on_device_factory(IUnknown* unknown) {
    ID3D12DeviceFactory* factory = nullptr;
    if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&factory)))) return;
    patch(factory, SP_VT_DEVICE_FACTORY_CREATE_DEVICE, reinterpret_cast<void*>(&hook_factory_create_device));
    factory->Release();
}

}  // namespace sp::hook
