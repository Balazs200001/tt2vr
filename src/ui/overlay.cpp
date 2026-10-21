// overlay.cpp - ImGui overlay drawn into the game's back buffer just before Present, after the eye images were handed
// to the headset (d3d::present: xr::OnFrameEnd, mirror, then ui::Render), so it only ever shows on the desktop.
//
// Input rules (CLAUDE.md hard rule 5): the overlay never blocks or changes game input. We don't
// subclass the game window; ImGui reads the cursor position (imgui_impl_win32) and we poll the mouse
// buttons once per frame. It may not change the cursor (NoMouseCursorChange). Clicks also reach the game.
// The game ships its own Dear ImGui and imgui.ini; ours is a separate copy with no ini file.
#include "ui/overlay.hpp"

#include "core/config.hpp"
#include "core/hooks.hpp"
#include "core/log.hpp"
#include "d3d/blit.hpp"
#include "d3d/internal.hpp"
#include "d3d/present.hpp"
#include "d3d/renderdoc.hpp"
#include "d3d/shader_registry.hpp"
#include "game/camera.hpp"
#include "game/gpu_timers.hpp"
#include "game/menu_state.hpp"
#include "game/render_size.hpp"
#include "game/rider_view.hpp"
#include "game/split_view.hpp"
#include "game/temporal_debug.hpp"
#include "game/version.hpp"
#include "xr/session.hpp"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <spdlog/fmt/fmt.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace ui {
namespace {

using Microsoft::WRL::ComPtr;

bool g_initialized = false;
bool g_failed = false;
bool g_visible = true;
bool g_cursor = false;
HWND g_window = nullptr;
ComPtr<ID3D11RenderTargetView> g_rtv;
bool g_mouseDown[2] = {};

void PollMouseButtons()
{
    ImGuiIO& io = ImGui::GetIO();
    const bool focused = GetForegroundWindow() == g_window;
    const int keys[2] = {VK_LBUTTON, VK_RBUTTON};
    for (int i = 0; i < 2; ++i) {
        const bool down = focused && (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        if (down != g_mouseDown[i]) {
            io.AddMouseButtonEvent(i, down);
            g_mouseDown[i] = down;
        }
    }
}

// The game may render at a size other than its window (game::RenderSize); the window then stretches the back buffer
// (bw x bh) onto its client area (cw x ch). Panel text drawn in back-buffer pixels came out about half size and
// unevenly squashed (3540x1892 on 1920x1080, runtime 2026-10-21). So then the panel is drawn off-screen at
// kSupersample x the client size (style and fonts scaled to match; ImGui 1.92 rasterises glyphs at the scaled size)
// and composited over the whole back buffer: after the window's stretch it lands at client resolution, shape intact.
// Same size: drawn straight into the back buffer.
constexpr float kSupersample = 2.0f;

struct Target {
    bool offscreen = false;
    float bw = 0, bh = 0;
};

ComPtr<ID3D11Texture2D> g_uiTex;
ComPtr<ID3D11RenderTargetView> g_uiRtv;
ComPtr<ID3D11ShaderResourceView> g_uiSrv;
UINT g_uiW = 0, g_uiH = 0;
float g_scale = 1.0f;
d3d::Blitter g_blitter;

void ApplyScale(float scale)
{
    if (scale == g_scale) {
        return;
    }
    g_scale = scale;
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark(&style);
    style.Alpha = 0.92f;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

bool EnsureUiTarget(ID3D11Device* device, UINT w, UINT h)
{
    if (g_uiTex && g_uiW == w && g_uiH == h) {
        return true;
    }
    g_uiSrv.Reset();
    g_uiRtv.Reset();
    g_uiTex.Reset();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &g_uiTex)) ||
        FAILED(device->CreateRenderTargetView(g_uiTex.Get(), nullptr, &g_uiRtv)) ||
        FAILED(device->CreateShaderResourceView(g_uiTex.Get(), nullptr, &g_uiSrv))) {
        g_uiSrv.Reset();
        g_uiRtv.Reset();
        g_uiTex.Reset();
        TT2VR_LOG_ERROR("overlay: cannot create the {}x{} off-screen target", w, h);
        return false;
    }
    g_uiW = w;
    g_uiH = h;
    TT2VR_LOG_INFO("overlay: drawn off-screen at {}x{} ({}x the window's client size) and composited", w, h,
        kSupersample);
    return true;
}

// Sets the display size and mouse position for this frame's target.
Target MatchWindow(IDXGISwapChain* swapChain)
{
    Target t;
    DXGI_SWAP_CHAIN_DESC desc{};
    RECT client{};
    if (FAILED(swapChain->GetDesc(&desc)) || !GetClientRect(g_window, &client)) {
        ApplyScale(1.0f);
        return t;
    }
    const float cw = static_cast<float>(client.right - client.left);
    const float ch = static_cast<float>(client.bottom - client.top);
    t.bw = static_cast<float>(desc.BufferDesc.Width);
    t.bh = static_cast<float>(desc.BufferDesc.Height);
    if (cw <= 0 || ch <= 0 || (cw == t.bw && ch == t.bh)) {
        ApplyScale(1.0f);
        return t;
    }
    t.offscreen = true;
    ApplyScale(kSupersample);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(std::round(cw * kSupersample), std::round(ch * kSupersample));
    POINT p{};
    if (GetCursorPos(&p) && ScreenToClient(g_window, &p)) {
        io.AddMousePosEvent(static_cast<float>(p.x) * kSupersample, static_cast<float>(p.y) * kSupersample);
    }
    return t;
}

bool Init(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context)
{
    DXGI_SWAP_CHAIN_DESC desc{};
    swapChain->GetDesc(&desc);
    g_window = desc.OutputWindow;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().Alpha = 0.92f;
    if (!ImGui_ImplWin32_Init(g_window) || !ImGui_ImplDX11_Init(device, context)) {
        TT2VR_LOG_ERROR("overlay: ImGui backend init failed; overlay disabled");
        return false;
    }
    g_visible = core::GetConfig().overlay.visible;
    g_cursor = core::GetConfig().overlay.show_cursor;
    TT2VR_LOG_INFO("overlay initialized (visible={}, toggle {}; cursor={}, toggle {})", g_visible,
        core::FormatHotkey(core::GetConfig().hotkeys.overlay), g_cursor,
        core::FormatHotkey(core::GetConfig().hotkeys.cursor));
    return true;
}

void VersionLine(const game::VersionInfo& v)
{
    switch (v.status) {
    case game::VersionStatus::Known:
        ImGui::Text("Game: build %s (known exe%s)", v.steamBuild.c_str(), v.faked ? ", FAKED hash" : "");
        break;
    case game::VersionStatus::Unknown:
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "UNKNOWN game version (sha256 %.12s...%s)",
            v.sha256.c_str(), v.faked ? ", faked" : "");
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "Engine hooks off: D3D layer only, game runs flat.");
        break;
    case game::VersionStatus::NotChecked:
        ImGui::TextDisabled("Game: checking exe version...");
        break;
    }
}

// PLAN Phase 2: camera override test controls. Values apply from the next frame (read in Renderer::DoUpdate).
void CameraSection()
{
    if (!ImGui::CollapsingHeader("Camera (Phase 2)")) {
        return;
    }
    if (!game::CameraHooksActive()) {
        ImGui::TextDisabled("camera hooks not active (see log)");
        return;
    }
    game::CameraSettings& s = game::Settings();
    const game::CameraInfo info = game::LastCameraInfo();
    constexpr float kRadToDeg = 57.2957795f;
    if (info.valid) {
        ImGui::Text("hfov %.2f deg  near %.2f  far %.0f  viewport %dx%d", info.fov * kRadToDeg, info.nearZ, info.farZ,
            info.viewportW, info.viewportH);
        ImGui::Text("front pass %s: fov %.2f deg  near %.2f  far %.1f", info.frontPass ? "on" : "off",
            info.frontFov * kRadToDeg, info.frontNear, info.frontFar);
        ImGui::Text("pos %.2f %.2f %.2f  fwd %.3f %.3f %.3f", info.world[0][3], info.world[1][3], info.world[2][3],
            -info.world[0][2], -info.world[1][2], -info.world[2][2]);
        ImGui::Text("proj m02 %.4f  m12 %.4f  (%u calls reshaped)", info.proj[0][2], info.proj[1][2],
            info.projOverrides);
    }
    else {
        ImGui::TextDisabled("no main view yet (menu or loading)");
    }
    ImGui::Checkbox("Override view", &s.viewEnabled);
    ImGui::SliderFloat("yaw (left +)", &s.yawDeg, -180.0f, 180.0f, "%.1f deg");
    ImGui::SliderFloat("pitch (up +)", &s.pitchDeg, -90.0f, 90.0f, "%.1f deg");
    ImGui::SliderFloat("roll (left +)", &s.rollDeg, -180.0f, 180.0f, "%.1f deg");
    ImGui::SliderFloat3("offset x/y/z (m)", s.offset, -2.0f, 2.0f, "%.2f");
    if (game::HelmetHooksActive()) {
        ImGui::Checkbox("Helmet follows view", &s.helmetFollows);
        ImGui::SameLine();
        ImGui::TextDisabled("(%u moves/frame)", info.helmetMoves);
    }
    else {
        ImGui::TextDisabled("helmet hooks not active: helmet stays where the game puts it");
    }
    if (ImGui::Button("Reset view")) {
        s.yawDeg = s.pitchDeg = s.rollDeg = 0.0f;
        s.offset[0] = s.offset[1] = s.offset[2] = 0.0f;
    }
    ImGui::Checkbox("Override projection", &s.projEnabled);
    ImGui::SliderFloat("shift x (right +)", &s.shiftX, -0.5f, 0.5f, "%.2f of width");
    ImGui::SliderFloat("shift y (up +)", &s.shiftY, -0.5f, 0.5f, "%.2f of height");
    if (ImGui::Button("Reset projection")) {
        s.shiftX = s.shiftY = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Log camera")) {
        TT2VR_LOG_INFO("{}", game::DescribeCamera());
    }
}

// PLAN Phase 8: comfort settings that apply from the next frame (game::CameraSettings, main thread).
void ComfortSection()
{
    if (!ImGui::CollapsingHeader("Comfort (Phase 8)", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    if (!game::CameraHooksActive()) {
        ImGui::TextDisabled("camera hooks not active (see log)");
        return;
    }
    game::CameraSettings& s = game::Settings();
    const game::CameraInfo info = game::LastCameraInfo();
    ImGui::SliderFloat("horizon lock", &s.horizonLock, 0.0f, 100.0f, "%.0f %%");
    ImGui::Text("roll: game %.1f deg (max %.1f), shown %.1f deg (max %.1f)", info.gameRollDeg, info.gameRollMaxDeg,
        info.shownRollDeg, info.shownRollMaxDeg);
    ImGui::SliderFloat("shake", &s.shake, 0.0f, 100.0f, "%.0f %%");
    ImGui::Text("shake: game %.2f deg, shown max %.2f deg; noise bob %.3f roll %.4f", info.shakeDeg,
        info.shakeShownMaxDeg, info.speedNoise, info.rollNoise);
    ImGui::SliderFloat3("seat forward/up/right", s.seat, -0.5f, 0.5f, "%.2f m");
    ImGui::SameLine();
    if (ImGui::Button("reset##seat")) {
        game::ResetSeat();
    }
    ImGui::SliderFloat("world scale", &s.worldScale, 0.5f, 2.0f, "%.2f");
    ImGui::SliderFloat("IPD scale", &s.ipdScale, 0.5f, 2.0f, "%.2f");
    ImGui::TextDisabled("changes are saved to tt2vr\\user.toml");
    ImGui::TextDisabled("%s", game::DescribeRiderView(game::LastRiderView()).c_str());
}

// PLAN Phase 3: OpenXR session status, eye image size and frame pacing.
void XrSection()
{
    const xr::Status st = xr::GetStatus();
    if (!ImGui::CollapsingHeader("Headset (Phase 3)", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    ImGui::Text("XR: %s%s", st.state.c_str(), st.running ? " (running)" : "");
    if (!st.runtime.empty()) {
        ImGui::Text("runtime %s  headset %s", st.runtime.c_str(), st.system.empty() ? "-" : st.system.c_str());
    }
    if (!st.error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "last error: %s", st.error.c_str());
    }
    if (st.eyeW > 0) {
        ImGui::Text("eye %ux%u (recommended %ux%u)  format %d", st.eyeW, st.eyeH, st.recommendedW, st.recommendedH,
            st.format);
        ImGui::Text("back buffer %ux%u -> %s", st.backBufferW, st.backBufferH, st.copyMode);
        const std::uint32_t expectedW = st.eyeW * (game::SplitViewAvailable() ? 2u : 1u); // eyes side by side
        if (st.backBufferW != 0 && (st.backBufferW != expectedW || st.backBufferH != st.eyeH)) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "image is stretched (render size forcing %s)",
                !core::GetConfig().xr.force_render_size ? "off in config"
                : game::RenderSizeHookActive()          ? "pending or not applied, see log"
                                                        : "unavailable");
        }
        ImGui::Text("xrWaitFrame %.2f ms  period %.2f ms  render %s  tracked %s", st.waitMs, st.periodMs,
            st.shouldRender ? "yes" : "no", st.tracked ? "yes" : "no");
        const game::MenuState menu = game::LastMenuState();
        ImGui::Text("frames submitted %llu  showing %s%s", static_cast<unsigned long long>(st.framesSubmitted),
            st.presentation, st.hudLayer ? " + HUD panel" : "");
        ImGui::Text("menu state: %s (menu %d, paused %d, time stopped %d, riding camera %d)",
            !menu.available     ? "unavailable"
            : menu.RidingPause() ? "riding pause"
            : menu.Active()      ? "screen"
                                 : "riding",
            menu.menuOpen, menu.paused, menu.timeStopped, menu.springCamera);
        ImGui::TextUnformatted(game::SplitViewSummary().c_str());
    }
    if (ImGui::Button(fmt::format("Recenter ({})", core::FormatHotkey(core::GetConfig().hotkeys.recenter)).c_str())) {
        xr::RequestRecenter();
    }
    ImGui::SameLine();
    if (ImGui::Button("Restart XR")) {
        xr::RequestRestart();
    }
}

// PLAN 6f: per-eye history VRAM, history / motion-vector / previous-depth views, [debug] freeze of eye R's history.
void TemporalSection()
{
    static bool s_wasShown = false;
    if (game::TemporalViewsShown() && !s_wasShown) {
        ImGui::SetNextItemOpen(true); // views switched on (also by the remote "views on"): show them
    }
    s_wasShown = game::TemporalViewsShown();
    if (!ImGui::CollapsingHeader("Temporal (Phase 6)")) {
        return;
    }
    const game::TemporalVram v = game::TemporalVramUsage();
    if (!v.valid) {
        ImGui::TextDisabled("per-eye history: only while two views render");
    }
    else {
        ImGui::Text("per-eye history VRAM: eye L %.1f MB, eye R %.1f MB", v.totalMB[0], v.totalMB[1]);
        ImGui::Text("  TAA %.1f / %.1f, last frame %.1f / %.1f, froxel %.1f / %.1f, luminance %.3f / %.3f MB",
            v.taaMB[0], v.taaMB[1], v.lastFrameMB[0], v.lastFrameMB[1], v.froxelMB[0], v.froxelMB[1], v.luminanceMB[0],
            v.luminanceMB[1]);
        ImGui::Text("  debug views and copies: %.1f MB", v.debugMB);
    }
    bool shown = game::TemporalViewsShown();
    if (ImGui::Checkbox("Show history views", &shown)) {
        game::SetTemporalViewsShown(shown);
    }
    bool freeze = game::FreezeRightHistory();
    if (ImGui::Checkbox("[debug] Freeze eye R TAA history", &freeze)) {
        game::SetFreezeRightHistory(freeze);
    }
    unsigned w = 0, h = 0;
    game::TemporalThumbnailSize(w, h);
    if (!shown || w == 0 || !ImGui::BeginTable("temporal_views", 4, ImGuiTableFlags_SizingFixedFit)) {
        return;
    }
    ImGui::TableNextRow();
    for (const char* header : {"", "TAA history", "motion vectors", "previous depth"}) {
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(header);
    }
    for (int eye = 0; eye < 2; ++eye) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(eye == 0 ? "L" : "R");
        using game::TemporalView;
        for (const auto view : {TemporalView::History, TemporalView::Motion, TemporalView::PreviousDepth}) {
            ImGui::TableNextColumn();
            if (ID3D11ShaderResourceView* srv = game::TemporalThumbnail(eye, view)) {
                ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(srv)),
                    ImVec2(static_cast<float>(w), static_cast<float>(h)));
            }
            else {
                ImGui::TextDisabled("-");
            }
        }
    }
    ImGui::EndTable();
}

// PLAN Phase 9: frame time (white) and GPU frame time (green) of the last frames against the headset's frame budget
// (yellow line; the display period, or 60 Hz without a headset). The y axis runs to twice the budget; taller frames
// are clipped at the top. Main thread.
void FrameTimeGraph()
{
    const std::vector<d3d::FrameTimeSample> samples = d3d::RecentFrameTimes();
    if (samples.size() < 2) {
        return;
    }
    const double period = xr::GetStatus().periodMs;
    const float budget = period > 1.0 ? static_cast<float>(period) : 1000.0f / 60.0f;
    const float top = 2.0f * budget;
    int over = 0;
    float worst = 0.0f;
    for (const d3d::FrameTimeSample& s : samples) {
        over += s.frameMs > budget * 1.1f ? 1 : 0;
        worst = std::max(worst, s.frameMs);
    }
    ImGui::Text("frame time: %d of the last %zu frames over the %.2f ms budget, worst %.1f ms", over, samples.size(),
        budget, worst);
    const ImVec2 size(static_cast<float>(d3d::kFrameTimeSamples), 80.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p0, p1, IM_COL32(20, 20, 20, 200));
    const auto y = [&](float ms) { return p1.y - std::clamp(ms / top, 0.0f, 1.0f) * size.y; };
    draw->AddLine(ImVec2(p0.x, y(budget)), ImVec2(p1.x, y(budget)), IM_COL32(230, 200, 60, 255));
    const float dx = size.x / static_cast<float>(d3d::kFrameTimeSamples - 1);
    const float x0 = p1.x - dx * static_cast<float>(samples.size() - 1);
    for (std::size_t i = 1; i < samples.size(); ++i) {
        const float xa = x0 + dx * static_cast<float>(i - 1), xb = x0 + dx * static_cast<float>(i);
        if (samples[i - 1].gpuMs > 0.0f && samples[i].gpuMs > 0.0f) {
            draw->AddLine(ImVec2(xa, y(samples[i - 1].gpuMs)), ImVec2(xb, y(samples[i].gpuMs)), IM_COL32(80, 220, 120, 255));
        }
        draw->AddLine(ImVec2(xa, y(samples[i - 1].frameMs)), ImVec2(xb, y(samples[i].frameMs)), IM_COL32(240, 240, 240, 255));
    }
    ImGui::Dummy(size);
}

void DrawPanel()
{
    const auto stats = d3d::Stats();
    const auto& version = game::ExeVersion();
    const auto& hk = core::GetConfig().hotkeys;
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.8f);
    ImGui::Begin("tt2vr", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::Text("tt2vr 0.1.0 - Phase 3 (left eye head-tracked)");
    ImGui::Text("%.1f fps  %.2f ms  frame %llu  %ux%u", stats.fps, stats.ms,
        static_cast<unsigned long long>(stats.frame), stats.width, stats.height);
    if (const game::GpuTimes gpu = game::LastGpuTimes(); gpu.valid && gpu.views >= 2) {
        ImGui::Text("GPU: view 0 %.2f ms  view 1 %.2f ms  frame %.2f ms", gpu.viewMs[0], gpu.viewMs[1], gpu.frameMs);
    }
    else if (gpu.valid) {
        ImGui::Text("GPU: view 0 %.2f ms  frame %.2f ms", gpu.viewMs[0], gpu.frameMs);
    }
    FrameTimeGraph();
    VersionLine(version);
    const int failed = core::hooks::FailedCount();
    const auto installed = core::hooks::Installed().size();
    if (failed > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Hooks: %zu installed, %d FAILED (see log)", installed, failed);
    }
    else {
        ImGui::Text("Hooks: %zu installed, 0 failed", installed);
    }
    const auto& warnings = core::ConfigWarnings();
    if (!warnings.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Config: %zu warning(s), defaults used for those keys:",
            warnings.size());
        for (const auto& w : warnings) {
            ImGui::BulletText("%s", w.c_str());
        }
    }
    ImGui::Text("Shaders: %zu unique", d3d::shaders::Count());
    ImGui::Text("RenderDoc: %s", d3d::renderdoc::Available() ? "attached" : "not attached");
    ImGui::Separator();
    if (ImGui::Button(fmt::format("Frame trace ({})", core::FormatHotkey(hk.frame_trace)).c_str())) {
        d3d::RequestFrameTrace("overlay");
    }
    ImGui::SameLine();
    if (ImGui::Button(fmt::format("Texture dump ({})", core::FormatHotkey(hk.texture_dump)).c_str())) {
        d3d::RequestTextureDump("overlay");
    }
    if (d3d::renderdoc::Available()) {
        ImGui::SameLine();
        if (ImGui::Button(fmt::format("RenderDoc capture ({})", core::FormatHotkey(hk.renderdoc_capture)).c_str())) {
            d3d::RequestRenderDocCapture("overlay", "tt2vr");
        }
    }
    if (ImGui::Button("Write shader registry")) {
        d3d::WriteShaderRegistry("overlay");
    }
    XrSection();
    ComfortSection();
    CameraSection();
    TemporalSection();
    ImGui::Checkbox(fmt::format("Show mouse cursor ({})", core::FormatHotkey(hk.cursor)).c_str(), &g_cursor);
    bool verbose = core::GetVerbose();
    if (ImGui::Checkbox("Verbose per-frame log", &verbose)) {
        core::SetVerbose(verbose);
    }
    const auto results = d3d::RecentResults();
    if (!results.empty()) {
        ImGui::Separator();
        for (const auto& r : results) {
            ImGui::TextWrapped("%s", r.c_str());
        }
    }
    ImGui::TextDisabled("%s hides this panel", core::FormatHotkey(hk.overlay).c_str());
    ImGui::End();
}

// Shown even with the panel hidden: the flat-fallback notice (PLAN v1: "on-screen notice").
void DrawNotice()
{
    const auto& version = game::ExeVersion();
    const std::size_t warnings = core::ConfigWarnings().size();
    if (version.status != game::VersionStatus::Unknown && core::hooks::FailedCount() == 0 && warnings == 0) {
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##tt2vr_notice", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
            ImGuiWindowFlags_NoSavedSettings);
    if (version.status == game::VersionStatus::Unknown) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "tt2vr: unknown game version - running flat (D3D layer only)");
    }
    if (core::hooks::FailedCount() > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "tt2vr: %d hook(s) failed - see tt2vr/logs/tt2vr.log",
            core::hooks::FailedCount());
    }
    if (warnings > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "tt2vr: %zu config warning(s) - press %s for details",
            warnings, core::FormatHotkey(core::GetConfig().hotkeys.overlay).c_str());
    }
    ImGui::End();
}

} // namespace

void Render(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (g_failed || !swapChain || !device || !context) {
        return;
    }
    d3d::InternalScope internal; // keep the overlay's own draws out of traces and dumps
    if (!g_initialized) {
        g_initialized = Init(swapChain, device, context);
        g_failed = !g_initialized;
        if (g_failed) {
            return;
        }
    }
    if (!g_rtv) {
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&back))) ||
            FAILED(device->CreateRenderTargetView(back.Get(), nullptr, &g_rtv))) {
            return;
        }
    }
    game::UpdateTemporalViews(device, context); // thumbnails for this frame's panel
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    const Target target = MatchWindow(swapChain);
    if (target.offscreen &&
        !EnsureUiTarget(device, static_cast<UINT>(ImGui::GetIO().DisplaySize.x),
            static_cast<UINT>(ImGui::GetIO().DisplaySize.y))) {
        return;
    }
    PollMouseButtons();
    // NoMouseCursorChange keeps imgui_impl_win32 from calling SetCursor, so this only adds ImGui's own drawing.
    ImGui::GetIO().MouseDrawCursor = g_cursor;
    ImGui::NewFrame();
    if (g_visible) {
        DrawPanel();
    }
    else {
        DrawNotice();
    }
    ImGui::Render();

    // imgui_impl_dx11 restores the state it touches except the output merger targets; we do those.
    ID3D11RenderTargetView* oldRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* oldDsv = nullptr;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtvs, &oldDsv);
    if (target.offscreen) {
        constexpr float kTransparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        context->ClearRenderTargetView(g_uiRtv.Get(), kTransparent); // ImGui's blend leaves premultiplied colour
        context->OMSetRenderTargets(1, g_uiRtv.GetAddressOf(), nullptr);
    }
    else {
        context->OMSetRenderTargets(1, g_rtv.GetAddressOf(), nullptr);
    }
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtvs, oldDsv);
    if (target.offscreen && (g_blitter.Ready() || g_blitter.Init(device))) {
        g_blitter.Composite(context, g_uiSrv.Get(), g_rtv.Get(), {0.0f, 0.0f, target.bw, target.bh, 0.0f, 1.0f});
    }
    for (auto* rtv : oldRtvs) {
        if (rtv) {
            rtv->Release();
        }
    }
    if (oldDsv) {
        oldDsv->Release();
    }
}

void ReleaseTargets()
{
    g_rtv.Reset();
}

void ToggleVisible()
{
    g_visible = !g_visible;
}

bool Visible()
{
    return g_visible;
}

void ToggleCursor()
{
    g_cursor = !g_cursor;
}

} // namespace ui
