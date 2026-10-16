#include "game/version.hpp"

#include "core/config.hpp"
#include "core/log.hpp"
#include "core/sha256.hpp"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <utility>

namespace game {

namespace {

// Written once by CheckExeVersion, then published by setting g_ready (release); readers load it (acquire).
VersionInfo g_info;
std::atomic<bool> g_ready{false};
std::mutex g_checkMutex;
const VersionInfo kNotChecked{};

std::filesystem::path ExePath()
{
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(std::wstring(buf, n));
}

} // namespace

const VersionInfo& CheckExeVersion()
{
    std::lock_guard lock(g_checkMutex);
    if (g_ready.load(std::memory_order_acquire)) return g_info;

    VersionInfo info;
    try {
        const std::string& fake = core::GetConfig().debug.fake_exe_sha256;
        if (!fake.empty()) {
            info = ClassifyExe(fake, true);
        } else if (const auto hash = core::Sha256FileHex(ExePath())) {
            info = ClassifyExe(*hash, false);
        } else {
            TT2VR_LOG_ERROR("exe sha256: could not hash the game exe; treating the version as unknown");
            info = ClassifyExe("", false);
        }

        if (info.status == VersionStatus::Known) {
            TT2VR_LOG_INFO("exe sha256 {}{} -> known build {}", info.sha256, info.faked ? " (faked)" : "", info.steamBuild);
        } else {
            TT2VR_LOG_WARN("exe sha256 {}{} -> UNKNOWN version: engine hooks off, D3D layer only", info.sha256,
                           info.faked ? " (faked)" : "");
        }
    }
    catch (...) {
        info = VersionInfo{};
        info.status = VersionStatus::Unknown;
    }

    g_info = std::move(info);
    g_ready.store(true, std::memory_order_release);
    return g_info;
}

const VersionInfo& ExeVersion()
{
    if (!g_ready.load(std::memory_order_acquire)) return kNotChecked;
    return g_info;
}

bool EngineHooksAllowed()
{
    return ExeVersion().status == VersionStatus::Known;
}

} // namespace game
