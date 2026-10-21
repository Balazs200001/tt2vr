// game::CameraNoise (PLAN 8c): scales the riding camera's speed noise (vertical bob, a little pitch, and roll, growing
// with speed squared) at its source: the view's CockpitSettings amplitudes +0x1c (bob) and +0x20 (roll), which
// TTSpringCameraMode's PhysicsStepTarget multiplies its noise waves by (0 skips the term). docs/re/camera.md 10.2.
// Balazs's choice (PLAN Decisions log 2026-10-21): exact, and the game's own camera (so the audio listener and the
// helmet springs) gets the smaller noise too. Tuning data in memory only, never saved.
//
// Restore rule: every settings object keeps the values it had when first seen; while it is the live view's settings
// the mod writes original * scale (scale 1 outside the headset session). A value that differs from the one the mod
// wrote last means the game reloaded the object (or reused its memory): that value becomes the new original. Only the
// pointer read live this frame is ever written.
#pragma once

#include <string>

namespace game {

// Thread: game main thread (Renderer::DoUpdate). `settings` = RiderView::settings (null: nothing to do).
void ApplyCameraNoise(const void* settings, float scale);
// The amplitudes written last (bob, roll) and their originals, for `camera`.
std::string DescribeCameraNoise();

} // namespace game
