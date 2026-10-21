// game::SkinMotion (PLAN Phase 5a): skinned meshes keep their object motion when two views render in one frame.
//
// Why: a SkinMesh owns one EntityRenderData (+0x68) with one current / previous bone palette pair, shared by both
// camera indices. The fill job calls SkinMesh::UpdateRenderData once per view that sees the mesh, with the same
// frameId. Each main-pass call sets "motion valid" from its own camera index's last frame (+0x158+idx*4), and the
// refill then copies prev <- cur: view 0 latches pose N-1 -> N, view 1 latches again (N -> N). The palettes are
// copied into the constant buffers when the draws are recorded, after both fills, so both eyes draw with prev == cur
// and the motion vectors of bike and rider carry no object motion (TAA treats them as static in the world).
// Evidence: docs/re/renderer.md 4 (decompile, and trace pairs of vs:51bfa2c7f479d821: 5/5 differ flat, 41/44 equal
// in two-view traces).
//
// How: pre-hook. Main pass only (ViewContext+0x460 == 0), camera index 0 or 1: if the other index's slot already
// holds this frameId (its main pass ran this frame), this index's last frame / LOD are set to this frameId and the
// other index's LOD first. The original then sets neither "motion valid" nor "reset", so the palette is latched
// once per frame; cur is refilled from the same pose. A mesh seen by one view only latches as before. If the two
// views pick different LODs the engine's own reset fires (one frame without motion for that mesh), as it does on
// any LOD change.
#pragma once

namespace game {

// Thread: game main thread, after ResolveSignatures. Optional: a missing signature leaves the engine behaviour.
void InstallSkinMotion();

} // namespace game
