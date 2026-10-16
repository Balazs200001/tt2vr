#include "core/paths.hpp"

#include <windows.h>

namespace core {

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

} // namespace core
