#include "game/hud_capture.hpp"

#include "core/hooks.hpp"
#include "core/log.hpp"
#include "game/engine_layout.hpp"
#include "game/menu_state.hpp"
#include "game/signatures.hpp"
#include "game/split_view.hpp"
#include "xr/session.hpp"

#include <spdlog/fmt/fmt.h>
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace game {
namespace {

using namespace layout;

using HudRenderFn = void (*)(void* hud, void* rt, void* list, void* loop, void* ctx, const void* cameraDesc, int flags,
    bool flag);
// Static; HudNode::TargetInfo (32 bytes, layout width / height first) is returned through the hidden pointer.
using GetTargetInfoFn = void* (*)(void* out);

HudRenderFn g_render = nullptr;
GetTargetInfoFn g_getTargetInfo = nullptr;
void* const* g_context = nullptr; // &GameContext::s_pInstance
void* g_renderDesc = nullptr; // the main renderer's RenderDesc while DeferredRenderLoop::Render records (main thread)
void* g_viewLists[2] = {}; // its two ViewContexts' HUD lists (+0x5e8) at the start of that call

enum class Kind : std::uint8_t { Global, View, Baker, Other };

struct Call {
    Kind kind = Kind::Other;
    int view = -1; // camera index, Kind::View
    bool gameGlobal = false; // the list is GameContext::GetHudItemList(-1)
    int flags = 0;
    std::uint32_t rtW = 0, rtH = 0;
    int hudW = -1, hudH = -1; // HudRenderer+4/+8 at the call (-1 = window layout)
};

constexpr std::size_t kMaxCalls = 32;
constexpr int kMaxLogLines = 200;
constexpr auto kLogInterval = std::chrono::milliseconds(500);

std::mutex g_mutex;
std::vector<Call> g_frame; // the frame being recorded
std::vector<Call> g_last; // the last complete frame
std::size_t g_dropped = 0;
std::vector<std::function<void(const std::string&)>> g_reports;
// Change log (main thread): one line when the state or the set of calls changes, rate-limited.
std::string g_lastLogged;
std::chrono::steady_clock::time_point g_lastLogTime;
int g_logLines = 0;

// SEH guard: engine objects are only read.
bool Classify(void* hud, void* rt, void* list, void* loop, void* ctx, Call& call)
{
    __try {
        call.hudW = Read<int>(hud, kHudRendererHudSize);
        call.hudH = Read<int>(hud, kHudRendererHudSize + 4);
        if (rt) {
            call.rtW = Read<std::uint32_t>(rt, kRenderTargetWidth);
            call.rtH = Read<std::uint32_t>(rt, kRenderTargetHeight);
        }
        if (!loop) {
            call.kind = Kind::Baker;
        }
        else if (g_renderDesc && list == Read<void*>(g_renderDesc, kRenderDescGlobalHudList)) {
            call.kind = Kind::Global;
        }
        else if (list && (list == g_viewLists[0] || list == g_viewLists[1])) {
            call.kind = Kind::View;
            call.view = ctx ? Read<std::uint16_t>(ctx, kRenderContextCameraIndex) : -1;
        }
        void* context = g_context ? *g_context : nullptr;
        call.gameGlobal = context && list && list == Read<void*>(context, kGameContextGlobalHudList);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WindowLayout(std::uint32_t& w, std::uint32_t& h)
{
    if (!g_getTargetInfo) {
        return false;
    }
    __try {
        alignas(16) std::uint32_t info[8] = {};
        g_getTargetInfo(info);
        w = info[0];
        h = info[1];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Thread: game main thread (frame recording; TextureBaker bakes go through here too).
void HookedHudRender(void* hud, void* rt, void* list, void* loop, void* ctx, const void* cameraDesc, int flags,
    bool flag)
{
    Call call;
    call.flags = flags;
    if (hud && Classify(hud, rt, list, loop, ctx, call)) {
        std::lock_guard lock(g_mutex);
        if (g_frame.size() < kMaxCalls) {
            g_frame.push_back(call); // capacity reserved at install: no allocation here
        }
        else {
            ++g_dropped;
        }
    }
    g_render(hud, rt, list, loop, ctx, cameraDesc, flags, flag);
}

std::string KindName(const Call& c)
{
    switch (c.kind) {
    case Kind::Global:
        return "global";
    case Kind::View:
        return fmt::format("view{}", c.view);
    case Kind::Baker:
        return "baker";
    default:
        return "other";
    }
}

std::string Describe(const std::vector<Call>& calls, std::size_t dropped)
{
    std::string s;
    for (const Call& c : calls) {
        s += fmt::format(" [{}{} flags=0x{:x} rt={}x{} layout={}]", KindName(c), c.gameGlobal ? " gamecontext" : "",
            c.flags, c.rtW, c.rtH, c.hudW < 0 || c.hudH < 0 ? std::string("window") : fmt::format("{}x{}", c.hudW, c.hudH));
    }
    if (dropped) {
        s += fmt::format(" (+{} not recorded)", dropped);
    }
    return s.empty() ? " none" : s;
}

std::string State()
{
    const MenuState m = LastMenuState();
    std::uint32_t w = 0, h = 0;
    const std::string layout = WindowLayout(w, h) ? fmt::format("{}x{}", w, h) : std::string("?");
    return fmt::format("presentation {}, two views {}, menu open {}, paused {}, time stopped {}, riding camera {}; "
                       "window layout {}",
        xr::GetStatus().presentation, SplitViewActive(), m.menuOpen, m.paused, m.timeStopped, m.springCamera, layout);
}

} // namespace

void InstallHudCapture()
{
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;
    g_context = static_cast<void* const*>(SignatureAddress(Sig::GameContextInstance));
    g_getTargetInfo = reinterpret_cast<GetTargetInfoFn>(SignatureAddress(Sig::HudNodeGetTargetInfo));
    g_frame.reserve(kMaxCalls);
    g_last.reserve(kMaxCalls);
    void* target = SignatureAddress(Sig::HudRendererRender);
    if (!target ||
        core::hooks::Create("HudRenderer::Render", target, &HookedHudRender, &g_render) != core::hooks::Result::Ok) {
        TT2VR_LOG_WARN("hud: HudRenderer::Render hook not installed; the HUD stays in the eye images");
        return;
    }
    TT2VR_LOG_INFO("hud: probe installed (HudRenderer::Render)");
}

void BeginHudLoopRender(void* renderer, void* renderDesc)
{
    g_renderDesc = renderDesc;
    g_viewLists[0] = g_viewLists[1] = nullptr;
    if (renderer) {
        __try {
            for (int k = 0; k < 2; ++k) {
                g_viewLists[k] = Read<void*>(renderer, kRendererViewContexts + k * kViewContextSize + kViewHudList);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_viewLists[0] = g_viewLists[1] = nullptr;
        }
    }
}

void EndHudLoopRender()
{
    g_renderDesc = nullptr;
    g_viewLists[0] = g_viewLists[1] = nullptr;
}

void OnHudPresent()
{
    if (!g_render) {
        return;
    }
    std::vector<std::function<void(const std::string&)>> reports;
    std::size_t dropped = 0;
    std::string calls;
    {
        std::lock_guard lock(g_mutex);
        g_last.swap(g_frame);
        g_frame.clear();
        dropped = g_dropped;
        g_dropped = 0;
        reports.swap(g_reports);
        calls = Describe(g_last, dropped);
    }
    const std::string text = State() + "; calls:" + calls;
    const auto now = std::chrono::steady_clock::now();
    if (text != g_lastLogged && g_logLines < kMaxLogLines && now - g_lastLogTime >= kLogInterval) {
        g_lastLogged = text;
        g_lastLogTime = now;
        if (++g_logLines == kMaxLogLines) {
            TT2VR_LOG_INFO("hud: {} (further changes not logged)", text);
        }
        else {
            TT2VR_LOG_INFO("hud: {}", text);
        }
    }
    for (auto& report : reports) {
        report("hud ok: " + text);
    }
}

void RequestHudReport(std::function<void(const std::string&)> done)
{
    if (!g_render) {
        done("hud FAILED: HudRenderer::Render is not hooked (signature missing or unknown game version)");
        return;
    }
    std::lock_guard lock(g_mutex);
    g_reports.push_back(std::move(done));
}

} // namespace game
