#include "game/virtual_pad.hpp"

#include "core/hooks.hpp"
#include "core/log.hpp"
#include "core/test_mode.hpp"

#include <spdlog/fmt/fmt.h>

#include <windows.h>
#include <Xinput.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <vector>

namespace game {
namespace {

using Clock = std::chrono::steady_clock;
using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using GetCapabilitiesFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
using GetKeystrokeFn = DWORD(WINAPI*)(DWORD, DWORD, PXINPUT_KEYSTROKE);

constexpr DWORD kPadUser = 0;
constexpr int kTapMs = 120;
constexpr int kGapMs = 200;

GetStateFn g_getState = nullptr;
GetCapabilitiesFn g_getCapabilities = nullptr;
GetKeystrokeFn g_getKeystroke = nullptr;
bool g_installed = false;

// Shared with the game's input thread(s).
std::mutex g_mutex;
XINPUT_GAMEPAD g_pad{};
DWORD g_packet = 1;
std::deque<XINPUT_KEYSTROKE> g_keys;

struct Button {
    const char* name;
    WORD mask;
    WORD vk;
};
constexpr Button kButtons[] = {
    {"a", XINPUT_GAMEPAD_A, VK_PAD_A},
    {"b", XINPUT_GAMEPAD_B, VK_PAD_B},
    {"x", XINPUT_GAMEPAD_X, VK_PAD_X},
    {"y", XINPUT_GAMEPAD_Y, VK_PAD_Y},
    {"start", XINPUT_GAMEPAD_START, VK_PAD_START},
    {"back", XINPUT_GAMEPAD_BACK, VK_PAD_BACK},
    {"lb", XINPUT_GAMEPAD_LEFT_SHOULDER, VK_PAD_LSHOULDER},
    {"rb", XINPUT_GAMEPAD_RIGHT_SHOULDER, VK_PAD_RSHOULDER},
    {"ls", XINPUT_GAMEPAD_LEFT_THUMB, VK_PAD_LTHUMB_PRESS},
    {"rs", XINPUT_GAMEPAD_RIGHT_THUMB, VK_PAD_RTHUMB_PRESS},
    {"up", XINPUT_GAMEPAD_DPAD_UP, VK_PAD_DPAD_UP},
    {"down", XINPUT_GAMEPAD_DPAD_DOWN, VK_PAD_DPAD_DOWN},
    {"left", XINPUT_GAMEPAD_DPAD_LEFT, VK_PAD_DPAD_LEFT},
    {"right", XINPUT_GAMEPAD_DPAD_RIGHT, VK_PAD_DPAD_RIGHT},
};

// Present thread: the running script.
struct Segment {
    XINPUT_GAMEPAD pad{};
    int ms = 0;
};
std::vector<Segment> g_script;
std::size_t g_step = 0;
Clock::time_point g_stepEnd{};
int g_totalMs = 0;
std::function<void(const std::string&)> g_done;

// Sets the pad and queues a keystroke for every button edge. Thread: present thread.
void SetPad(const XINPUT_GAMEPAD& next)
{
    std::lock_guard lock(g_mutex);
    const WORD changed = static_cast<WORD>(g_pad.wButtons ^ next.wButtons);
    for (const Button& b : kButtons) {
        if (changed & b.mask) {
            XINPUT_KEYSTROKE k{};
            k.VirtualKey = b.vk;
            k.Flags = (next.wButtons & b.mask) ? XINPUT_KEYSTROKE_KEYDOWN : XINPUT_KEYSTROKE_KEYUP;
            k.UserIndex = static_cast<BYTE>(kPadUser);
            g_keys.push_back(k);
        }
    }
    if (std::memcmp(&g_pad, &next, sizeof(next)) != 0) {
        g_pad = next;
        ++g_packet;
    }
}

DWORD WINAPI HookedGetState(DWORD user, XINPUT_STATE* state)
{
    if (user == kPadUser && state) {
        std::lock_guard lock(g_mutex);
        state->dwPacketNumber = g_packet;
        state->Gamepad = g_pad;
        return ERROR_SUCCESS;
    }
    return g_getState(user, state);
}

DWORD WINAPI HookedGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps)
{
    if (user == kPadUser && caps) {
        *caps = {};
        caps->Type = XINPUT_DEVTYPE_GAMEPAD;
        caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        caps->Gamepad.wButtons = 0xf3ff;
        caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 0xff;
        caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY =
            static_cast<SHORT>(0xffc0);
        caps->Vibration.wLeftMotorSpeed = caps->Vibration.wRightMotorSpeed = 0xffff;
        return ERROR_SUCCESS;
    }
    return g_getCapabilities(user, flags, caps);
}

DWORD WINAPI HookedGetKeystroke(DWORD user, DWORD reserved, PXINPUT_KEYSTROKE key)
{
    if ((user == kPadUser || user == XUSER_INDEX_ANY) && key) {
        std::lock_guard lock(g_mutex);
        if (!g_keys.empty()) {
            *key = g_keys.front();
            g_keys.pop_front();
            return ERROR_SUCCESS;
        }
        if (user == kPadUser) {
            return ERROR_EMPTY;
        }
    }
    return g_getKeystroke(user, reserved, key);
}

SHORT Stick(float v)
{
    return static_cast<SHORT>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
}

// Parses one step; false + `error` on an unknown part.
bool ParseStep(std::string token, std::vector<Segment>& out, std::string& error)
{
    int ms = -1;
    if (const size_t colon = token.find(':'); colon != std::string::npos) {
        try {
            ms = std::stoi(token.substr(colon + 1));
        }
        catch (...) {
            error = token;
            return false;
        }
        token = token.substr(0, colon);
    }
    if (token == "wait" || (!token.empty() && std::all_of(token.begin(), token.end(), ::isdigit))) {
        const int wait = token == "wait" ? ms : std::stoi(token);
        out.push_back({{}, std::max(wait, 0)});
        return true;
    }
    Segment s;
    bool button = false;
    std::stringstream parts(token);
    std::string part;
    while (std::getline(parts, part, '+')) {
        if (const size_t eq = part.find('='); eq != std::string::npos) {
            float v = 0.0f;
            try {
                v = std::stof(part.substr(eq + 1));
            }
            catch (...) {
                error = part;
                return false;
            }
            const std::string axis = part.substr(0, eq);
            if (axis == "lt") {
                s.pad.bLeftTrigger = static_cast<BYTE>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
            }
            else if (axis == "rt") {
                s.pad.bRightTrigger = static_cast<BYTE>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
            }
            else if (axis == "lx") {
                s.pad.sThumbLX = Stick(v);
            }
            else if (axis == "ly") {
                s.pad.sThumbLY = Stick(v);
            }
            else if (axis == "rx") {
                s.pad.sThumbRX = Stick(v);
            }
            else if (axis == "ry") {
                s.pad.sThumbRY = Stick(v);
            }
            else {
                error = part;
                return false;
            }
            continue;
        }
        const auto it = std::find_if(std::begin(kButtons), std::end(kButtons),
            [&](const Button& b) { return part == b.name; });
        if (it == std::end(kButtons)) {
            error = part;
            return false;
        }
        s.pad.wButtons |= it->mask;
        button = true;
    }
    s.ms = ms >= 0 ? ms : kTapMs;
    out.push_back(s);
    if (button) {
        out.push_back({{}, kGapMs});
    }
    return true;
}

void Finish(const std::string& result)
{
    SetPad({});
    g_script.clear();
    g_step = 0;
    if (g_done) {
        auto done = std::move(g_done);
        g_done = nullptr;
        done(result);
    }
}

void* Export(HMODULE module, const char* name)
{
    return module ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr;
}

} // namespace

void InstallVirtualPad()
{
    if (g_installed || !core::TestMode()) {
        return;
    }
    g_installed = true;
    HMODULE xinput = GetModuleHandleW(L"xinput1_3.dll");
    if (!xinput) {
        xinput = LoadLibraryW(L"xinput1_3.dll"); // the game imports this one by name, so it gets the same module
    }
    using core::hooks::Result;
    const bool ok = xinput &&
                    core::hooks::Create("XInputGetState", Export(xinput, "XInputGetState"), &HookedGetState,
                        &g_getState) == Result::Ok &&
                    core::hooks::Create("XInputGetCapabilities", Export(xinput, "XInputGetCapabilities"),
                        &HookedGetCapabilities, &g_getCapabilities) == Result::Ok &&
                    core::hooks::Create("XInputGetKeystroke", Export(xinput, "XInputGetKeystroke"),
                        &HookedGetKeystroke, &g_getKeystroke) == Result::Ok;
    if (!ok) {
        TT2VR_LOG_ERROR("virtual pad: XInput hooks failed (xinput1_3.dll {}); pad scripts will not reach the game",
            xinput ? "found" : "missing");
        return;
    }
    TT2VR_LOG_INFO("virtual pad: TEST MODE, XInput user {} is the scripted pad", kPadUser);
}

void RunPadScript(const std::string& script, std::function<void(const std::string&)> done)
{
    if (!core::TestMode() || !g_getState) {
        done(core::TestMode() ? "pad FAILED: XInput hooks not installed"
                              : "pad FAILED: test mode only (launch with -tt2vr-test)");
        return;
    }
    std::vector<Segment> segments;
    std::string text = script;
    std::replace(text.begin(), text.end(), ',', ' ');
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::istringstream in(text);
    std::string token, error;
    while (in >> token) {
        if (!ParseStep(token, segments, error)) {
            done(fmt::format("pad FAILED: unknown step '{}'", error));
            return;
        }
    }
    if (segments.empty()) {
        done("pad FAILED: empty script");
        return;
    }
    if (g_done) {
        Finish("pad FAILED: superseded by a newer script");
    }
    g_script = std::move(segments);
    g_step = 0;
    g_totalMs = 0;
    for (const Segment& s : g_script) {
        g_totalMs += s.ms;
    }
    g_done = std::move(done);
    SetPad(g_script[0].pad);
    g_stepEnd = Clock::now() + std::chrono::milliseconds(g_script[0].ms);
}

void TickVirtualPad()
{
    if (g_script.empty()) {
        return;
    }
    const auto now = Clock::now();
    while (now >= g_stepEnd) {
        if (++g_step >= g_script.size()) {
            Finish(fmt::format("pad ok ({} segments, {} ms)", g_script.size(), g_totalMs));
            return;
        }
        SetPad(g_script[g_step].pad);
        g_stepEnd += std::chrono::milliseconds(g_script[g_step].ms);
    }
}

} // namespace game
