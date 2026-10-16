#include "ui/input_diag.hpp"

#include "core/config.hpp"
#include "core/log.hpp"

#include <Xinput.h>

#include <chrono>
#include <cstdio>
#include <string>

namespace ui {
namespace {

using Clock = std::chrono::steady_clock;
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

XInputGetStateFn g_getState = nullptr;
bool g_loadTried = false;
HWND g_lastForeground = reinterpret_cast<HWND>(-1);
bool g_connected[XUSER_MAX_COUNT] = {};
XINPUT_STATE g_state[XUSER_MAX_COUNT] = {};
Clock::time_point g_nextProbe{}; // disconnected slots are slow to query: re-probe every 2 s
Clock::time_point g_nextSummary{};

// Loaded at run time so the mod adds no import; xinput1_4 ships with Windows 8+.
void LoadXInput()
{
    g_loadTried = true;
    for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput9_1_0.dll"}) {
        if (HMODULE mod = LoadLibraryW(dll)) {
            g_getState = reinterpret_cast<XInputGetStateFn>(GetProcAddress(mod, "XInputGetState"));
            if (g_getState) {
                return;
            }
        }
    }
    TT2VR_LOG_WARN("input diag: XInput not available; logging focus only");
}

std::string Describe(HWND window)
{
    if (!window) {
        return "none";
    }
    char title[128] = {};
    GetWindowTextA(window, title, sizeof(title));
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    std::string exe = "?";
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        char path[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameA(process, 0, path, &size)) {
            const std::string full(path, size);
            exe = full.substr(full.find_last_of('\\') + 1);
        }
        CloseHandle(process);
    }
    return exe + " \"" + title + "\"";
}

void PollPads(const Clock::time_point now)
{
    const bool probe = now >= g_nextProbe;
    if (probe) {
        g_nextProbe = now + std::chrono::seconds(2);
    }
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (!g_connected[i] && !probe) {
            continue;
        }
        XINPUT_STATE s{};
        const bool connected = g_getState(i, &s) == ERROR_SUCCESS;
        if (connected != g_connected[i]) {
            TT2VR_LOG_INFO("input diag: XInput pad {} {}", i, connected ? "connected" : "disconnected");
            g_connected[i] = connected;
        }
        g_state[i] = connected ? s : XINPUT_STATE{};
    }
}

} // namespace

void TickInputDiagnostics(HWND gameWindow)
{
    try {
        if (!core::GetConfig().debug.input_log) {
            return;
        }
        const HWND foreground = GetForegroundWindow();
        if (foreground != g_lastForeground) {
            g_lastForeground = foreground;
            TT2VR_LOG_INFO("input diag: foreground -> {} ({})", Describe(foreground),
                foreground == gameWindow ? "GAME HAS FOCUS" : "game NOT focused");
        }
        if (!g_loadTried) {
            LoadXInput();
        }
        if (!g_getState) {
            return;
        }
        const auto now = Clock::now();
        PollPads(now);
        if (now < g_nextSummary) {
            return;
        }
        g_nextSummary = now + std::chrono::seconds(1);
        std::string line = foreground == gameWindow ? "focus game" : "focus OTHER";
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            if (!g_connected[i]) {
                continue;
            }
            const XINPUT_GAMEPAD& p = g_state[i].Gamepad;
            line += " | pad" + std::to_string(i) + " lx " + std::to_string(p.sThumbLX) + " ly " +
                    std::to_string(p.sThumbLY) + " lt " + std::to_string(p.bLeftTrigger) + " rt " +
                    std::to_string(p.bRightTrigger) + " btn 0x" + [&] {
                        char hex[8];
                        std::snprintf(hex, sizeof(hex), "%04x", p.wButtons);
                        return std::string(hex);
                    }();
        }
        TT2VR_LOG_INFO("input diag: {}", line);
    }
    catch (...) {
    }
}

} // namespace ui
