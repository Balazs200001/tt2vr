// game::TaaControl (PLAN Phases 4d, 6b): history resets for both eyes and the temporal tests.
//
// Hook: RenderLoop::RenderTAA 0x1408ddf70 (RenderLoop*, RenderContext&), called once per view at record time (main
// thread, per-view chain of DeferredRenderLoop::Render; camera index at RenderContext+0x594).
//   Reset: the engine's TAA reset flag RenderLoop+0xd08 is one shared byte, cleared at the end of each RenderTAA, so
//     a reset is written before every view's call while pending (docs/re/temporal.md N6).
//   Leak test (remote "leaktest", PLAN Temporal design tests): for one frame, eye 0's TAA history texture is found
//     at record time (RenderLoop+0xce8 -> RenderTarget vtbl+0x40 -> Texture2DDX11+0x68), and a pre-draw callback
//     clears the FrameBuffer input (t1) of the draw that reads that history (t2) to red, on the same context right
//     before the draw. (RenderLayer::ClearRT queues into a pass queue that executes before the frame's main queue,
//     so an engine-queued clear was overwritten; runtime 2026-10-20.) At Present (before the mirror redraws it) the back buffer is
//     read back for a baseline frame, the tint frame and 6 frames after, counting red-dominant pixels in each half.
//     Pass: the left half jumps on the tint frame and decays; the right half stays at its baseline (no history of
//     eye 0 reaches eye 1, invariant T2). Passed 2026-10-21 (left 0.0% -> 99.7%, right at its 0.15% baseline).
//   Full reset (6b, docs/re/temporal.md 10.1), for both eyes while a reset is pending: the TAA byte before each
//     view's RenderTAA; the froxel ping-pong index DeferredLighting+0x780+idx*4 = -1 (one non-temporal frame) and
//     both slots' 'Last Frame Depth' / 'Last Frame Buffer' cleared to 0 (far depth, black: SSR has no history check
//     and would read the pre-cut frame) in the DeferredRenderLoop::Render pre-hook, on the immediate context before
//     the frame's lists execute; RenderLoop::PostUpdateLastFrameCamera right after each view's RenderContext::Prepare
//     (previous camera = current: no camera motion on the reset frame for MVs, SSR, TAA). Its front-pass set is put
//     back afterwards: the front ViewContext is built later in the view and still holds the previous view's. Exposure
//     is not reset (shared, PLAN Decisions log 2026-10-21).
//   Triggers: recenter, the remote "taareset" command, two views switched on (OnSplitChanged recreates the TAA and
//     last-frame RTs but sets no reset and keeps slot 1's froxel index and previous camera), an IPD change of more than
//     1 mm, the player camera's mode changing (helmet <-> chase or crash camera, respawn), and a camera cut.
//   Camera cut: the engine has none (temporal.md N6); the game camera moving more than 2 m + 1.5 x its last speed x dt
//     or turning more than 10 deg in one frame resets both eyes this frame and the next. A World Shift (Renderer+0xd28,
//     set before DoUpdate) is subtracted from the stored position first, so the 2048 m rebase is no cut.
//   Convergence test (remote "convergence"): 11 consecutive back-buffer readbacks; per eye the mean luma change
//     between consecutive frames, then the same with the history reset before every frame as the reference. Pass:
//     with history below 0.6 x the reference in each eye, and the eyes within 25% of each other. Head and bike still.
//     Saves the change of the last frame pair as screens\convergence_diff.png.
#pragma once

#include <dxgi.h>
#include <d3d11.h>

#include <functional>
#include <string>

namespace game {

// Thread: game main thread, after ResolveSignatures. Optional.
void InstallTaaControl();
// Thread: game main thread, Renderer::DoUpdate pre-hook. `recenterCount` = xr::RecenterCount().
void OnTaaFrame(void* renderer, unsigned recenterCount);
// Thread: game main thread, DoUpdate pre-hook after OnTaaFrame. `modeKey` = the player camera's active mode (null if
// unknown), `eyeDistance` = located eye separation in metres (<= 0 if no headset pose), `twoViews` = SplitViewActive().
void OnTaaCameraMode(const void* modeKey);
void OnTaaEyeDistance(float eyeDistance);
void OnTaaTwoViews(bool twoViews);
// Thread: game main thread, DeferredRenderLoop::Render pre-hook (main render only).
void OnTaaLoopRender(void* renderLoop);
// Thread: game main thread, after RenderContext::Prepare(view) on the main context (RenderLoop+0x1f00).
void OnTaaViewPrepared(void* renderLoop, void* renderContext);
// Thread: game main thread.
void RequestTaaReset(const char* reason);
void RequestLeakTest(std::function<void(const std::string&)> done);
void RequestConvergenceTest(std::function<void(const std::string&)> done);
// Remote "cuttest [noreset]": waits up to 60 s for the next camera cut inside two-view riding, then reports each
// eye's frame-to-frame change for 12 frames and saves screenshots of frames +1 and +2. noreset keeps the history for
// that cut (A/B).
void RequestCutTest(bool noReset, std::function<void(const std::string&)> done);
// Remote "shifttest" (PLAN 6c): waits up to 5 min for the next World Shift inside two-view riding; checks that no reset
// fires and that both slots' previous camera positions are rebased, then reports the frames like cuttest.
void RequestShiftTest(std::function<void(const std::string&)> done);
// Thread: game main thread, InitFromCamera post-hook for view 0 (inside DoUpdate). `world` = the game camera's
// world transform before the head pose (rows, translation in column 3). Resets both eyes' history on a cut.
void OnMainCameraWorld(const float (&world)[3][4]);
// Thread: game main thread, Present pre-hook, before anything draws over the back buffer.
void OnTaaPresent(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context);

} // namespace game
