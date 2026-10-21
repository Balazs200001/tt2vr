// game::SlotMirror (PLAN 8f / 8g): per-camera-index state the engine only feeds for slot 0, made to reach view 1 (the
// right eye) while the mod's two views run. Evidence: docs/re/renderer.md 6 B (visor drops) and 6 D (crash grey).
//   Visor drops, shared (Balazs's choice, PLAN Decisions log 2026-10-21): WaterDrop::Render(wd, dt, lastFrame, slot)
//     simulates and draws one slot's drops per view. For slot 1 the hook renders slot 0's drops again with dt 0 (no
//     second simulation step, no spawn) over view 1's own last-frame colour, and copies slot 0's alive byte to slot 1
//     so view 1's drop composite and last-frame copy follow. Both eyes then show the same drops.
//   Crash grey: TTCollisionEffect (TTGameEngine+0x778) drives the "FadeToColor" post process for slot 0 only. In the
//     DeferredRenderLoop::Render pre-hook, slot 1's enable state follows slot 0's and the material parameters
//     fBlend, fGrey, vLevels and vColor are copied from slot 0 to slot 1 every frame slot 0 is on. Also covers a
//     script grey fade with mask 1 (KTUFadeToColor), which uses the same material.
// Both are visual only and leave the game's own slot-0 state untouched.
#pragma once

#include <functional>
#include <string>

namespace game {

// After ResolveSignatures (main thread, between frames). Missing signatures leave view 1 as before.
void InstallSlotMirror();
// Thread: game main thread, DeferredRenderLoop::Render pre-hook of the main render. `twoViews` = the mod's two views
// run this frame.
void OnSlotMirrorLoopRender(void* renderLoop, bool twoViews);
// [debug] remote "drops <seconds>" (test mode only): rain on slot 0's visor for that long, to see the drops without
// rain weather. Returns the result line.
std::string ForceVisorRain(float seconds);

} // namespace game
