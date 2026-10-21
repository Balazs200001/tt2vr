#include "game/hud_capture.hpp"

#include "core/config.hpp"
#include "core/hooks.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "d3d/blit.hpp"
#include "d3d/internal.hpp"
#include "d3d/texture_dump.hpp"
#include "game/camera.hpp"
#include "game/engine_layout.hpp"
#include "game/engine_textures.hpp"
#include "game/menu_state.hpp"
#include "game/signatures.hpp"
#include "game/split_view.hpp"
#include "xr/session.hpp"

#include <spdlog/fmt/fmt.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <vector>

namespace game {
namespace {

using namespace layout;
using Microsoft::WRL::ComPtr;

using HudRenderFn = void (*)(void* hud, void* rt, void* list, void* loop, void* ctx, const void* cameraDesc, int flags,
    bool flag);
// Static; HudNode::TargetInfo (32 bytes, layout width / height first) is returned through the hidden pointer.
using GetTargetInfoFn = void* (*)(void* out);
// (RenderTargetManager*, StrongPtr<RenderTarget>& out, const RenderTarget::Desc&) -> 0 on success.
using CreateRenderTargetFn = int (*)(void* rtm, void** renderTarget, const void* desc);

HudRenderFn g_render = nullptr;
GetTargetInfoFn g_getTargetInfo = nullptr;
CreateRenderTargetFn g_createRenderTarget = nullptr;
void* const* g_rtm = nullptr; // &RenderTargetManager::s_pSingletonInstance
void* const* g_context = nullptr; // &GameContext::s_pInstance
void* g_renderDesc = nullptr; // the main renderer's RenderDesc while DeferredRenderLoop::Render records (main thread)
void* g_viewLists[2] = {}; // its two ViewContexts' HUD lists (+0x5e8) at the start of that call

// ---- 7a probe

enum class Kind : std::uint8_t { Global, View, Baker, Other };

struct Call {
    Kind kind = Kind::Other;
    int view = -1; // camera index, Kind::View
    bool gameGlobal = false; // the list is GameContext::GetHudItemList(-1)
    bool captured = false; // drawn into the capture targets instead of `rt`
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

// ---- 7b capture (main thread)

struct Target {
    void* rt = nullptr; // engine RenderTarget*; the mod holds its one reference for the life of the game
    ID3D11Texture2D* texture = nullptr; // the engine's texture behind it (re-read every frame: a device reset)
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11RenderTargetView> rtv;
};
Target g_targets[2]; // [0] cleared to transparent black, [1] to transparent white
constexpr const char* kTargetNames[2] = {"tt2vr HUD black", "tt2vr HUD white"};
bool g_createTried = false;
bool g_ready = false;
std::uint32_t g_width = 0, g_height = 0; // target pixels
int g_layoutW = 0, g_layoutH = 0; // HudRenderer layout during the capture
bool g_viewDrawn = false; // this frame recorded a view's HUD call (a 3D frame; loading screens have none)
bool g_captured = false; // this frame's global call drew into the targets
bool g_capturedMenu = false; // ... during the riding pause
std::uint64_t g_capturedFrames = 0;
ID3D11Device* g_device = nullptr; // the game's; not owned
d3d::Blitter g_blitter;
ComPtr<ID3D11Texture2D> g_resolved; // premultiplied colour and alpha
ComPtr<ID3D11ShaderResourceView> g_resolvedSrv;
ComPtr<ID3D11RenderTargetView> g_resolvedRtv;
HudImage g_current;
std::vector<std::function<void(const std::string&)>> g_saves;
int g_saveWait = 0; // frames the pending saves have waited for a captured HUD
constexpr int kSaveTimeoutFrames = 600;

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

// HudRenderer+4/+8 := (w, h); the previous values go to `old` if given. SEH guard: engine object.
bool SetHudSize(void* hud, int w, int h, int* old)
{
    __try {
        if (old) {
            old[0] = Read<int>(hud, kHudRendererHudSize);
            old[1] = Read<int>(hud, kHudRendererHudSize + 4);
        }
        Write<int>(hud, kHudRendererHudSize, w);
        Write<int>(hud, kHudRendererHudSize + 4, h);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CaptureWanted()
{
    return g_ready && core::GetConfig().hud.enabled && xr::SessionRunning() && SplitViewActive();
}

// Thread: game main thread (frame recording; TextureBaker bakes go through here too).
void HookedHudRender(void* hud, void* rt, void* list, void* loop, void* ctx, const void* cameraDesc, int flags,
    bool flag)
{
    Call call;
    call.flags = flags;
    const bool known = hud && Classify(hud, rt, list, loop, ctx, call);
    g_viewDrawn = g_viewDrawn || (known && call.kind == Kind::View);
    call.captured = known && call.kind == Kind::Global && (flags & 2) && g_viewDrawn && CaptureWanted();
    if (known) {
        std::lock_guard lock(g_mutex);
        if (g_frame.size() < kMaxCalls) {
            g_frame.push_back(call); // capacity reserved at install: no allocation here
        }
        else {
            ++g_dropped;
        }
    }
    int old[2] = {-1, -1};
    if (!call.captured || !SetHudSize(hud, g_layoutW, g_layoutH, old)) {
        g_render(hud, rt, list, loop, ctx, cameraDesc, flags, flag);
        return;
    }
    // The same list twice (TextureBaker renders one list once per tile the same way).
    g_render(hud, g_targets[0].rt, list, loop, ctx, cameraDesc, flags, flag);
    g_render(hud, g_targets[1].rt, list, loop, ctx, cameraDesc, flags, flag);
    SetHudSize(hud, old[0], old[1], nullptr);
    g_captured = true;
    g_capturedMenu = LastMenuState().RidingPause() || MenuFreezeActive();
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
        s += fmt::format(" [{}{}{} flags=0x{:x} rt={}x{} layout={}]", KindName(c), c.gameGlobal ? " gamecontext" : "",
            c.captured ? " captured" : "", c.flags, c.rtW, c.rtH,
            c.hudW < 0 || c.hudH < 0 ? std::string("window") : fmt::format("{}x{}", c.hudW, c.hudH));
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
    const std::string capture = g_ready ? fmt::format("ready {}x{} layout {}x{}", g_width, g_height, g_layoutW, g_layoutH)
                                : g_createTried ? std::string("failed")
                                                : std::string("not created");
    return fmt::format("presentation {}, two views {}, menu open {}, paused {}, time stopped {}, riding camera {}; "
                       "window layout {}; capture {}",
        xr::GetStatus().presentation, SplitViewActive(), m.menuOpen, m.paused, m.timeStopped, m.springCamera, layout,
        capture);
}

// SEH guard: the engine call.
bool CreateEngineTarget(void* rtm, const char* name, std::uint32_t w, std::uint32_t h, void*& out)
{
    alignas(16) unsigned char desc[kRtDescSize] = {};
    Write<int>(desc, kRtDescCount, 1);
    Write<int>(desc, kRtDescWidth, static_cast<int>(w));
    Write<int>(desc, kRtDescHeight, static_cast<int>(h));
    Write<int>(desc, kRtDescMips, 1);
    Write<std::uint32_t>(desc, kRtDescFormat, kRtFormatRgba);
    Write<std::uint8_t>(desc, kRtDescSrgb, 1);
    Write<std::uint8_t>(desc, kRtDescSrgb2, 1);
    Write<const char*>(desc, kRtDescName, name);
    Write<std::uint8_t>(desc, kRtDescFlag81, 1);
    __try {
        void* rt = nullptr;
        if (g_createRenderTarget(rtm, &rt, desc) != 0 || !rt) {
            return false;
        }
        out = rt;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// _SRGB views of the engine texture behind `t` (recreated when the engine replaced the texture).
bool MakeViews(Target& t)
{
    ID3D11Texture2D* texture = RenderTargetTexture(t.rt);
    if (!texture || !g_device) {
        return false;
    }
    if (texture == t.texture && t.srv && t.rtv) {
        return true;
    }
    t.srv.Reset();
    t.rtv.Reset();
    t.texture = nullptr;
    D3D11_TEXTURE2D_DESC d{};
    texture->GetDesc(&d);
    const bool srgb = d3d::IsSrgb(d.Format) || d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    constexpr UINT kBind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (!srgb || !d3d::SameFamily(d.Format, DXGI_FORMAT_R8G8B8A8_UNORM) || (d.BindFlags & kBind) != kBind) {
        TT2VR_LOG_ERROR("hud: capture target {}x{} has format {} bind 0x{:x} (need sRGB RGBA8, shader resource and "
                        "render target); HUD stays in the eye images",
            d.Width, d.Height, static_cast<int>(d.Format), d.BindFlags);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    D3D11_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(g_device->CreateShaderResourceView(texture, &sv, &t.srv)) ||
        FAILED(g_device->CreateRenderTargetView(texture, &rv, &t.rtv))) {
        TT2VR_LOG_ERROR("hud: cannot create views of a capture target; HUD stays in the eye images");
        t.srv.Reset();
        t.rtv.Reset();
        return false;
    }
    t.texture = texture;
    return true;
}

void ClearTargets(ID3D11DeviceContext* context)
{
    d3d::InternalScope internal;
    constexpr float kBlack[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    constexpr float kWhite[4] = {1.0f, 1.0f, 1.0f, 0.0f}; // same alpha as black: the masks read dest alpha
    context->ClearRenderTargetView(g_targets[0].rtv.Get(), kBlack);
    context->ClearRenderTargetView(g_targets[1].rtv.Get(), kWhite);
}

void CreateTargets()
{
    g_createTried = true;
    void* rtm = g_rtm ? *g_rtm : nullptr;
    if (!g_render || !g_createRenderTarget || !rtm) {
        TT2VR_LOG_WARN("hud: capture unavailable (signature or RenderTargetManager missing); HUD stays in the eye "
                       "images");
        return;
    }
    const auto& cfg = core::GetConfig().hud;
    g_width = static_cast<std::uint32_t>(cfg.pixels);
    g_height = std::max<std::uint32_t>(64, static_cast<std::uint32_t>(std::lround(cfg.pixels / cfg.aspect)));
    g_layoutH = 1080;
    g_layoutW = static_cast<int>(std::lround(1080.0f * cfg.aspect));
    for (int i = 0; i < 2; ++i) {
        if (!CreateEngineTarget(rtm, kTargetNames[i], g_width, g_height, g_targets[i].rt)) {
            TT2VR_LOG_ERROR("hud: RenderTargetManager::CreateRenderTarget failed for '{}'; HUD stays in the eye "
                            "images",
                kTargetNames[i]);
            return;
        }
    }
    ID3D11Texture2D* texture = RenderTargetTexture(g_targets[0].rt);
    if (!texture) {
        TT2VR_LOG_ERROR("hud: capture target has no texture; HUD stays in the eye images");
        return;
    }
    ComPtr<ID3D11Device> device;
    texture->GetDevice(&device);
    g_device = device.Get(); // the game's device outlives the mod's use of it
    if (!MakeViews(g_targets[0]) || !MakeViews(g_targets[1])) {
        return;
    }
    D3D11_TEXTURE2D_DESC rd{};
    rd.Width = g_width;
    rd.Height = g_height;
    rd.MipLevels = 1;
    rd.ArraySize = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    rd.SampleDesc.Count = 1;
    rd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    D3D11_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(g_device->CreateTexture2D(&rd, nullptr, &g_resolved)) ||
        FAILED(g_device->CreateShaderResourceView(g_resolved.Get(), &sv, &g_resolvedSrv)) ||
        FAILED(g_device->CreateRenderTargetView(g_resolved.Get(), &rv, &g_resolvedRtv))) {
        TT2VR_LOG_ERROR("hud: cannot create the resolved HUD texture; HUD stays in the eye images");
        return;
    }
    ComPtr<ID3D11DeviceContext> context;
    g_device->GetImmediateContext(&context);
    ClearTargets(context.Get());
    g_ready = true;
    TT2VR_LOG_INFO("hud: capture targets {}x{} (layout {}x{}), engine texture format {}", g_width, g_height, g_layoutW,
        g_layoutH, [] {
            D3D11_TEXTURE2D_DESC d{};
            g_targets[0].texture->GetDesc(&d);
            return static_cast<int>(d.Format);
        }());
}

// "hud save": the resolved image over a checkerboard and its alpha, from a staging copy (stalls once).
std::string SaveResolved(ID3D11DeviceContext* context)
{
    D3D11_TEXTURE2D_DESC d{};
    g_resolved->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(g_device->CreateTexture2D(&d, nullptr, &staging))) {
        return "hud FAILED: staging texture";
    }
    d3d::InternalScope internal;
    context->CopyResource(staging.Get(), g_resolved.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        return "hud FAILED: map";
    }
    const std::size_t pixels = static_cast<std::size_t>(d.Width) * d.Height;
    std::vector<std::uint8_t> color(pixels * 3), alpha(pixels * 3);
    std::uint64_t covered = 0;
    for (UINT y = 0; y < d.Height; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch;
        for (UINT x = 0; x < d.Width; ++x) {
            const std::uint8_t* p = row + x * 4; // RGBA, premultiplied (sRGB-encoded colour)
            const int a = p[3];
            covered += a > 8 ? 1 : 0;
            const int checker = ((x / 32 + y / 32) & 1) ? 160 : 96;
            const std::size_t o = (static_cast<std::size_t>(y) * d.Width + x) * 3;
            for (int c = 0; c < 3; ++c) { // BGR out; over the checker in encoded space (a preview)
                color[o + c] = static_cast<std::uint8_t>(std::min(255, p[2 - c] + checker * (255 - a) / 255));
                alpha[o + c] = static_cast<std::uint8_t>(a);
            }
        }
    }
    context->Unmap(staging.Get(), 0);
    const auto dir = core::CaptureDir() / "screens";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ok = d3d::dump::WritePngBgr(dir / "hud_color.png", d.Width, d.Height, color) &&
                    d3d::dump::WritePngBgr(dir / "hud_alpha.png", d.Width, d.Height, alpha);
    if (SUCCEEDED(co)) {
        CoUninitialize();
    }
    if (!ok) {
        return fmt::format("hud FAILED: cannot write {}", dir.string());
    }
    return fmt::format("hud ok: {} and hud_alpha.png ({}x{}, {:.1f}% of the pixels covered)",
        (dir / "hud_color.png").string(), d.Width, d.Height, 100.0 * static_cast<double>(covered) / pixels);
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
    g_createRenderTarget =
        reinterpret_cast<CreateRenderTargetFn>(SignatureAddress(Sig::RenderTargetManagerCreateRenderTarget));
    g_rtm = static_cast<void* const*>(SignatureAddress(Sig::RenderTargetManagerInstance));
    g_frame.reserve(kMaxCalls);
    g_last.reserve(kMaxCalls);
    void* target = SignatureAddress(Sig::HudRendererRender);
    if (!target ||
        core::hooks::Create("HudRenderer::Render", target, &HookedHudRender, &g_render) != core::hooks::Result::Ok) {
        TT2VR_LOG_WARN("hud: HudRenderer::Render hook not installed; the HUD stays in the eye images");
        return;
    }
    TT2VR_LOG_INFO("hud: HudRenderer::Render hooked (probe; capture {})",
        core::GetConfig().hud.enabled ? "once the headset session runs" : "off: [hud] enabled = false");
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

void OnHudFramePresent(ID3D11DeviceContext* context)
{
    g_current = {};
    if (!g_captured || !context) {
        g_captured = false;
        return;
    }
    g_captured = false;
    if (!MakeViews(g_targets[0]) || !MakeViews(g_targets[1]) || (!g_blitter.Ready() && !g_blitter.Init(g_device))) {
        g_ready = false; // nothing would clear the targets: stop capturing (the HUD returns to the eye images)
        TT2VR_LOG_ERROR("hud: resolve unavailable; capture stopped, HUD back in the eye images");
        return;
    }
    if (!g_blitter.Matte(context, g_targets[0].srv.Get(), g_targets[1].srv.Get(), g_resolvedRtv.Get(), g_width,
            g_height)) {
        return;
    }
    if (++g_capturedFrames == 1) {
        TT2VR_LOG_INFO("hud: first HUD frame captured; the eye images carry no 2D HUD while two views ride");
    }
    g_current = {g_targets[0].srv.Get(), g_targets[1].srv.Get(), g_resolvedSrv.Get(), g_width, g_height, g_capturedMenu};
    if (!g_saves.empty()) {
        const std::string result = SaveResolved(context);
        TT2VR_LOG_INFO("{}", result);
        for (auto& done : g_saves) {
            done(result);
        }
        g_saves.clear();
    }
}

HudImage CurrentHud()
{
    return g_current;
}

bool HudCaptureReady()
{
    return g_ready && core::GetConfig().hud.enabled;
}

void EndHudFrame(ID3D11DeviceContext* context)
{
    if (g_current && context) {
        ClearTargets(context);
    }
    g_current = {};
}

void OnHudPresent()
{
    if (!g_render) {
        return;
    }
    if (!g_createTried && core::GetConfig().hud.enabled && xr::SessionRunning()) {
        CreateTargets();
    }
    if (!g_saves.empty() && ++g_saveWait > kSaveTimeoutFrames) {
        for (auto& done : g_saves) {
            done("hud FAILED: no HUD captured in 600 frames (capture runs while two views ride in the headset)");
        }
        g_saves.clear();
    }
    g_viewDrawn = false;
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

void RequestHudSave(std::function<void(const std::string&)> done)
{
    if (!g_ready) {
        done("hud FAILED: no capture targets (they are made once the headset session runs; see the log)");
        return;
    }
    if (g_saves.empty()) {
        g_saveWait = 0;
    }
    g_saves.push_back(std::move(done));
}

} // namespace game
