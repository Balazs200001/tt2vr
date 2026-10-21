// game::SharedShadows (PLAN Phase 5c): the cascaded (sun) shadow maps are rendered once per frame and used by both
// eyes ([xr] shared_shadows, read at startup).
//
// Why: in the two-view frame the engine fits one cascade set per camera index (CSM::SetupRenderContext, from each
// view's ViewContext) and renders it per view (CSM::Render: atlas clear, static map, 3 cascades), so the right eye
// repeats the whole shadow render (about 725 depth draws in trace 4400) and its cascades can snap differently from
// the left eye's. Shadow maps are view-independent state: once per frame, shared (CLAUDE.md rule 1, PLAN
// "Temporal design"). Evidence: docs/re/renderer.md 3.
//
// How (main thread, recording inside DeferredRenderLoop::Render, two-view frames only):
//   CSM::SetupRenderContext   view 0 (camera index 0) is fitted from the head centre (eye midpoint) instead of the
//                             left eye, so the shared cascades sit symmetrically between the eyes, with the union
//                             eye projection the mod supplies in this phase. Each eye's frustum edge then lies at most
//                             half the eye separation (about 3 cm) outside the fitted one, inside the power-of-two
//                             slack of the cascade box except on its texel-snapped min edge (docs/re/renderer.md 3).
//   CSM::Render, index 0      unchanged; the CascadedShadowContext it hands to SetCascadedShadowContext is kept.
//   CSM::Render, index 1      not run; SetCascadedShadowContext(rc, kept context) instead, so view 1's lighting reads
//                             view 0's matrices and atlas. Passes execute in submission order, and nothing writes the
//                             atlas between view 0's cascades and view 1's lighting.
// Index 1's own fit and static-map bookkeeping still run every frame but are not used.
#pragma once

namespace game {

// Thread: game main thread, after ResolveSignatures. Optional: missing signatures leave per-view shadows.
void InstallSharedShadows();

} // namespace game
