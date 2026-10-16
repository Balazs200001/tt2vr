// xr::Session (PLAN Phase 3): OpenXR instance, session on the game's D3D11 device, frame loop, eye swapchains.
//
// Frame timing (all calls on the game main thread, which also runs Present; docs/re/README.md):
//   Present post-hook   OnFrameStart: pump events (session state machine, recenter events), then xrWaitFrame +
//                       xrBeginFrame. The wait throttles the start of the next frame, never the middle of a render.
//   Renderer::DoUpdate  LocateViews: xrLocateViews at the frame's predictedDisplayTime, once per XR frame, after the
//                       game's camera update and before the views are built (late as possible, PLAN frame model).
//   InitFromCamera      MarkRendered: the main view of this frame was rendered from LocateViews' eye 0.
//   Present pre-hook    OnFrameEnd: copy the back buffer into the left eye, clear the right eye, xrEndFrame with the
//                       exact poses and FOVs that were located. No head-tracked render this frame -> no layers.
// Fail safe (CLAUDE.md rule 4): no runtime, no headset, an adapter mismatch or any XR error leaves the game flat.
// Nothing here blocks the game except xrWaitFrame while the session is running.
#pragma once

#include "stereo/math/xr_to_engine.hpp"

#include <d3d11.h>
#include <dxgi.h>

#include <cstdint>
#include <string>

namespace xr {

struct EyeView {
    stereo::Pose pose; // in the LOCAL reference space, XR axes
    float angleLeft = 0, angleRight = 0, angleUp = 0, angleDown = 0; // XrFovf, radians
};

struct FrameViews {
    EyeView eye[2]; // 0 = left, 1 = right
    stereo::Mat34 recenter; // stereo::EyeLocal(recenter, eye[i].pose) = camera-local eye transform
};

// Called once from the device-creation hook with the game's device (game main thread). Does no XR work yet.
void Init(ID3D11Device* device);

void OnFrameStart(); // Present post-hook
bool LocateViews(FrameViews& out); // Renderer::DoUpdate; false = no head pose this frame (render flat)
void MarkRendered(); // ViewContext::InitFromCamera, after the main view used LocateViews' eye 0
void OnFrameEnd(IDXGISwapChain* swapChain, ID3D11DeviceContext* context); // Present pre-hook

bool SessionRunning(); // between xrBeginSession and xrEndSession
void RequestRecenter(); // hotkey / overlay; applied at the next LocateViews
void RequestRestart(); // overlay: tear everything down and start again (also after the runtime asked us to exit)

struct Status {
    bool enabled = false;
    std::string state; // human readable: "no runtime", "waiting for headset", "focused", ...
    std::string runtime; // "Oculus 1.103.0"
    std::string system; // headset name
    std::string error; // last error, empty if none
    std::uint32_t recommendedW = 0, recommendedH = 0; // runtime's per-eye recommendation
    std::uint32_t eyeW = 0, eyeH = 0; // our swapchains (recommendation * [xr] resolution_scale)
    int format = 0; // DXGI_FORMAT of the swapchains
    bool running = false;
    bool shouldRender = false;
    bool tracked = false; // last located views had a valid orientation
    const char* copyMode = "-"; // "copy" (raw CopySubresourceRegion), "blit" (stretch), "-" (nothing submitted)
    std::uint32_t backBufferW = 0, backBufferH = 0;
    double waitMs = 0; // xrWaitFrame, smoothed
    double periodMs = 0; // predictedDisplayPeriod
    std::uint64_t framesSubmitted = 0; // xrEndFrame calls with a projection layer
};
Status GetStatus(); // main thread

} // namespace xr
