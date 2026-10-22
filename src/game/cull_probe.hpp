// game::CullProbe ([debug] flash_detect, 2026-10-22): records what the visibility culler gets for each eye, to explain
// the one-frame "scenery missing in one eye" glitches d3d::flash catches (test mode: the right eye loses roadside
// houses / trees on the left of its view for one frame). Per frame, for the main views (camera index 0 / 1):
//   - at VisibilityCuller::AddCullingData (main-view flags): the ViewContext's view matrix and frustum planes;
//   - at RenderContext::Prepare(view) on the main context: how far the rendered view matrix is from the culled one.
// The summary line goes with each frame the flash detector keeps.
#pragma once

#include <string>

namespace game {

// Thread: game main thread, after ResolveSignatures. Only when [debug] flash_detect is on.
void InstallCullProbe();
// Thread: game main thread, start of the main DeferredRenderLoop::Render.
void CullProbeLoopBegin();
// Thread: game main thread, RenderContext::Prepare(view) on the main RenderContext.
void CullProbeRenderView(const void* viewContext);
// The last main render's line, e.g. "v0 culled, view diff 0.0000, planes moved 0.3 deg; v1 ...". Main thread.
std::string CullProbeSummary();

} // namespace game
