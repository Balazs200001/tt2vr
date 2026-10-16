// game::SplitView (PLAN Phase 4): render the two eyes as the engine's two split-screen views in one frame, through
// renderer state only. GameEngine::SetSplitMode is never called: it also changes audio listeners, GUI screens, CVar
// caps, cloth simulation and content density (CLAUDE.md rule 5). Evidence: docs/re/frame-anatomy.md 5.1.
//
// Per frame, in the Renderer::DoUpdate pre-hook (game main thread), before DoUpdate copies the CameraDesc:
//   GameMainCamera  tracks the CameraDesc the game wants (Renderer+0xe40..+0xe58) and returns its main camera.
//                   The game's own writes (level loads, SetActiveCamera) are told apart from ours by comparing with
//                   what we wrote last frame.
//   ApplySplitView  two views: RenderTargetManager::SetPossibleSplitMode(1) once, then CameraDesc {1, cam, cam, 3},
//                   view 1's scene = view 0's (Renderer+0xd88 = +0xd80), effector-map centre slot 1 = slot 0, no
//                   'Split border'. DoUpdate itself then switches RenderTargetManager and EffectorMapManager to mode
//                   1 and the next OnRender resizes the window RTs and allocates slot-1 history (OnSplitChanged).
//                   One view: the game's CameraDesc and our saved fields are written back.
// The back buffer must be 2 x eye width (game::RenderSize), so each half-width split view is one full-size eye:
// view k lands in back-buffer half k.
#pragma once

#include <string>

namespace game {

// Resolves the signatures (after ResolveSignatures, main thread). Missing ones leave the game single-view.
void InitSplitView();
// Signatures resolved and [xr] stereo on: the render size should be 2 x eye width.
bool SplitViewAvailable();

// Thread: game main thread, Renderer::DoUpdate pre-hook, before ApplySplitView.
// The Graphics::Camera of the game's main view (CameraDesc index ctz(mask)), or nullptr.
const void* GameMainCamera(void* renderer);
// Thread: game main thread, Renderer::DoUpdate pre-hook. twoViews = render `camera` as views 0 and 1 this frame.
// Refused (single view) unless the game's desc is one view at index 0 and `camera` is its camera.
// `riderCamera` = the TT2 camera controller last seen in its physics step (or nullptr): its per-camera-index
// state (own suit/helmet hidden in first person, shadow decal, first-person lighting) is copied from index 0 to 1.
void ApplySplitView(void* renderer, const void* camera, bool twoViews, const void* riderCamera);
// This frame was set up as two views (valid from ApplySplitView to the next one).
bool SplitViewActive();

// One-line state for the log and overlay.
std::string SplitViewSummary();

} // namespace game
