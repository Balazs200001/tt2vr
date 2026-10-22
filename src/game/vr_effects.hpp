// game::VrEffects: post effects switched off while the headset session runs, through the engine's own per-camera-
// index CVar value bytes (CVar<bool> +0x80/+0x81, which the renderer reads per view every frame). The CVar value is
// never changed, so the in-game option and UserSettings.cfg are untouched; when the session stops the CVar's own
// per-slot update (vtable slot 12) recomputes the bytes from it.
//   SSAO ([xr] ssao = false): IntelSSAO::s_bSSAOEnable, checked per view in DeferredRenderLoop::Render 0x140847c91.
//     Screen-space AO only sees what is on screen and swims with the head in the wide, off-centre eye views, even
//     with the constants fixed (game::SsaoFix). Not a temporal effect.
//   Motion blur ([xr] motion_blur = false): RenderLoop::s_bMotionBlurEnable (per-slot bytes 0x141407830[idx],
//     gate of RenderMotionBlur). Comfort/clarity choice by Balazs 2026-10-20 (PLAN Decisions log, exception to
//     CLAUDE.md rule 1): the smear on bike and rider was ordinary camera motion blur, lengthened by the VR frame
//     rate, not a stereo artifact (motion vector data identical in both views, frame trace 13970). TAA and the
//     motion-vector pass stay on (the MV pass runs while TAA is on).
// Dynamic resolution (PLAN 6d, docs/re/temporal.md N4 / 10.3): while the headset session runs the scale stays at
//   exactly 1.0 / 1.0 and the upscale is off, so the game renders at the eye size it is forced to. The controller
//   FUN_1405b1270 rewrites DrawPass::s_vResolutionScaling and s_bDynamicResolutionEnabled every frame in OnRender,
//   before the render loop, from the untouched mode CVar; both are overwritten in the DeferredRenderLoop::Render
//   pre-hook, which every reader follows (LensFlareInstance::Update in the next DoUpdate reads the forced value too).
//   The saved setting is never written; when the session stops the controller's values come back the next frame.
//   A resolution-control choice, not a temporal bypass: TAA, SSR and froxels keep their history (previous scale 1.0).
// FXAA ([xr] fxaa = false, PLAN 9c): RenderLoop::RenderFXAA (export) runs for AA modes 1 and 2, and the menu's
//   "TAA" is mode 2 (FXAA_TAA), so the game runs FXAA before TAA. The hook skips it while the headset session runs;
//   TAA stays on. 0.15 ms per eye at 1770x1892 (gpuprofile).
// Vignette ([xr] vignette = false, PLAN 9c): Vignette::ApplyParams enables or disables the "Vignette" post process for
//   both camera indices every frame (environment mixer); the render pre-hook disables it again after that while the
//   headset session runs, so the game's next ApplyParams restores it once the session stops. The Ansel session
//   switches it the same way.
#pragma once

#include <string>

namespace game {

// After ResolveSignatures (main thread, between frames): the FXAA hook. Optional.
void InstallVrEffects();
// Thread: game main thread (Renderer::DoUpdate pre-hook). After ResolveSignatures.
void UpdateVrEffects(bool vrActive);
// Thread: game main thread (DeferredRenderLoop::Render pre-hook, main render only).
void ForceVrResolution(bool vrActive);
// Thread: game main thread (DeferredRenderLoop::Render pre-hook, main render only): post processes off in VR.
void ApplyVrPostEffects(void* renderLoop, bool vrActive);
// Remote "vreffect <fxaa|vignette> <on|off>" (main thread): changes [xr] fxaa / vignette for this run. Result line.
std::string VrEffectCommand(const std::string& arg);

} // namespace game
