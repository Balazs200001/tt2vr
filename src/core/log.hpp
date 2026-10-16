#pragma once

#include <spdlog/spdlog.h>

namespace core {

// Creates <game>\tt2vr\logs\tt2vr.log (truncated each launch). Safe to call once.
void InitLog();

} // namespace core

// Logging macros (CLAUDE.md). spdlog is configured not to throw; see log.cpp.
#define TT2VR_LOG_INFO(...) ::spdlog::info(__VA_ARGS__)
#define TT2VR_LOG_WARN(...) ::spdlog::warn(__VA_ARGS__)
#define TT2VR_LOG_ERROR(...) ::spdlog::error(__VA_ARGS__)
