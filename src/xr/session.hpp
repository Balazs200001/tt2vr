// xr::Session (PLAN Phase 3): OpenXR instance, session on the game's D3D11 device, frame loop, eye swapchains.
//
// Frame timing (all calls on the game main thread, which also runs Present; docs/re/README.md):
//   Present post-hook   OnFrameStart: pump events (session state machine, recenter events), then xrWaitFrame +
//                       xrBeginFrame. The wait throttles the start of the next frame, never the middle of a render.
//   Renderer::DoUpdate  LocateViews: xrLocateViews at the frame's predictedDisplayTime, once per XR frame, after the
//                       game's camera update and before the views are built (late as possible, PLAN frame model).
//   DoUpdate            SetSideBySide: the engine renders two views this frame (game::SplitView), so back-buffer
//                       half k is view k.
//   InitFromCamera      MarkRendered: view k of this frame was rendered from LocateViews' eye e.
//   Present pre-hook    OnFrameEnd: side by side, eye k <- half k; otherwise the back buffer into the left eye and
//                       the right eye cleared. xrEndFrame with the exact pose and FOV each image was rendered from. No head-tracked render this frame (menu, pause,
//                       loading, movie) -> the flat frame on a quad layer fixed in LOCAL space ([xr] menu_screen).
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
void MarkRendered(int view, int eye); // ViewContext::InitFromCamera: view 0/1 was rendered from LocateViews' eye
void SetSideBySide(bool sideBySide); // Renderer::DoUpdate: this frame's back buffer holds view 0 | view 1
void OnFrameEnd(IDXGISwapChain* swapChain, ID3D11DeviceContext* context); // Present pre-hook
// Present pre-hook, after OnFrameEnd and before the overlay: while the session runs, redraws the back buffer for the
// desktop window ([xr] desktop_view), letterboxed so the window's stretch gives the right shape.
void DrawDesktopMirror(IDXGISwapChain* swapChain, ID3D11DeviceContext* context, HWND window);

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
    const char* presentation = "-"; // "stereo" / "head-tracked" (projection layer), "screen" (menu quad), "-"
    bool sideBySide = false; // last DoUpdate set up two views
    const char* copyMode = "-"; // "copy" (raw CopySubresourceRegion), "blit" (stretch), "-" (nothing submitted)
    std::uint32_t backBufferW = 0, backBufferH = 0;
    double waitMs = 0; // xrWaitFrame, smoothed
    double periodMs = 0; // predictedDisplayPeriod
    std::uint64_t framesSubmitted = 0; // xrEndFrame calls with a layer (projection or screen)
};
Status GetStatus(); // main thread

} // namespace xr
