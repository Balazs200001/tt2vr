// game::CameraOverride (PLAN Phase 2): moves the rendered main view and reshapes its projection without
// touching the game camera itself. Design and evidence: docs/re/camera.md.
//
// Hooks (all on the game main thread unless noted):
//   Renderer::DoUpdate            pre: remember the main view's Camera* and open the "render window".
//   ViewContext::InitFromCamera   post, main camera inside DoUpdate only: world' = world * local offset; rebuild
//                                 the ViewContext's view, camera position and frustum. The scene-graph pool
//                                 (audio listener, physics, gameplay readers) is never written (CLAUDE.md rule 5).
//   Math::MatrixPerspectiveFovRH  post, any thread, render window only: when the fov argument is the main
//                                 camera's fov or front fov, reshape the FOV terms (keeps the caller's near/far).
//                                 This one choke point covers culling, front pass, lighting, sky, CSM and 3D HUD.
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
    bool projEnabled = false;
    float shiftX = 0, shiftY = 0; // window shift as a fraction of width / height (0.2 = 20% to the right)
};
CameraSettings& Settings(); // main thread only

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
};
CameraInfo LastCameraInfo(); // main thread

// Resolves the signatures and installs the three hooks. Call once on the main thread after the exe version is
// known. Returns false (and leaves the game untouched) if a required signature or hook is missing.
bool InstallCameraHooks();
bool CameraHooksActive();

// Present post-hook (main thread): closes the render window and rolls the per-frame counters.
void OnPresent();

// Multi-line description of LastCameraInfo for the log, and a one-line summary for remote requests.
std::string DescribeCamera();
std::string CameraSummary();

} // namespace game
