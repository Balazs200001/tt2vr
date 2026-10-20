// game::ShadowSettings (PLAN Phase 4): shadow resolution for VR as a multiplier of the game's own setting.
//
// Why: the CSM crop (BuildCropViewProjectionMatrix 0x140823510) rounds each cascade's light-space box up to a power
// of two; the wide, tall eye frustum doubles every cascade (c0 64 m instead of 32 m), so the game's map size gives
// half the flat shadow density. Split mode also halves the static (distant) map per view.
// docs/re/frame-anatomy.md 5.3.
//
// How: the engine reads the two size CVars only while it creates the maps. Around exactly those calls the CVar's
// camera-index-0 slot is multiplied by [xr] shadow_scale and restored right after, so the maps follow whatever the
// in-game shadow quality sets, and the game never sees or saves a changed value:
//   GameEngine::ConfigureRenderer        startup, Engine.Renderer.Shadow.CascadeShadowMapSize -> renderer desc
//   Renderer::UpdateShadowQuality        in-game shadow quality change, same CVar -> CSM rebuilt
//   CascadedShadowMapRenderer::CreateStaticShadowMap   every split change, Engine.Renderer.StaticShadow.Size
// The cascade size is capped so the 3-cascade atlas stays within the 16384-texel D3D11 limit.
// Hooks are installed in the device-creation hook (game main thread), before "Creating renderer".
#pragma once

namespace game {

// Thread: game main thread (D3D11CreateDeviceAndSwapChain hook). Needs the known exe build; no-op otherwise.
void InstallShadowScaleHooks();

} // namespace game
