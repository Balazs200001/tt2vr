// game::CameraOverride (PLAN Phase 2): moves the rendered main view and reshapes its projection without
// touching the game camera itself. Design and evidence: docs/re/camera.md.
//
// Hooks (all on the game main thread unless noted):
//   Renderer::DoUpdate            pre: remember the main view's Camera* and open the "render window".
//   ViewContext::InitFromCamera   post, main camera inside DoUpdate only: world' = world * local offset; rebuild
//                                 the ViewContext's view, camera position and frustum. The scene-graph pool
//                                 (audio listener, physics, gameplay readers) is never written (CLAUDE.md rule 5).
//   TTCamera::AfterPhysicsStep +  physics step: while the game places the HelmetView actor at the camera mode's
//   Actor::SetWorldMatrix         helmet matrix H (mode+0x4c4, built from the mode camera matrix M = mode+0x8),
//                                 pass M * local * M^-1 * H instead, so the helmet turns with the view. Optional
//                                 ([camera] helmet_follows_view); the game's "Helmet No Mesh" view never places it.
//   Math::MatrixPerspectiveFovRH  post, any thread, render window only: when the fov argument is the main
//                                 camera's fov or front fov, reshape the FOV terms (keeps the caller's near/far).
//                                 This one choke point covers culling, front pass, lighting, sky, CSM and 3D HUD.
// Head tracking (Phase 3): DoUpdate pre also asks xr::LocateViews for the left eye. When a pose is available the view
// becomes world * sliders * EyeLocal and the projection takes the eye's XrFovf tangents (TAA jitter is still added
// by the engine afterwards); the right eye is not rendered yet.
// The render window closes at Present, so game-logic callers of MatrixPerspectiveFovRH (KTUCameraFixed::Manage
// etc., which run before DoUpdate) are never affected.
#pragma once

#include <cstdint>
#include <string>

namespace game {

// Edited by the overlay on the main thread; read by the hooks at the next DoUpdate.
struct CameraSettings {
    bool viewEnabled = false;
    float yawDeg = 0, pitchDeg = 0, rollDeg = 0; // camera-local, see stereo::CameraLocalTransform
    float offset[3] = {0, 0, 0}; // metres, camera-local (x right, y up, z back)
    bool helmetFollows = true; // helmet mesh follows the view override (init from [camera] helmet_follows_view)
    float horizonLock = 100.0f; // percent of the camera's roll removed while head-tracked ([camera] horizon_lock)
    float shake = 100.0f; // percent of the game's camera shake and speed noise kept in VR ([camera] shake)
    float seat[3] = {0, 0, 0}; // eye point from the recentered head: forward, up, right (m; [camera] seat_*, 8d)
    float worldScale = 1.0f, ipdScale = 1.0f; // [xr] world_scale / ipd_scale (PLAN 8e)
    bool projEnabled = false;
    float shiftX = 0, shiftY = 0; // window shift as a fraction of width / height (0.2 = 20% to the right)
};
CameraSettings& Settings(); // main thread only
// Seat hotkeys (main thread, PLAN 8d): move the eye point by `forward` / `up` metres (clamped to +-0.5); reset = 0.
void NudgeSeat(float forward, float up);
void ResetSeat();

struct CameraInfo {
    bool valid = false;
    std::uint64_t updates = 0; // main-view InitFromCamera calls seen
    const void* camera = nullptr;
    float fov = 0, nearZ = 0, farZ = 0, aspectMul = 0; // radians / units
    float frontFov = 0, frontNear = 0, frontFar = 0;
    bool frontPass = false;
    int viewportW = 0, viewportH = 0;
    float world[3][4] = {}; // engine camera world (before our override)
    float proj[4][4] = {}; // main ViewContext projection as installed (after override and jitter)
    float jitter[2] = {};
    std::uint32_t projOverrides = 0; // MatrixPerspectiveFovRH calls reshaped in the last frame
    std::uint32_t projUnion = 0; // of those, with the union of both eye frusta (no current eye)
    float autoLookYawDeg = 0; // yaw of the game's look rotation removed from the view this frame (Dynamic View)
    // Horizon lock (PLAN 8b): roll against the horizon of the game camera and of the frame the head pose was put on
    // (after look removal and lock), this frame and the largest magnitude over the last 10-20 s. Degrees.
    float gameRollDeg = 0, shownRollDeg = 0, gameRollMaxDeg = 0, shownRollMaxDeg = 0;
    // Shake scale (PLAN 8c): the game's KTUCamera shake rotation this frame, the largest shown share over the last
    // 10-20 s (degrees), and the mode's last speed noise values (bob in metres-ish units, roll in radians).
    float shakeDeg = 0, shakeShownMaxDeg = 0, speedNoise = 0, rollNoise = 0;
    float viewPos[2][3] = {}; // rendered position of views 0 and 1 (world, after head tracking and the comfort settings)
    std::uint32_t helmetMoves = 0; // helmet placements adjusted in the last frame (0 while physics is paused)
    bool headTracked = false; // the last main view was rendered from the headset's left eye (Phase 3)
};
CameraInfo LastCameraInfo(); // main thread

// Resolves the signatures and installs the three hooks. Call once on the main thread after the exe version is
// known. Returns false (and leaves the game untouched) if a required signature or hook is missing.
bool InstallCameraHooks();
bool CameraHooksActive();
bool HelmetHooksActive();

// Present post-hook (main thread): closes the render window and rolls the per-frame counters.
void OnPresent();

// Thread: game main thread, scene phase of DeferredRenderLoop::Render (CSM::SetupRenderContext). While head tracking
// runs, gives `viewContext` (a main view built this frame) the head's view (eye midpoint) and returns true;
// EndHeadCenterView puts the saved view back. False = nothing changed.
bool BeginHeadCenterView(void* viewContext);
void EndHeadCenterView(void* viewContext);

// Remote "comfort [<setting> <value>]" (main thread): with no argument, the current comfort settings; "lock <0..100>"
// sets the horizon lock, "shake <0..100>" the shake and speed noise share. Returns the result line.
std::string ComfortCommand(const std::string& arg);

// The local player's TT2 camera controller (TTCamera, last seen in its physics step), or nullptr. Main thread.
const void* LocalRiderCamera();

// Multi-line description of LastCameraInfo for the log, and a one-line summary for remote requests.
std::string DescribeCamera();
std::string CameraSummary();

} // namespace game
