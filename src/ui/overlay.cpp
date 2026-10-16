// overlay.cpp - ImGui overlay drawn into the game's back buffer just before Present.
//
// Input rules (CLAUDE.md hard rule 5): the overlay never blocks or changes game input. We don't
// subclass the game window; ImGui reads the cursor position (imgui_impl_win32) and we poll the mouse
// buttons once per frame. It may not change the cursor (NoMouseCursorChange). Clicks also reach the game.
// The game ships its own Dear ImGui and imgui.ini; ours is a separate copy with no ini file.
#include "ui/overlay.hpp"

#include "core/config.hpp"
#include "core/hooks.hpp"
#include "core/log.hpp"
#include "d3d/internal.hpp"
#include "d3d/present.hpp"
#include "d3d/renderdoc.hpp"
#include "d3d/shader_registry.hpp"
#include "game/camera.hpp"
#include "game/menu_state.hpp"
#include "game/version.hpp"
#include "xr/session.hpp"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <spdlog/fmt/fmt.h>
#include <wrl/client.h>

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
        if (st.backBufferW != 0 && (st.backBufferW != st.eyeW || st.backBufferH != st.eyeH)) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                "image is stretched: run tools/set-eye-resolution.ps1 -Width %u -Height %u", st.eyeW, st.eyeH);
        }
        ImGui::Text("xrWaitFrame %.2f ms  period %.2f ms  render %s  tracked %s", st.waitMs, st.periodMs,
            st.shouldRender ? "yes" : "no", st.tracked ? "yes" : "no");
        const game::MenuState menu = game::LastMenuState();
        ImGui::Text("frames submitted %llu  showing %s", static_cast<unsigned long long>(st.framesSubmitted),
            st.presentation);
        ImGui::Text("menu state: %s (menu %d, paused %d, riding camera %d)",
            !menu.available ? "unavailable" : menu.Active() ? "screen" : "riding", menu.menuOpen, menu.paused,
            menu.springCamera);
    }
    if (ImGui::Button(fmt::format("Recenter ({})", core::FormatHotkey(core::GetConfig().hotkeys.recenter)).c_str())) {
        xr::RequestRecenter();
    }
    ImGui::SameLine();
    if (ImGui::Button("Restart XR")) {
        xr::RequestRestart();
    }
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
    CameraSection();
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
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
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
    context->OMSetRenderTargets(1, g_rtv.GetAddressOf(), nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtvs, oldDsv);
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
