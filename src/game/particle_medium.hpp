// game::ParticleMedium (PLAN Phase 5d): PopcornFX simulates the right eye's per-camera particle collection.
//
// Why: effects flagged "Update Per Split" (HHParticlesComponent+0x4a) get one instance per camera index, in particle
// mediums 1 and 2 (HHParticlesComponent::PostObjectInit 0x1405091f0); view k renders collection 1 + k
// (StartRenderParticles). GameContext::RenderOneFrame calls EnableMedium(mgr, 2, GameEngine+0x184 != 0) every frame,
// and the mod's renderer-only two-view mode leaves the split-on byte +0x184 at 0, so medium 2 is never spawned or
// evolved (UpdateSpawn / UpdateEvolve skip disabled mediums): such effects would show in the left eye only.
// Evidence: docs/re/renderer.md 6 A.
//
// How: hook HHParticlesManager::EnableMedium (export; only caller RenderOneFrame): while two views run, medium 2 is
// enabled. Every collection is allocated at manager creation and the pair instances exist from actor init, so
// nothing else needs setting up. The two-view state read here is the previous frame's (EnableMedium runs before
// DoUpdate), so medium 2 starts and stops one frame after the views do.
#pragma once

namespace game {

// Thread: game main thread, after ResolveSignatures. Optional: a missing signature leaves medium 2 to the game.
void InstallParticleMedium();

} // namespace game
