// game::PassProfiler (PLAN Phase 9): GPU time of every engine pass, per eye, on request (remote "gpuprofile [frames]").
// Evidence: docs/re/renderer.md 7 and 9. The engine records passes into one queue (RenderLayer::DrawCommandBuffer,
// Dispatch, CopyTextureSlice: draw, compute, copy) and executes them later in append order, split into jobs on the
// immediate and deferred contexts. While a profile runs, each recorded pass is bracketed by two of the engine's own
// profiling events (RenderLayer::AddProfilingEvent: a timestamp End plus BeginEvent(name) / EndEvent() on the context
// that executes it), so end - begin is the pass's GPU time and the names show in RenderDoc. Profiling events carry
// no cost, so the job split is unchanged.
// A frame is everything recorded between two Presents; a disjoint query per frame brackets it on the immediate
// context (Present post-hook, inside the engine's immediate-context lock). Passes are attributed to the view being
// recorded (game::GpuTimersOpenView: view 0, view 1) or to "frame" (shadow cascades, probes, HUD, ...). Results are
// summed per pass name over the requested frames and written to <capture dir>\gpuprofile_<frame>.txt.
// Thread: game main thread (recording, Present). Idle cost: one branch per pass.
#pragma once

#include <d3d11.h>

#include <functional>
#include <string>

namespace game {

// After ResolveSignatures (main thread, between frames). Missing exports leave the profiler off.
void InstallPassProfiler();
// Remote "gpuprofile [frames]" (main thread): profile the next `frames` frames (1..600); `done` gets the result line
// once they have been read back.
void RequestPassProfile(int frames, std::function<void(const std::string&)> done);
// Thread: game main thread, Present post-hook (inside the engine's immediate-context lock).
void PassProfilerPresent(ID3D11Device* device, ID3D11DeviceContext* immediate);
// Test mode "flashtest": the next frame is recorded without its 'Opaque' and 'Alpha test' passes (a one-frame
// scenery dropout for checking d3d::flash). Main thread.
void RequestFlashTest(std::function<void(const std::string&)> done);

} // namespace game
