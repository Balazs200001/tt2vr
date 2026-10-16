#pragma once

#include <spdlog/spdlog.h>

#include <atomic>
#include <string_view>

namespace core {

// Creates <game>\tt2vr\logs\tt2vr.log (truncated each launch). Safe to call once.
void InitLog();

// "trace", "debug", "info", "warn", "error" (config [log] level).
void SetLogLevel(std::string_view level);

namespace detail {
inline std::atomic<bool> g_verbose{false};
}
// Per-frame logging is allowed only while this is on (config [log] verbose_frames, overlay toggle).
inline bool GetVerbose()
{
    return detail::g_verbose.load(std::memory_order_relaxed);
}
inline void SetVerbose(bool on)
{
    detail::g_verbose.store(on, std::memory_order_relaxed);
}

} // namespace core

// Logging macros (CLAUDE.md). spdlog is configured not to throw; see log.cpp.
#define TT2VR_LOG_TRACE(...) ::spdlog::trace(__VA_ARGS__)
#define TT2VR_LOG_DEBUG(...) ::spdlog::debug(__VA_ARGS__)
#define TT2VR_LOG_INFO(...) ::spdlog::info(__VA_ARGS__)
#define TT2VR_LOG_WARN(...) ::spdlog::warn(__VA_ARGS__)
#define TT2VR_LOG_ERROR(...) ::spdlog::error(__VA_ARGS__)
