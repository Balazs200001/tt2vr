// game::LensMask (PLAN 9, lens visibility mask; renderer.md 13): the pixels the headset's lenses never show are not
// rendered. Per eye, right after the engine clears the view's depth (the 'discard framebuffer & clear stencil &
// depth' pass at the start of RenderGBuffer), the runtime's hidden-area mesh is drawn into that depth at 1.0 (the
// near plane), so every later world and front-pass draw fails the GREATER_EQUAL test there. Just before the view's
// 'Clear GBuffer' pass the same mesh is drawn again at 0.0: those pixels then read as sky, so 'Clear GBuffer' fills
// their GBuffer and lighting takes its cheap sky path (no stale GBuffer content, no NaNs for bloom to spread).
//
// The passes are found when they are recorded (game::PassProfiler's RenderLayer::DrawCommandBuffer hook, with the
// view that is open) and the mesh is drawn when they execute, on whatever context the engine's job uses.
#pragma once

namespace game {

// Thread: game main thread, after ResolveSignatures. Needs game::PassProfiler's draw hook.
void InstallLensMask();
// Thread: game main thread (RenderLayer::DrawCommandBuffer hook), before the pass is queued or run. `view` = the
// main render loop's open view (camera index 0 / 1), or -1.
void LensMaskOnRecord(void* pass, int view);
// Thread: game main thread, start of the main DeferredRenderLoop::Render: forget the last frame's passes.
void LensMaskLoopBegin(void* renderLoop);

} // namespace game
