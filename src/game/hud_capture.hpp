// game::HudCapture (PLAN Phase 7): the game's 2D HUD out of the eye images.
// Evidence: docs/re/frame-anatomy.md 8.6. One HudRenderer::Render call per HUD list and flag set; the global 2D list
// (race HUD, pause and menu UI) is drawn once per frame after both views, into the back buffer that holds both eyes,
// laid out across its whole width.
//
// 7a (probe, pass-through): records every HudRenderer::Render call of a frame (kind of list, flags, target size,
// layout size). Thread: game main thread (calls are recorded while DeferredRenderLoop::Render records the frame).
#pragma once

#include <functional>
#include <string>

namespace game {

// After ResolveSignatures (main thread). A missing signature leaves the HUD untouched.
void InstallHudCapture();

// Thread: game main thread, around DeferredRenderLoop::Render of the main renderer (game::CameraOverride's hook):
// the Renderer (its views' lists) and the RenderDesc whose global list (+0xc50) this frame draws.
void BeginHudLoopRender(void* renderer, void* renderDesc);
void EndHudLoopRender();

// Thread: game main thread, Present post-hook: the frame's calls are complete.
void OnHudPresent();

// Remote "hud": the HUD calls of the next complete frame, with the presentation state (main thread).
void RequestHudReport(std::function<void(const std::string&)> done);

} // namespace game
