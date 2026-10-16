// main.cpp - tt2vr.dll entry. Loaded by the dinput8.dll proxy from <game>\tt2vr\.
#include "core/log.hpp"
#include "d3d/device_hooks.hpp"

#include <windows.h>

extern "C" __declspec(dllexport) void __cdecl tt2vr_start()
{
    // Thread: proxy init thread (game process startup, before the game creates its D3D11 device).
    try {
        core::InitLog();
        TT2VR_LOG_INFO("tt2vr starting (first slice: device flag fix for RenderDoc)");
        d3d::InstallDeviceHooks();
    }
    catch (...) {
        // Never let an exception escape into the game; the game keeps running flat.
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
