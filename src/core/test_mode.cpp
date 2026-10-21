#include "core/test_mode.hpp"

#include <windows.h>

#include <cwctype>
#include <string>

namespace core {

bool TestMode()
{
    static const bool s_test = [] {
        std::wstring line = GetCommandLineW();
        for (wchar_t& c : line) {
            c = static_cast<wchar_t>(std::towlower(c));
        }
        return line.find(L"-tt2vr-test") != std::wstring::npos;
    }();
    return s_test;
}

} // namespace core
