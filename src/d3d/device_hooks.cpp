// device_hooks.cpp - D3D11 device creation hook.
//
// Why: TT2 passes D3D11_CREATE_DEVICE_PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY (0x80) to
// D3D11CreateDeviceAndSwapChain. RenderDoc treats that flag as "do not hook me" and the game then
// crashes under RenderDoc (docs/re/README.md, "Capture-tool opt-out"). We clear the bit before the
// call reaches anything else. The flag only affects debug-layer registry settings; no gameplay effect.
//
// How: redirect TT2.exe's import address table slot (a data write, no code patching). Whatever the
// slot pointed to before (RenderDoc's hook if present, otherwise d3d11.dll) is called next, so the
// chain stays intact. RenderDoc re-applies its import hooks when new DLLs load, so a watchdog
// re-checks the slot until the game has created its device.
#include "d3d/device_hooks.hpp"

#include "core/log.hpp"
#include "d3d/present.hpp"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstring>
#include <string>

namespace d3d {
namespace {

using CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
    D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

constexpr UINT kPreventAlteringLayerSettings = 0x80; // D3D11_CREATE_DEVICE_PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY

void** g_slot = nullptr;
std::atomic<CreateDeviceAndSwapChainFn> g_next{nullptr};
std::atomic<bool> g_called{false};

std::string ModuleOf(const void* address)
{
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address), &mod)) {
        return "?";
    }
    char path[MAX_PATH] = {};
    GetModuleFileNameA(mod, path, MAX_PATH);
    const char* name = std::strrchr(path, '\\');
    return name ? name + 1 : path;
}

// Thread: game main thread (inside VideoDeviceDX11Windows::EnableVideoAdapter).
HRESULT WINAPI HookedCreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType,
    HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* scDesc, IDXGISwapChain** swapChain, ID3D11Device** device,
    D3D_FEATURE_LEVEL* gotLevel, ID3D11DeviceContext** context)
{
    g_called = true;
    const UINT newFlags = flags & ~kPreventAlteringLayerSettings;
    CreateDeviceAndSwapChainFn next = g_next.load();
    // Re-entry guard: if another hook chained back into us, go straight to the real export.
    thread_local int depth = 0;
    if (depth > 0) {
        next = reinterpret_cast<CreateDeviceAndSwapChainFn>(
            GetProcAddress(GetModuleHandleW(L"d3d11.dll"), "D3D11CreateDeviceAndSwapChain"));
    }
    struct DepthGuard {
        int& d;
        explicit DepthGuard(int& v) : d(v) { ++d; }
        ~DepthGuard() { --d; }
    } guard(depth);
    if (!next) {
        return E_FAIL;
    }
    const HRESULT hr = next(adapter, driverType, software, newFlags, levels, levelCount, sdkVersion, scDesc,
        swapChain, device, gotLevel, context);
    TT2VR_LOG_INFO("D3D11CreateDeviceAndSwapChain flags 0x{:x} -> 0x{:x}, hr=0x{:08x}, featureLevel=0x{:x}, "
                   "context={}, next={}",
        flags, newFlags, static_cast<unsigned>(hr), gotLevel ? static_cast<unsigned>(*gotLevel) : 0u,
        (context && *context) ? "ok" : "null", ModuleOf(reinterpret_cast<const void*>(next)));
    // Install the rest of the D3D layer before the game creates its first shader (outermost call only).
    static std::atomic<bool> s_setupDone{false};
    if (SUCCEEDED(hr) && depth == 1 && device && *device && !s_setupDone.exchange(true)) {
        try {
            OnDeviceCreated(*device, context ? *context : nullptr, swapChain ? *swapChain : nullptr);
        }
        catch (...) {
            TT2VR_LOG_ERROR("exception while installing D3D hooks; continuing without them");
        }
    }
    return hr;
}

// Finds the IAT slot of `func` imported from `dll` in `module`.
void** FindImportSlot(HMODULE module, const char* dll, const char* func)
{
    auto base = reinterpret_cast<BYTE*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) {
        return nullptr;
    }
    for (auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
        if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), dll) != 0) {
            continue;
        }
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }
            auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), func) == 0) {
                return reinterpret_cast<void**>(&slots->u1.Function);
            }
        }
    }
    return nullptr;
}

bool WriteSlot(void* value)
{
    DWORD old = 0;
    if (!VirtualProtect(g_slot, sizeof(void*), PAGE_READWRITE, &old)) {
        return false;
    }
    *g_slot = value;
    VirtualProtect(g_slot, sizeof(void*), old, &old);
    return true;
}

// Thread: our watchdog. Keeps the redirect until the device exists (re-applies after RenderDoc re-hooks).
DWORD WINAPI Watchdog(LPVOID)
{
    for (int i = 0; i < 12000 && !g_called; ++i) { // up to ~60 s
        void* current = *g_slot;
        if (current != reinterpret_cast<void*>(&HookedCreateDeviceAndSwapChain)) {
            g_next = reinterpret_cast<CreateDeviceAndSwapChainFn>(current);
            if (WriteSlot(reinterpret_cast<void*>(&HookedCreateDeviceAndSwapChain))) {
                TT2VR_LOG_WARN("Import slot was overwritten (now -> {}); redirect re-applied",
                    ModuleOf(current));
            }
        }
        Sleep(5);
    }
    if (!g_called) {
        TT2VR_LOG_WARN("Device hook watchdog timed out before D3D11CreateDeviceAndSwapChain was called");
    }
    return 0;
}

} // namespace

bool InstallDeviceHooks()
{
    g_slot = FindImportSlot(GetModuleHandleW(nullptr), "d3d11.dll", "D3D11CreateDeviceAndSwapChain");
    if (!g_slot) {
        TT2VR_LOG_ERROR("D3D11CreateDeviceAndSwapChain import not found in TT2.exe; device hook disabled");
        return false;
    }
    void* current = *g_slot;
    g_next = reinterpret_cast<CreateDeviceAndSwapChainFn>(current);
    if (!WriteSlot(reinterpret_cast<void*>(&HookedCreateDeviceAndSwapChain))) {
        TT2VR_LOG_ERROR("VirtualProtect failed on the import slot; device hook disabled");
        return false;
    }
    TT2VR_LOG_INFO("D3D11CreateDeviceAndSwapChain import redirected (previous target: {})", ModuleOf(current));
    if (HANDLE t = CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr)) {
        CloseHandle(t);
    }
    return true;
}

} // namespace d3d
