// main.cpp - tt2vr.dll entry. Loaded by the dinput8.dll proxy from <game>\tt2vr\.
#include "core/config.hpp"
#include "core/hooks.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/user_toml.hpp"
#include "core/version.hpp"
#include "d3d/device_hooks.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <string>

namespace {

// Launch diagnostics (2026-10-16: some launches end silently ~1 ms after the mod starts, before the game creates
// its window). At process exit the loader calls DllMain(DLL_PROCESS_DETACH) only for ExitProcess, not for
// TerminateProcess; other threads are already gone then, so the line is written with a raw handle, not spdlog.
wchar_t g_logPath[MAX_PATH] = {};
LARGE_INTEGER g_startCounter{};

void AppendExitLine()
{
    if (!g_logPath[0]) {
        return;
    }
    HANDLE f = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return;
    }
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const double ms = freq.QuadPart ? 1000.0 * static_cast<double>(now.QuadPart - g_startCounter.QuadPart) /
                                          static_cast<double>(freq.QuadPart)
                                    : 0.0;
    char line[160];
    const int n = wsprintfA(line, "[exit] process exiting through ExitProcess, %d ms after tt2vr_start, thread %lu\r\n",
        static_cast<int>(ms), GetCurrentThreadId());
    DWORD written = 0;
    WriteFile(f, line, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(f);
}

// Who started us (steam.exe, the SteamStub relaunching, a shortcut, RenderDoc).
std::string ParentProcess()
{
    const DWORD self = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return "?";
    }
    PROCESSENTRY32W e{sizeof(e)};
    DWORD parent = 0;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        if (e.th32ProcessID == self) {
            parent = e.th32ParentProcessID;
            break;
        }
    }
    std::string name = "?";
    e.dwSize = sizeof(e);
    for (BOOL ok = Process32FirstW(snap, &e); ok && parent; ok = Process32NextW(snap, &e)) {
        if (e.th32ProcessID == parent) {
            char buf[MAX_PATH] = {};
            WideCharToMultiByte(CP_UTF8, 0, e.szExeFile, -1, buf, MAX_PATH, nullptr, nullptr);
            name = buf;
            break;
        }
    }
    CloseHandle(snap);
    return name + " (pid " + std::to_string(parent) + ")";
}

void LoadConfiguration()
{
    const auto file = core::ModDir() / L"tt2vr.toml";
    const core::ConfigResult fileResult = core::LoadConfig(file);
    // Settings changed at runtime (PLAN 8d / 8i) override tt2vr.toml.
    core::ConfigResult result = core::LoadConfig(core::UserTomlPath(), fileResult.config, true);
    for (auto& warning : result.warnings) {
        warning = "user.toml: " + warning;
    }
    result.warnings.insert(result.warnings.begin(), fileResult.warnings.begin(), fileResult.warnings.end());
    core::SetFileConfig(fileResult.config);
    core::SetConfig(result.config);
    core::SetLogLevel(result.config.log.level);
    core::SetVerbose(result.config.log.verbose_frames);
    core::SetConfigWarnings(result.warnings);
    for (const auto& warning : result.warnings) {
        TT2VR_LOG_WARN("config: {}", warning);
    }
    std::error_code ec;
    TT2VR_LOG_INFO("config loaded from {}{} ({} warnings)", file.string(),
        std::filesystem::exists(core::UserTomlPath(), ec) ? " and user.toml" : "", result.warnings.size());
}

} // namespace

extern "C" __declspec(dllexport) void __cdecl tt2vr_start()
{
    // Thread: proxy init thread (game process startup, before the game creates its D3D11 device).
    // Order matters: config first (the device hook reads it), then the device hook as early as possible. Nothing
    // slow or loader-heavy here: this runs while the SteamStub is still starting the game. Engine hooks (Phase 2+)
    // are only allowed once the device hook has hashed the exe and found a known build.
    try {
        QueryPerformanceCounter(&g_startCounter);
        core::InitLog();
        const auto logPath = core::ModDir() / L"logs" / L"tt2vr.log";
        lstrcpynW(g_logPath, logPath.c_str(), MAX_PATH);
        TT2VR_LOG_INFO("tt2vr {} starting; pid {}, parent {}", core::kVersion, GetCurrentProcessId(),
            ParentProcess());
        LoadConfiguration();
        if (!core::hooks::Init()) {
            TT2VR_LOG_ERROR("MinHook unavailable: overlay, traces and dumps disabled");
        }
        d3d::InstallDeviceHooks();
        // The exe hash (bcrypt) runs in the device-creation hook instead: here it ran while the SteamStub was still
        // starting the game, and bcrypt's provider loading deadlocked with it (2026-10-20: three hung launches,
        // stacks in bcrypt / ntdll loader on both threads).
        TT2VR_LOG_INFO("startup: done; exe check at device creation");
    }
    catch (...) {
        // Never let an exception escape into the game; the game keeps running flat.
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }
    else if (reason == DLL_PROCESS_DETACH && reserved) { // process exit (not FreeLibrary)
        AppendExitLine();
    }
    return TRUE;
}
