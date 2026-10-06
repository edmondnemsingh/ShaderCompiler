// Proxy d3d12.dll. Placed next to the game's executable, Windows loads it instead of the system
// d3d12.dll; every export forwards to the real System32 copy, and device creation is intercepted
// so the device's pipeline-creation methods can be hooked.
#include "win/d3d12.hpp"

#include <mutex>

#include "hook/hooks.hpp"

namespace {

HMODULE g_real = nullptr;
std::once_flag g_loadOnce;

HMODULE real_d3d12() {
    std::call_once(g_loadOnce, [] {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n + 11 >= MAX_PATH) return;
        lstrcatW(path, L"\\d3d12.dll");
        g_real = LoadLibraryW(path);
    });
    return g_real;
}

template <typename F>
F real_fn(const char* name) {
    HMODULE m = real_d3d12();
    return m ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}

}  // namespace

extern "C" {

HRESULT WINAPI Proxy_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL minLevel, REFIID riid, void** device) {
    using Fn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    static Fn fn = real_fn<Fn>("D3D12CreateDevice");
    if (!fn) return E_FAIL;
    HRESULT hr = fn(adapter, minLevel, riid, device);
    if (SUCCEEDED(hr) && device && *device) sp::hook::on_device_created(static_cast<IUnknown*>(*device));
    return hr;
}

HRESULT WINAPI Proxy_D3D12GetDebugInterface(REFIID riid, void** debug) {
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    static Fn fn = real_fn<Fn>("D3D12GetDebugInterface");
    return fn ? fn(riid, debug) : E_FAIL;
}

HRESULT WINAPI Proxy_D3D12GetInterface(REFCLSID clsid, REFIID riid, void** out) {
    using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
    static Fn fn = real_fn<Fn>("D3D12GetInterface");
    if (!fn) return E_NOINTERFACE;
    HRESULT hr = fn(clsid, riid, out);
    if (SUCCEEDED(hr) && out && *out && IsEqualCLSID(clsid, CLSID_D3D12DeviceFactory))
        sp::hook::on_device_factory(static_cast<IUnknown*>(*out));
    return hr;
}

HRESULT WINAPI Proxy_D3D12CreateRootSignatureDeserializer(LPCVOID src, SIZE_T size, REFIID riid, void** out) {
    using Fn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    static Fn fn = real_fn<Fn>("D3D12CreateRootSignatureDeserializer");
    return fn ? fn(src, size, riid, out) : E_FAIL;
}

HRESULT WINAPI Proxy_D3D12CreateVersionedRootSignatureDeserializer(LPCVOID src, SIZE_T size, REFIID riid, void** out) {
    using Fn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    static Fn fn = real_fn<Fn>("D3D12CreateVersionedRootSignatureDeserializer");
    return fn ? fn(src, size, riid, out) : E_FAIL;
}

HRESULT WINAPI Proxy_D3D12EnableExperimentalFeatures(UINT n, const IID* iids, void* configs, UINT* sizes) {
    using Fn = HRESULT(WINAPI*)(UINT, const IID*, void*, UINT*);
    static Fn fn = real_fn<Fn>("D3D12EnableExperimentalFeatures");
    return fn ? fn(n, iids, configs, sizes) : E_FAIL;
}

HRESULT WINAPI Proxy_D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc, D3D_ROOT_SIGNATURE_VERSION v,
                                                 ID3DBlob** blob, ID3DBlob** error) {
    using Fn = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    static Fn fn = real_fn<Fn>("D3D12SerializeRootSignature");
    return fn ? fn(desc, v, blob, error) : E_FAIL;
}

HRESULT WINAPI Proxy_D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc,
                                                          ID3DBlob** blob, ID3DBlob** error) {
    using Fn = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
    static Fn fn = real_fn<Fn>("D3D12SerializeVersionedRootSignature");
    return fn ? fn(desc, blob, error) : E_FAIL;
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        sp::hook::set_module(inst);
    }
    // No cleanup on detach: worker threads may still be running and the process is exiting.
    return TRUE;
}
