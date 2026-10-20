// game::ShadowSettings (PLAN Phase 4): shadow resolution for VR, applied by the mod itself (no game file edits).
//
// Why: the CSM crop (BuildCropViewProjectionMatrix 0x140823510) rounds each cascade's light-space box up to a power
// of two; the wide, tall eye frustum doubles every cascade (c0 64 m instead of 32 m), so the same 2048-texel maps
// give half the flat shadow density. Split mode also halves the static (distant) map per view.
// docs/re/frame-anatomy.md 5.3.
//
// How: in the device-creation hook (game main thread, after GameEngine::LoadSettings read UserSettings.cfg and
// before "Creating renderer" builds the shadow maps), raise Engine.Renderer.Shadow.CascadeShadowMapSize and
// Engine.Renderer.StaticShadow.Size to [xr] shadow_map_size / static_shadow_map_size, never lower them. Values go
// through the CVar's own min/max and its per-slot update (vtable slot 12), as PlatformSettings does.
// Side effect: the game saves its CVars when the player applies the Options menu, so a raised value can end up in
// UserSettings.cfg; a later shadow-quality preset change sets the preset value again.
#pragma once

namespace game {

// Thread: game main thread (D3D11CreateDeviceAndSwapChain hook). Needs the known exe build; no-op otherwise.
void ApplyShadowSettings();

} // namespace game
