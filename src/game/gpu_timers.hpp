// game::GpuTimers (PLAN Phase 5b): GPU time of each view (eye) and of the whole frame.
//
// The engine records passes first and executes them later, split into jobs on the immediate and deferred contexts
// (docs/re/frame-anatomy.md 1.2, 1.6), so a GPU timestamp for "view k starts" must sit at that point of the pass
// queue. RenderLayer::AddProfilingEvent (exported, never called by the engine) queues exactly that: a kind-5 pass
// that, when its job executes it, calls End(query) on that job's context and then BeginEvent(name) (name != null) or
// EndEvent() (docs/re/renderer.md 7). Per frame, recorded on the main thread inside DeferredRenderLoop::Render:
//   view k starts (the main RenderContext is prepared for a new camera index): end the open view, begin view k
//   end of DeferredRenderLoop::Render: end the open view
// so the markers are balanced and name the views in RenderDoc. Frequency, disjoint flag and the frame total come
// from the engine's own GPU timer slot of that frame (Renderer+((ec8&3)*3+0x1cd)*8: start, end, disjoint), which
// brackets OnRender to OnFlushRender. Results are read without waiting in the Present post-hook, a frame or two later
// (ring of 4 frames).
#pragma once

#include <d3d11.h>

#include <string>

namespace game {

struct GpuTimes {
    bool valid = false;
    int views = 0; // 1 or 2
    double viewMs[2] = {};
    double frameMs = 0; // the engine's bracket: OnRender start to OnFlushRender end
};

// Thread: game main thread, after ResolveSignatures. Optional: missing signatures leave the timers off.
void InstallGpuTimers();
// Thread: game main thread, around the main DeferredRenderLoop::Render (not external scenes).
void GpuTimersLoopBegin(void* renderer);
void GpuTimersLoopEnd();
// Thread: game main thread, the main RenderContext prepared for `viewContext` (RenderContext::Prepare hook).
void GpuTimersView(const void* viewContext);
// Thread: game main thread, Present post-hook (the frame's passes have executed).
void GpuTimersPresent(ID3D11Device* device, ID3D11DeviceContext* immediate);

// Last complete frame (for the overlay).
GpuTimes LastGpuTimes();
// Averages since the previous call, for the periodic perf log line; empty if no frame was measured.
std::string GpuTimersPerfSummary();

} // namespace game
