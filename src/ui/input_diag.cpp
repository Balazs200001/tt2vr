#include "ui/input_diag.hpp"

#include "core/config.hpp"
#include "core/log.hpp"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#ifndef DIDFT_OPTIONAL
#define DIDFT_OPTIONAL 0x80000000 // dinput.h value; not exposed by every SDK configuration
#endif
#include <wrl/client.h>
#include <Xinput.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

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

// ---- DirectInput (wheels, pedals: the game reads these through DirectInput8, docs/re/README.md imports)
using Microsoft::WRL::ComPtr;
using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

// Our own data format with only the axes (c_dfDIJoystick2 lives in dinput8.lib, which would import the game-folder
// dinput8.dll, i.e. our proxy). Offsets are into DiAxes.
struct DiAxes {
    LONG x, y, z, rx, ry, rz, slider0, slider1;
};
DIOBJECTDATAFORMAT g_axisFormat[] = {
    {&GUID_XAxis, offsetof(DiAxes, x), DIDFT_AXIS | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL, 0},
    {&GUID_YAxis, offsetof(DiAxes, y), DIDFT_AXIS | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL, 0},
    {&GUID_ZAxis, offsetof(DiAxes, z), DIDFT_AXIS | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL, 0},
    {&GUID_RxAxis, offsetof(DiAxes, rx), DIDFT_AXIS | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL, 0},
    {&GUID_RyAxis, offsetof(DiAxes, ry), DIDFT_AXIS | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL, 0},
    {&GUID_RzAxis, offsetof(DiAxes, rz), DIDFT_AXIS | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL, 0},
    {&GUID_Slider, offsetof(DiAxes, slider0), DIDFT_AXIS | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL, 0},
    {&GUID_Slider, offsetof(DiAxes, slider1), DIDFT_AXIS | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL, 0},
};
const DIDATAFORMAT g_dataFormat = {sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_ABSAXIS, sizeof(DiAxes),
    static_cast<DWORD>(std::size(g_axisFormat)), g_axisFormat};

struct DiDevice {
    GUID instance{};
    std::string name;
    ComPtr<IDirectInputDevice8W> device; // non-exclusive, background: never conflicts with the game's own access
    DiAxes axes{};
    bool ok = false;
};

ComPtr<IDirectInput8W> g_di;
bool g_diTried = false;
std::vector<DiDevice> g_diDevices;
Clock::time_point g_nextDiScan{};

std::string Narrow(const wchar_t* w)
{
    char out[260] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, sizeof(out) - 1, nullptr, nullptr);
    return out;
}

void InitDirectInput()
{
    g_diTried = true;
    // The real System32 dinput8.dll (the game folder's dinput8.dll is our proxy, which forwards to the same one).
    wchar_t dir[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return;
    }
    const std::wstring path = std::wstring(dir, n) + L"\\dinput8.dll";
    HMODULE mod = LoadLibraryW(path.c_str());
    auto create = mod ? reinterpret_cast<DirectInput8CreateFn>(GetProcAddress(mod, "DirectInput8Create")) : nullptr;
    if (!create || FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                       reinterpret_cast<void**>(g_di.GetAddressOf()), nullptr))) {
        g_di.Reset();
        TT2VR_LOG_WARN("input diag: DirectInput not available");
    }
}

BOOL CALLBACK CollectDevice(const DIDEVICEINSTANCEW* inst, void* user)
{
    static_cast<std::vector<DIDEVICEINSTANCEW>*>(user)->push_back(*inst);
    return DIENUM_CONTINUE;
}

// Every 2 s: attached game controllers; logs attach/detach and opens a reader for new ones.
void ScanDirectInput(HWND gameWindow)
{
    std::vector<DIDEVICEINSTANCEW> found;
    if (FAILED(g_di->EnumDevices(DI8DEVCLASS_GAMECTRL, &CollectDevice, &found, DIEDFL_ATTACHEDONLY))) {
        return;
    }
    for (auto it = g_diDevices.begin(); it != g_diDevices.end();) {
        const bool still = std::any_of(found.begin(), found.end(),
            [&](const DIDEVICEINSTANCEW& f) { return IsEqualGUID(f.guidInstance, it->instance); });
        if (!still) {
            TT2VR_LOG_WARN("input diag: DirectInput device DETACHED: {}", it->name);
            it = g_diDevices.erase(it);
        }
        else {
            ++it;
        }
    }
    for (const auto& f : found) {
        const bool known = std::any_of(g_diDevices.begin(), g_diDevices.end(),
            [&](const DiDevice& d) { return IsEqualGUID(d.instance, f.guidInstance); });
        if (known) {
            continue;
        }
        DiDevice d;
        d.instance = f.guidInstance;
        d.name = Narrow(f.tszProductName);
        HRESULT hr = g_di->CreateDevice(f.guidInstance, &d.device, nullptr);
        if (SUCCEEDED(hr)) hr = d.device->SetDataFormat(&g_dataFormat);
        if (SUCCEEDED(hr)) hr = d.device->SetCooperativeLevel(gameWindow, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
        if (FAILED(hr)) {
            d.device.Reset();
        }
        TT2VR_LOG_INFO("input diag: DirectInput device attached: {} (reader {})", d.name,
            d.device ? "ok" : "failed");
        g_diDevices.push_back(std::move(d));
    }
}

void PollDirectInput()
{
    for (auto& d : g_diDevices) {
        d.ok = false;
        if (!d.device) {
            continue;
        }
        d.device->Acquire(); // non-exclusive background acquire; harmless if already acquired
        d.device->Poll();
        DiAxes a{};
        if (SUCCEEDED(d.device->GetDeviceState(sizeof(a), &a))) {
            d.axes = a;
            d.ok = true;
        }
    }
}

// Prefer the XInput DLL the game itself loaded (xinput1_3, possibly UWP\xinput1_3.dll): Steam Input hooks that copy,
// so we see what the game sees. Otherwise load the system one (loaded at run time: the mod adds no import).
void LoadXInput()
{
    g_loadTried = true;
    for (const wchar_t* dll : {L"xinput1_3.dll", L"xinput1_4.dll", L"xinput9_1_0.dll"}) {
        HMODULE mod = GetModuleHandleW(dll);
        const bool loaded = mod != nullptr;
        if (!mod && dll != std::wstring_view(L"xinput1_3.dll")) {
            mod = LoadLibraryW(dll);
        }
        if (mod) {
            g_getState = reinterpret_cast<XInputGetStateFn>(GetProcAddress(mod, "XInputGetState"));
            if (g_getState) {
                char path[MAX_PATH] = {};
                GetModuleFileNameA(mod, path, MAX_PATH);
                TT2VR_LOG_INFO("input diag: reading pads through {} ({})", path,
                    loaded ? "already loaded by the game" : "loaded by us");
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
        const auto now = Clock::now();
        if (g_getState) {
            PollPads(now);
        }
        if (!g_diTried) {
            InitDirectInput();
        }
        if (g_di && now >= g_nextDiScan) {
            g_nextDiScan = now + std::chrono::seconds(2);
            ScanDirectInput(gameWindow);
        }
        if (now < g_nextSummary) {
            return;
        }
        if (g_di) {
            PollDirectInput();
            std::string di;
            for (const auto& d : g_diDevices) {
                if (!d.ok) {
                    di += " | " + d.name + " (no state)";
                    continue;
                }
                const DiAxes& a = d.axes;
                di += " | " + d.name + " x " + std::to_string(a.x) + " y " + std::to_string(a.y) + " z " +
                      std::to_string(a.z) + " rx " + std::to_string(a.rx) + " ry " + std::to_string(a.ry) + " rz " +
                      std::to_string(a.rz) + " s0 " + std::to_string(a.slider0) + " s1 " + std::to_string(a.slider1);
            }
            if (!di.empty()) {
                TT2VR_LOG_INFO("input diag: DI{}", di);
            }
        }
        g_nextSummary = now + std::chrono::seconds(1);
        if (!g_getState) {
            return;
        }
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
