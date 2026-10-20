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
#pragma once

namespace game {

// Thread: game main thread (Renderer::DoUpdate pre-hook). After ResolveSignatures.
void UpdateVrEffects(bool vrActive);

} // namespace game
