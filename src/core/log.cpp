#include "core/log.hpp"

#include "core/paths.hpp"

#include <spdlog/sinks/basic_file_sink.h>

namespace core {

void InitLog()
{
    try {
        const auto dir = ModDir() / L"logs";
        std::filesystem::create_directories(dir);
        // Keep the previous session's log: the game is often relaunched right after a test.
        std::error_code ec;
        std::filesystem::rename(dir / L"tt2vr.log", dir / L"tt2vr.prev.log", ec);
        auto logger = spdlog::basic_logger_mt("tt2vr", (dir / L"tt2vr.log").string(), true);
        logger->set_pattern("[%H:%M:%S.%e] [%t] [%l] %v");
        logger->flush_on(spdlog::level::info);
        spdlog::set_default_logger(logger);
        // Never let a logging failure throw into game code.
        spdlog::set_error_handler([](const std::string&) {});
    }
    catch (...) {
        // Logging unavailable; the mod keeps running without it.
    }
}

void SetLogLevel(std::string_view level)
{
    spdlog::set_level(spdlog::level::from_str(std::string(level)));
}

} // namespace core
