// game::RiderView (PLAN 8a): which riding view the player-0 camera renders this frame. Every riding view, chase and
// cockpit, is one TTSpringCameraMode; the view's CameraData picks the behaviour (docs/re/camera.md 10.3):
//   CameraType Spring (chase)   UpdateSpringCamView builds its own look-at into mode+0x8; mode+0x3e4 holds an
//                               unrelated bike frame, so the look removal (camera.md 9) must not run.
//   Fixed / Cockpit             mode+0x3e4 -> +0x8, then LookAroundInput: the look removal applies. Runtime
//                               2026-10-21: Helmet, HelmetNoMesh, Dash and Fork are Fixed; FollowNear and FollowFar
//                               are Spring.
//   Photo                       ComputeView leaves the matrix alone.
// While crashed (BikeController+0x15e4, from the crash until the respawn) Manage skips ComputeView: the matrix is
// frozen at the crash pose or follows the tumbling bike, whatever the view says.
// All reads are SEH-guarded game memory, never written.
#pragma once

#include <string>

namespace game {

struct RiderView {
    bool valid = false; // the rendered camera is the player-0 KTUCamera in TTSpringCameraMode (no override mode)
    const void* mode = nullptr; // that TTSpringCameraMode
    int view = -1; // GameCameraViews: 0 FollowFar, 1 FollowNear, 2 Helmet, 3 HelmetNoMesh, 4 Dash, 5 Fork, 6..9 replay
    int type = -1; // TTCameraType of the view's CameraData: 0 Fixed, 1 Spring, 2 Cockpit, 3 Helicopter, 4 Photo
    bool rear = false; // rear view held
    bool crashed = false;
    const void* settings = nullptr; // the view's CockpitSettings (types 0 and 2), else null
    // The cockpit path of ComputeView and not crashed: mode+0x3e4 is the frame before the look rotations.
    bool Cockpit() const { return valid && !crashed && (type == 0 || type == 2); }
};

// Resolves TTSpringCameraMode::Manage (after ResolveSignatures). Without it every read is invalid.
void InitRiderView();
// Thread: game main thread (Renderer::DoUpdate). `ktuCamera` = the player-0 KTUCamera (game::LastPlayerCamera),
// `rendered` = the Graphics::Camera of the main view. Logs every change of view, type or crash state.
RiderView ReadRiderView(const void* ktuCamera, const void* rendered);
RiderView LastRiderView(); // main thread

const char* ViewName(int view);
std::string DescribeRiderView(const RiderView& v);

} // namespace game
