// game::TaaControl (PLAN Phase 4d): TAA history resets for both eyes and the cross-eye leak test.
//
// Hook: RenderLoop::RenderTAA 0x1408ddf70 (RenderLoop*, RenderContext&), called once per view at record time (main
// thread, per-view chain of DeferredRenderLoop::Render; camera index at RenderContext+0x594).
//   Reset: the engine's TAA reset flag RenderLoop+0xd08 is one shared byte, cleared at the end of each RenderTAA, so
//     a reset is written before every view's call while pending (docs/re/temporal.md N6). Triggers: recenter (the
//     view jumps) and the remote "taareset" command; split-mode changes already recreate the history
//     (OnSplitChanged).
//   Leak test (remote "leaktest", PLAN Temporal design tests): for one frame, eye 0's TAA history texture is found
//     at record time (RenderLoop+0xce8 -> RenderTarget vtbl+0x40 -> Texture2DDX11+0x68), and a pre-draw callback
//     clears the FrameBuffer input (t1) of the draw that reads that history (t2) to red, on the same context right
//     before the draw. (RenderLayer::ClearRT queues into a pass queue that executes before the frame's main queue,
//     so an engine-queued clear was overwritten; runtime 2026-10-20.) At Present (before the mirror redraws it) the back buffer is
//     read back for a baseline frame, the tint frame and 6 frames after, counting red-dominant pixels in each half.
//     Pass: the left half jumps on the tint frame and decays; the right half stays at its baseline (no history of
//     eye 0 reaches eye 1, invariant T2).
#pragma once

#include <dxgi.h>
#include <d3d11.h>

#include <functional>
#include <string>

namespace game {

// Thread: game main thread, after ResolveSignatures. Optional.
void InstallTaaControl();
// Thread: game main thread, Renderer::DoUpdate pre-hook. `recenterCount` = xr::RecenterCount().
void OnTaaFrame(unsigned recenterCount);
// Thread: game main thread.
void RequestTaaReset(const char* reason);
void RequestLeakTest(std::function<void(const std::string&)> done);
// Thread: game main thread, Present pre-hook, before anything draws over the back buffer.
void OnTaaPresent(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context);

} // namespace game
