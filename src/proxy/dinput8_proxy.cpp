// dinput8_proxy.cpp - forwarding proxy for dinput8.dll (loader decision: PLAN.md Decisions log 2026-10-16).
//
// TT2.exe statically imports dinput8.dll, so Windows loads this file from the game folder before any
// game code runs. It forwards the 5 real exports to System32\dinput8.dll and starts the mod.
// Rules: no heavy work in DllMain (loader lock). DllMain only redirects one import slot (a memory write) and
// starts a thread; that thread loads tt2vr\tt2vr.dll once the loader lock is released and calls tt2vr_start.
//
// Why the import redirect is here: DllMain runs after the loader has bound TT2.exe's imports and before the
// SteamStub entry point. A redirect written later from a thread raced the stub (2026-10-16: some launches
// exited silently right after it); here nothing else runs yet. The slot points at ProxyCreateDevice, which waits
// for tt2vr_start to finish and then calls tt2vr's device hook (or the original if the mod did not load).
#include <windows.h>
#include <unknwn.h>
#include <d3d11.h>

#include <atomic>
#include <cstring>
#include <string>

namespace {

HMODULE g_self = nullptr;

using CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
    UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*,
    ID3D11DeviceContext**);

void** g_deviceSlot = nullptr; // TT2.exe's import slot of d3d11!D3D11CreateDeviceAndSwapChain
CreateDeviceFn g_deviceOriginal = nullptr; // what the slot held at load
std::atomic<CreateDeviceFn> g_deviceHook{nullptr}; // tt2vr's hook, set by tt2vr_proxy_device_import
HANDLE g_ready = nullptr; // set when the init thread is done (mod started, or failed to load)

// Thread: game main thread (the game's device creation).
HRESULT WINAPI ProxyCreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdkVersion, const DXGI_SWAP_CHAIN_DESC* scDesc,
    IDXGISwapChain** swapChain, ID3D11Device** device, D3D_FEATURE_LEVEL* gotLevel, ID3D11DeviceContext** context)
{
    if (g_ready) {
        WaitForSingleObject(g_ready, 30000); // the mod starts in ~50 ms; the game creates its device ~1 s in
    }
    CreateDeviceFn fn = g_deviceHook.load();
    if (!fn) {
        fn = g_deviceOriginal;
    }
    return fn ? fn(adapter, driverType, software, flags, levels, levelCount, sdkVersion, scDesc, swapChain, device,
                    gotLevel, context)
              : E_FAIL;
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
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(
            base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
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

// Thread: loader (DllMain, process attach). No other thread of the process runs game code yet.
void RedirectDeviceImport()
{
    void** slot = FindImportSlot(GetModuleHandleW(nullptr), "d3d11.dll", "D3D11CreateDeviceAndSwapChain");
    DWORD old = 0;
    if (!slot || !VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        return; // the mod falls back to its own redirect
    }
    g_deviceOriginal = reinterpret_cast<CreateDeviceFn>(*slot);
    *slot = reinterpret_cast<void*>(&ProxyCreateDevice);
    VirtualProtect(slot, sizeof(void*), old, &old);
    g_deviceSlot = slot;
}

// Real dinput8.dll, loaded on first use by full System32 path (never from the game folder).
HMODULE RealDinput8()
{
    static HMODULE real = [] {
        wchar_t dir[MAX_PATH] = {};
        const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            return static_cast<HMODULE>(nullptr);
        }
        std::wstring path(dir);
        path += L"\\dinput8.dll";
        return LoadLibraryW(path.c_str());
    }();
    return real;
}

template <typename Fn>
Fn RealProc(const char* name)
{
    HMODULE real = RealDinput8();
    return real ? reinterpret_cast<Fn>(GetProcAddress(real, name)) : nullptr;
}

std::wstring ModMainPath()
{
    wchar_t self[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(g_self, self, MAX_PATH);
    std::wstring path(self, n);
    const size_t slash = path.find_last_of(L"\\/");
    path = (slash == std::wstring::npos) ? std::wstring() : path.substr(0, slash + 1);
    return path + L"tt2vr\\tt2vr.dll";
}

// Thread: game process startup (spawned from DllMain; runs after the loader lock is released).
DWORD WINAPI InitThread(LPVOID)
{
    HMODULE mod = LoadLibraryW(ModMainPath().c_str());
    if (!mod) {
        OutputDebugStringW(L"[tt2vr] proxy: failed to load tt2vr\\tt2vr.dll; running flat\n");
    }
    else {
        using StartFn = void(__cdecl*)();
        if (auto start = reinterpret_cast<StartFn>(GetProcAddress(mod, "tt2vr_start"))) {
            start();
        }
    }
    if (g_ready) {
        SetEvent(g_ready);
    }
    return 0;
}

} // namespace

extern "C" {

// Called by tt2vr.dll (d3d::InstallDeviceHooks, from tt2vr_start on the init thread): hands over the slot the
// proxy redirected at load and installs the mod's device hook behind ProxyCreateDevice. FALSE = no redirect here.
BOOL WINAPI Proxy_TakeDeviceImport(void*** slot, void** original, void** forwarder, void* hook)
{
    if (!g_deviceSlot || !slot || !original || !forwarder) {
        return FALSE;
    }
    *slot = g_deviceSlot;
    *original = reinterpret_cast<void*>(g_deviceOriginal);
    *forwarder = reinterpret_cast<void*>(&ProxyCreateDevice);
    g_deviceHook = reinterpret_cast<CreateDeviceFn>(hook);
    return TRUE;
}

// Forwarded exports (names mapped in dinput8.def). Signatures use generic types; the real
// functions receive the caller's arguments unchanged.
HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, IUnknown* outer)
{
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, IUnknown*);
    auto real = RealProc<Fn>("DirectInput8Create");
    return real ? real(inst, version, riid, out, outer) : E_FAIL;
}

HRESULT WINAPI Proxy_DllCanUnloadNow()
{
    using Fn = HRESULT(WINAPI*)();
    auto real = RealProc<Fn>("DllCanUnloadNow");
    return real ? real() : S_FALSE;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out)
{
    using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    auto real = RealProc<Fn>("DllGetClassObject");
    return real ? real(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI Proxy_DllRegisterServer()
{
    using Fn = HRESULT(WINAPI*)();
    auto real = RealProc<Fn>("DllRegisterServer");
    return real ? real() : E_FAIL;
}

HRESULT WINAPI Proxy_DllUnregisterServer()
{
    using Fn = HRESULT(WINAPI*)();
    auto real = RealProc<Fn>("DllUnregisterServer");
    return real ? real() : E_FAIL;
}

// Returns a pointer to the DIDATAFORMAT for joysticks (c_dfDIJoystick); typed as void* here.
const void* WINAPI Proxy_GetdfDIJoystick()
{
    using Fn = const void*(WINAPI*)();
    auto real = RealProc<Fn>("GetdfDIJoystick");
    return real ? real() : nullptr;
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        RedirectDeviceImport();
        if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) {
            CloseHandle(t);
        }
        else if (g_ready) {
            SetEvent(g_ready); // no mod: the forwarder calls the original right away
        }
    }
    return TRUE;
}
