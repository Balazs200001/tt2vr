// Game build identification: hash TT2.exe and decide whether our engine signatures/hooks apply.
// ClassifyExe and the known-build table are header-only so the unit tests can use them without the game.
#pragma once

#include <string>
#include <string_view>

namespace game {

enum class VersionStatus { NotChecked, Known, Unknown };

struct VersionInfo {
    VersionStatus status = VersionStatus::NotChecked;
    std::string sha256; // hash used for the decision (the fake one if faked)
    std::string steamBuild; // e.g. "4989319" when known
    bool faked = false;
};

struct KnownBuild {
    const char* sha256;
    const char* steamBuild;
};

inline constexpr KnownBuild kKnownBuilds[] = {
    // docs/re/README.md, Build identity
    {"103d4b2e4d86786a7eb1810faa280a41cec6cadce1e8865165c3de8ba87a601a", "4989319"},
};

// Pure decision, unit-testable without the game: lower-cases `sha256` before lookup.
inline VersionInfo ClassifyExe(std::string_view sha256, bool faked)
{
    VersionInfo info;
    info.faked = faked;
    info.sha256.reserve(sha256.size());
    for (const char c : sha256) {
        info.sha256.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    info.status = VersionStatus::Unknown;
    for (const KnownBuild& build : kKnownBuilds) {
        if (info.sha256 == build.sha256) {
            info.status = VersionStatus::Known;
            info.steamBuild = build.steamBuild;
            break;
        }
    }
    return info;
}

// Hashes the running exe (GetModuleFileNameW(nullptr)) once, or uses core::GetConfig().debug.fake_exe_sha256
// if set. Logs one line, e.g. "exe sha256 <hash> (faked) -> known build 4989319" or
// "exe sha256 <hash> -> UNKNOWN version: engine hooks off, D3D layer only".
// Call once from the init thread; later calls return the first result.
const VersionInfo& CheckExeVersion();
// Last result (NotChecked before CheckExeVersion has finished). Safe to call from any thread.
const VersionInfo& ExeVersion();
// status == Known
bool EngineHooksAllowed();

} // namespace game
