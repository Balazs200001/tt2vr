// main.cpp - tt2vr.dll entry. Loaded by the dinput8.dll proxy from <game>\tt2vr\.
#include "core/config.hpp"
#include "core/hooks.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "d3d/device_hooks.hpp"
#include "game/version.hpp"

#include <windows.h>

namespace {

void LoadConfiguration()
{
    const auto file = core::ModDir() / L"tt2vr.toml";
    const core::ConfigResult result = core::LoadConfig(file);
    core::SetConfig(result.config);
    core::SetLogLevel(result.config.log.level);
    core::SetVerbose(result.config.log.verbose_frames);
    core::SetConfigWarnings(result.warnings);
    for (const auto& warning : result.warnings) {
        TT2VR_LOG_WARN("config: {}", warning);
    }
    TT2VR_LOG_INFO("config loaded from {} ({} warnings)", file.string(), result.warnings.size());
}

} // namespace

extern "C" __declspec(dllexport) void __cdecl tt2vr_start()
{
    // Thread: proxy init thread (game process startup, before the game creates its D3D11 device).
    // Order matters: config first (the device hook reads it), then the device hook as early as possible,
    // then the slow exe hash. Engine hooks (Phase 2+) are only allowed when the hash is a known build.
    try {
        core::InitLog();
        TT2VR_LOG_INFO("tt2vr 0.1.0 starting (Phase 3: OpenXR, left eye head-tracked)");
        LoadConfiguration();
        if (!core::hooks::Init()) {
            TT2VR_LOG_ERROR("MinHook unavailable: overlay, traces and dumps disabled");
        }
        d3d::InstallDeviceHooks();
        game::CheckExeVersion();
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
