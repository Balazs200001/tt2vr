// game::HudCapture (PLAN Phase 7): the game's 2D HUD out of the eye images, onto its own panel.
// Evidence: docs/re/frame-anatomy.md 8.6. One HudRenderer::Render call per HUD list and flag set; the global 2D list
// (race HUD, pause and menu UI) is drawn once per frame after both views, into the back buffer that holds both eyes,
// laid out across its whole width.
//
// 7a probe: records every HudRenderer::Render call of a frame (kind of list, flags, target size, layout size).
// 7b capture, while two views ride in the headset session ([hud] enabled): the global call is drawn twice, into two
// engine render targets of the mod ('tt2vr HUD black' / 'white', RenderTargetManager::CreateRenderTarget, sRGB RGBA,
// [hud] pixels), with the 2D layout set to 1080 x [hud] aspect (HudRenderer+4/+8, restored after the call), so the
// back buffer and both eye images get no 2D HUD. The minimap is masked through the destination alpha, so one render
// cannot give the HUD's alpha; over black and over white it can: alpha = 1 - (white - black), colour = black
// (premultiplied, d3d::Blitter::Matte). The targets are cleared to transparent black / white after use, for the next
// frame (record-then-execute: the engine draws into them when it executes the frame, before Present).
// Fail safe: any missing signature, engine call or view leaves the HUD painted into the eye images as before.
// Thread: game main thread (recording, Present).
#pragma once

#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <string>

namespace game {

// After ResolveSignatures (main thread). A missing signature leaves the HUD untouched.
void InstallHudCapture();

// Thread: game main thread, around DeferredRenderLoop::Render of the main renderer (game::CameraOverride's hook):
// the Renderer (its views' lists) and the RenderDesc whose global list (+0xc50) this frame draws.
void BeginHudLoopRender(void* renderer, void* renderDesc);
void EndHudLoopRender();

// This frame's captured HUD, valid from OnHudFramePresent to EndHudFrame (Present pre-hook); empty if none.
struct HudImage {
    ID3D11ShaderResourceView* black = nullptr; // the two renders, through _SRGB views (linear reads)
    ID3D11ShaderResourceView* white = nullptr;
    ID3D11ShaderResourceView* resolved = nullptr; // premultiplied colour and alpha, sRGB-encoded (desktop mirror)
    std::uint32_t width = 0, height = 0; // pixels
    explicit operator bool() const { return resolved != nullptr; }
};
// Present pre-hook, before xr::OnFrameEnd: resolves this frame's capture (if any) and answers "hud save".
void OnHudFramePresent(ID3D11DeviceContext* context);
HudImage CurrentHud();
// Present pre-hook, after xr::OnFrameEnd and the desktop mirror: clears the capture targets for the next frame.
void EndHudFrame(ID3D11DeviceContext* context);

// Thread: game main thread, Present post-hook: the frame's calls are complete; creates the capture targets once the
// headset session runs.
void OnHudPresent();

// Remote "hud": the HUD calls of the next complete frame, with the presentation and capture state (main thread).
void RequestHudReport(std::function<void(const std::string&)> done);
// Remote "hud save": the next captured HUD as <capture dir>\screens\hud_color.png (over a checkerboard) and
// hud_alpha.png (main thread).
void RequestHudSave(std::function<void(const std::string&)> done);

} // namespace game
