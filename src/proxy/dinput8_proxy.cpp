// dinput8_proxy.cpp - forwarding proxy for dinput8.dll (loader decision: PLAN.md Decisions log 2026-10-16).
//
// TT2.exe statically imports dinput8.dll, so Windows loads this file from the game folder before any
// game code runs. It forwards the 5 real exports to System32\dinput8.dll and starts the mod.
// Rules: no heavy work in DllMain (loader lock). DllMain only starts a thread; that thread loads
// tt2vr\tt2vr.dll once the loader lock is released and calls its tt2vr_start export.
#include <windows.h>
#include <unknwn.h>

#include <string>

namespace {

HMODULE g_self = nullptr;

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
        return 0;
    }
    using StartFn = void(__cdecl*)();
    if (auto start = reinterpret_cast<StartFn>(GetProcAddress(mod, "tt2vr_start"))) {
        start();
    }
    return 0;
}

} // namespace

extern "C" {

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
        if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) {
            CloseHandle(t);
        }
    }
    return TRUE;
}
