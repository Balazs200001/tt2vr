#include "core/paths.hpp"

#include "core/config.hpp"

#include <windows.h>

#include <system_error>

namespace core {
namespace {

std::filesystem::path Resolve(const std::string& configured)
{
    std::filesystem::path p(std::u8string(configured.begin(), configured.end())); // config text is UTF-8
    if (p.is_relative()) {
        p = ModDir() / p;
    }
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p.lexically_normal();
}

} // namespace

std::filesystem::path GameDir()
{
    wchar_t exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(std::wstring(exe, n)).parent_path();
}

std::filesystem::path ModDir()
{
    return GameDir() / L"tt2vr";
}

std::filesystem::path CaptureDir()
{
    return Resolve(GetConfig().capture.dir);
}

std::filesystem::path RenderDocDir()
{
    const auto& dir = GetConfig().capture.renderdoc_dir;
    return dir.empty() ? CaptureDir() : Resolve(dir);
}

std::filesystem::path CommandsDir()
{
    return Resolve("commands");
}

} // namespace core
