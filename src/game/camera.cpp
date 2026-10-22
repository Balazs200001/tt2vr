#include "game/camera.hpp"

#include "core/config.hpp"
#include "core/hooks.hpp"
#include "core/log.hpp"
#include "core/user_toml.hpp"
#include "d3d/present.hpp"
#include "game/camera_noise.hpp"
#include "game/engine_layout.hpp"
#include "game/gpu_timers.hpp"
#include "game/hud_capture.hpp"
#include "game/menu_state.hpp"
#include "game/rider_view.hpp"
#include "game/signatures.hpp"
#include "game/slot_mirror.hpp"
#include "game/split_view.hpp"
#include "game/shared_exposure.hpp"
#include "game/taa_control.hpp"
#include "game/temporal_debug.hpp"
#include "game/vr_effects.hpp"
#include "stereo/math/comfort.hpp"
#include "stereo/math/projection.hpp"
#include "stereo/math/xr_to_engine.hpp"
#include "xr/session.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <mutex>
#include <numbers>
#include <sstream>

namespace game {
namespace {

using namespace layout;
using stereo::Mat34;
using stereo::Mat44;
using stereo::Vec3;

using DoUpdateFn = int (*)(void* renderer, void* gameContext, unsigned arg);
using InitFromCameraFn = void (*)(void* viewContext, const void* camera, const void* viewport, const void* offset);
using PerspectiveFn = float* (*)(float* out, float hfov, float nearZ, float farZ, float aspect);
using SetViewProjectionFn = void (*)(void* transformMatrix, const float* view, const float* proj);
using RebuildFrustumFn = void (*)(void* viewContext);
using AfterPhysicsStepFn = int (*)(void* ttCamera, void* gameContext);
using ActorSetWorldMatrixFn = void (*)(void* actor, const float* matrix34);
using PrepareViewFn = void (*)(void* renderContext, const void* viewContext);
using CullingFn = void (*)(void* renderLoop, void* viewContext);
using LoopRenderFn = void (*)(void* renderLoop, void* renderDesc);

DoUpdateFn g_doUpdate = nullptr;
InitFromCameraFn g_initFromCamera = nullptr;
PerspectiveFn g_perspective = nullptr;
SetViewProjectionFn g_setViewProjection = nullptr;
RebuildFrustumFn g_rebuildFrustum = nullptr;
AfterPhysicsStepFn g_afterPhysicsStep = nullptr;
ActorSetWorldMatrixFn g_actorSetWorldMatrix = nullptr;
PrepareViewFn g_prepareView = nullptr;
CullingFn g_prepareForCulling = nullptr;
CullingFn g_restoreAfterCulling = nullptr;
LoopRenderFn g_loopRender = nullptr;
bool g_active = false;
bool g_eyeHooks = false; // the four per-view context hooks above are in (Phase 4b)
bool g_helmetActive = false;

// Main thread state.
CameraSettings g_settings;
CameraInfo g_info;
const void* g_mainCamera = nullptr; // this frame's main view camera, set in DoUpdate
void* g_renderer = nullptr; // DoUpdate's Renderer (views 0 and 1 are its ViewContexts)
const void* g_loggedCamera = nullptr;
bool g_inDoUpdate = false;
// Head tracking: this frame's eye transforms in camera-local axes (valid while g_head) and the head (eye midpoint,
// left eye orientation) for the helmet mesh.
bool g_head = false;
Mat34 g_eyeLocal[2] = {stereo::Identity34(), stereo::Identity34()};
Mat34 g_headLocal = stereo::Identity34();
// Camera-local rotation that undoes the camera mode's look rotations this frame (identity when not removing them):
// D = R(mode+0x8)^T * R(mode+0x3e4). docs/re/camera.md section 9.
Mat34 g_autoLookFix = stereo::Identity34();
// The frame the head pose goes onto, camera-local: D * horizon lock (PLAN 8b). Set at the frame's first main view
// (g_baseReady), the same for both eyes, the helmet and the cascade-fitting head view.
Mat34 g_baseFix = stereo::Identity34();
bool g_baseReady = false;
// The same frame relative to the mode matrix M (mode+0x8), which the helmet is placed from: S^k * D * lock, so the
// helmet stays on the head through the shake the view shows (PLAN 8c).
Mat34 g_helmetFix = stereo::Identity34();
// This frame's mode matrix (rotation only), read in DoUpdate after KTUCamera::Update: W = M * S, S = the shakes.
Mat34 g_modeMatrix = stereo::Identity34();
bool g_haveModeMatrix = false;
// Largest roll magnitude (degrees) in the current and the previous 10 s window.
struct RollWindow {
    float current = 0, previous = 0;
    std::chrono::steady_clock::time_point start{};
    void Add(float deg, std::chrono::steady_clock::time_point now)
    {
        if (now - start > std::chrono::seconds(10)) {
            previous = current;
            current = 0;
            start = now;
        }
        current = std::max(current, std::fabs(deg));
    }
    float Max() const { return std::max(current, previous); }
};
RollWindow g_gameRoll, g_shownRoll, g_shake;

// user.toml (PLAN Decisions log 2026-10-21): the runtime settings, saved 1 s after the last change.
// horizon lock, shake, seat forward / up / right, world scale, ipd scale, then [hud] distance and height and [xr]
// render_scale and fsr_sharpness (changed in the VR Options tab through core::SetConfig).
using UserValues = std::array<float, 11>;
UserValues UserValuesOf(const CameraSettings& s)
{
    const auto& c = core::GetConfig();
    return {s.horizonLock, s.shake, s.seat[0], s.seat[1], s.seat[2], s.worldScale, s.ipdScale, c.hud.distance,
        c.hud.height, c.xr.render_scale, c.xr.fsr_sharpness};
}
UserValues g_savedUser{}, g_seenUser{};
std::chrono::steady_clock::time_point g_userChanged{};

// Writes the runtime settings that differ from tt2vr.toml (everything else keeps coming from there).
void SaveUserSettings(const UserValues& v)
{
    const auto& f = core::GetFileConfig();
    const struct {
        const char* section;
        const char* key;
        float file;
    } keys[] = {{"camera", "horizon_lock", f.camera.horizon_lock}, {"camera", "shake", f.camera.shake},
        {"camera", "seat_forward", f.camera.seat_forward}, {"camera", "seat_up", f.camera.seat_up},
        {"camera", "seat_right", f.camera.seat_right}, {"xr", "world_scale", f.xr.world_scale},
        {"xr", "ipd_scale", f.xr.ipd_scale}, {"hud", "distance", f.hud.distance}, {"hud", "height", f.hud.height},
        {"xr", "render_scale", f.xr.render_scale}, {"xr", "fsr_sharpness", f.xr.fsr_sharpness}};
    static_assert(std::size(keys) == std::tuple_size_v<UserValues>);
    std::vector<core::UserValue> out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (std::fabs(v[i] - keys[i].file) > 1e-5f) {
            out.push_back({keys[i].section, keys[i].key, v[i]});
        }
    }
    std::string error;
    if (core::WriteUserToml(core::UserTomlPath(), out, error)) {
        TT2VR_LOG_INFO("camera: {} runtime setting(s) saved to {}", out.size(), core::UserTomlPath().string());
    }
    else {
        TT2VR_LOG_WARN("camera: user.toml not saved: {}", error);
    }
}

void SaveUserSettingsWhenStable()
{
    if (!g_active) {
        return; // settings never initialised from the config
    }
    const UserValues now = UserValuesOf(g_settings);
    const auto t = std::chrono::steady_clock::now();
    if (now != g_seenUser) {
        g_seenUser = now;
        g_userChanged = t;
    }
    else if (now != g_savedUser && t - g_userChanged > std::chrono::seconds(1)) {
        SaveUserSettings(now);
        g_savedUser = now;
    }
}

// Read by MatrixPerspectiveFovRH, which may also run on render job threads.
std::atomic<bool> g_projActive{false};
std::atomic<std::uint32_t> g_mainFovBits{0};
std::atomic<std::uint32_t> g_frontFovBits{0};
std::atomic<float> g_shiftX{0};
std::atomic<float> g_shiftY{0};
std::atomic<bool> g_headProj{false}; // reshape to the eyes' XrFovf tangents instead of shifting the engine's
// Phase 4b: the two views are the two eyes. MatrixPerspectiveFovRH then takes the current eye's tangents (t_eye),
// or the union of both frusta where no single eye applies (cascade fitting in the scene phase, job threads).
std::atomic<bool> g_perEye{false};
struct AtomicTangents {
    std::atomic<float> l{0}, r{0}, u{0}, d{0};
    void Store(const stereo::FovTangents& t)
    {
        l = t.left;
        r = t.right;
        u = t.up;
        d = t.down;
    }
    stereo::FovTangents Load() const
    {
        return {l.load(std::memory_order_relaxed), r.load(std::memory_order_relaxed), u.load(std::memory_order_relaxed),
            d.load(std::memory_order_relaxed)};
    }
};
AtomicTangents g_tan[3]; // eye 0, eye 1, union
std::atomic<std::uint32_t> g_projCount{0};
std::atomic<std::uint32_t> g_projUnion{0}; // reshaped with the union (no current eye) in the last frame
// Current eye of this thread's projection work: set while InitFromCamera builds view k, while culling prepares or
// restores a view, and from RenderContext::Prepare(view) on the render loop's main context until the next view or
// the end of DeferredRenderLoop::Render. -1 = none.
thread_local int t_eye = -1;

// Helmet: snapshot of the camera-local transform, published at Present (main thread) and read in the physics
// step (main thread as far as known; the mutex keeps it safe if the physics step ever runs elsewhere).
std::mutex g_helmetMutex;
bool g_helmetApply = false;
Mat34 g_helmetLocal = stereo::Identity34();
std::atomic<std::uint32_t> g_helmetCount{0};
thread_local const void* t_helmetActor = nullptr; // non-null only inside TTCamera::AfterPhysicsStep
// Last local-player TT2 camera controller seen in its physics step; split view mirrors its per-camera-index state
// into view 1.
std::atomic<const void*> g_riderCamera{nullptr};

constexpr float kDegToRad = std::numbers::pi_v<float> / 180.0f;
constexpr float kRadToDeg = 180.0f / std::numbers::pi_v<float>;

void* At(void* base, std::ptrdiff_t offset)
{
    return static_cast<char*>(base) + offset;
}

// The helmet camera mode turns its view after building it: LookAroundInput 0x1401a0a70 post-multiplies the
// Dynamic View yaw (TTDriverAnim+0x120, toward a racing-line point ahead, |yaw| <= 0.25 rad) and pre-multiplies the
// stick look, mode+0x3e4 -> mode+0x8. D = R(+0x8)^T R(+0x3e4) in camera-local axes removes both; translation, lean,
// tuck and shakes stay. Only for RiderView::Cockpit(): in chase views and while crashed +0x3e4 is an unrelated bike
// frame (docs/re/camera.md 10.3). Read-only; SEH: game objects.
bool ReadAutoLookFix(const void* mode, Mat34& d)
{
    __try {
        Mat34 post = Read<Mat34>(mode, kModeCameraMatrix);
        Mat34 pre = Read<Mat34>(mode, kModePreLookMatrix);
        for (int r = 0; r < 3; ++r) {
            post.m[r][3] = 0.0f;
            pre.m[r][3] = 0.0f;
        }
        d = stereo::Mul(stereo::RigidInverse(post), pre);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The mode's output matrix (before KTUCamera::Update's shakes; translation dropped) and the last physics step's speed
// noise values, for the shake scale (PLAN 8c). SEH: game object, read only.
bool ReadModeState(const void* mode, Mat34& matrix, float& speedNoise, float& rollNoise)
{
    __try {
        matrix = Read<Mat34>(mode, kModeCameraMatrix);
        for (int r = 0; r < 3; ++r) {
            matrix.m[r][3] = 0.0f;
        }
        speedNoise = Read<float>(mode, kModeSpeedNoise);
        rollNoise = Read<float>(mode, kModeSpeedRollNoise);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The player camera's active mode (the override mode if one is set, e.g. Ansel or free camera): a change is a camera
// cut for the history reset. SEH: game objects, read only.
const void* CameraModeKey(const void* ktuCamera)
{
    __try {
        if (!ktuCamera) {
            return nullptr;
        }
        const void* override = Read<const void*>(ktuCamera, kKTUCameraOverrideMode);
        return override ? override : Read<const void*>(ktuCamera, kKTUCameraMode);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Sets up this frame's main view from `camera`. Returns true when it should be rendered as two views (Phase 4).
bool BeginMainView(const void* camera)
{
    g_mainCamera = camera;
    g_mainFovBits = std::bit_cast<std::uint32_t>(Read<float>(camera, kCameraFov));
    g_frontFovBits = std::bit_cast<std::uint32_t>(Read<float>(camera, kCameraFrontFov));
    g_shiftX = g_settings.projEnabled ? g_settings.shiftX : 0.0f;
    g_shiftY = g_settings.projEnabled ? g_settings.shiftY : 0.0f;
    // Pose and FOV of both eyes from xrLocateViews at this frame's predicted display time. Menus and the pause screen
    // are not head-tracked: xr shows the flat frame on a world-fixed screen instead. The TT2 pause over the riding
    // view stays head-tracked in two views while the HUD capture takes its UI onto a panel (PLAN 7d).
    const MenuState state = ReadMenuState(camera);
    const RiderView rider = ReadRiderView(LastPlayerCamera(), camera);
    // Speed noise at its source while the headset session runs; outside it the game's values are put back (PLAN 8c).
    ApplyCameraNoise(rider.settings, xr::SessionRunning() ? g_settings.shake / 100.0f : 1.0f);
    g_haveModeMatrix = rider.valid && ReadModeState(rider.mode, g_modeMatrix, g_info.speedNoise, g_info.rollNoise);
    const bool menu = core::GetConfig().xr.menu_screen && state.Active() && !(state.RidingPause() && HudCaptureReady());
    xr::FrameViews views;
    g_head = !menu && xr::LocateViews(views);
    if (g_head) {
        stereo::FovTangents t[2];
        for (int i = 0; i < 2; ++i) {
            const xr::EyeView& eye = views.eye[i];
            g_eyeLocal[i] = stereo::EyeLocal(views.recenter, eye.pose);
            t[i] = stereo::TangentsFromXrFov(eye.angleLeft, eye.angleRight, eye.angleUp, eye.angleDown);
            g_tan[i].Store(t[i]);
        }
        g_tan[2].Store(stereo::UnionTangents(t[0], t[1]));
        const stereo::Pose& l = views.eye[0].pose;
        const stereo::Pose& r = views.eye[1].pose;
        const stereo::Pose head{l.orientation, {(l.position.x + r.position.x) * 0.5f, (l.position.y + r.position.y) * 0.5f,
                                                   (l.position.z + r.position.z) * 0.5f}};
        g_headLocal = stereo::EyeLocal(views.recenter, head);
        // World and IPD scale (8e), then the seat offset (8d), in the camera's axes (x right, y up, z back).
        const Vec3 seat{g_settings.seat[2], g_settings.seat[1], -g_settings.seat[0]};
        stereo::ScaleEyes(g_eyeLocal, g_headLocal, g_settings.worldScale, g_settings.ipdScale, seat);
        const float dx = g_eyeLocal[1].m[0][3] - g_eyeLocal[0].m[0][3];
        const float dy = g_eyeLocal[1].m[1][3] - g_eyeLocal[0].m[1][3];
        const float dz = g_eyeLocal[1].m[2][3] - g_eyeLocal[0].m[2][3];
        OnTaaEyeDistance(std::sqrt(dx * dx + dy * dy + dz * dz)); // a scale change resets the history like an IPD change
        Mat34 fix;
        if (core::GetConfig().camera.remove_auto_look && rider.Cockpit() && ReadAutoLookFix(rider.mode, fix)) {
            g_autoLookFix = fix;
            g_info.autoLookYawDeg = std::atan2(fix.m[0][2], fix.m[0][0]) * kRadToDeg;
        }
    }
    g_headProj = g_head;
    g_projActive.store(g_settings.projEnabled || g_head, std::memory_order_release);
    OnTaaCameraMode(CameraModeKey(LastPlayerCamera()));
    return !menu && xr::SessionRunning();
}

void BeginFrame(void* renderer)
{
    g_mainCamera = nullptr;
    g_projActive = false;
    g_head = false;
    g_autoLookFix = stereo::Identity34();
    g_baseFix = stereo::Identity34();
    g_helmetFix = stereo::Identity34();
    g_baseReady = false;
    g_haveModeMatrix = false;
    g_info.autoLookYawDeg = 0.0f;
    OnSharedExposureFrame();
    OnTaaFrame(renderer, xr::RecenterCount());
    g_renderer = renderer;
    // The game's own CameraDesc, not the Renderer fields: while two views run those hold our desc.
    const void* camera = GameMainCamera(renderer);
    bool twoViews = false;
    if (camera && Read<int>(camera, kCameraProjType) == 0) {
        twoViews = BeginMainView(camera);
    }
    ApplySplitView(renderer, camera, twoViews, g_riderCamera.load(std::memory_order_relaxed));
    OnTaaTwoViews(SplitViewActive());
    xr::SetSideBySide(SplitViewActive());
    // Per-eye views need every projection rebuild to know its eye (the context hooks); without them both views keep
    // eye 0 (Phase 4a behaviour).
    g_perEye = g_head && SplitViewActive() && g_eyeHooks;
    UpdateVrEffects(xr::SessionRunning());
}

// View index of a DoUpdate ViewContext: 0 (Renderer+0x30), 1 (Renderer+0x650), or -1.
int ViewIndex(const void* viewContext)
{
    if (!g_renderer) {
        return -1;
    }
    const auto* base = static_cast<const char*>(g_renderer) + kRendererViewContexts;
    for (int i = 0; i < 2; ++i) {
        if (viewContext == base + i * kViewContextSize) {
            return i;
        }
    }
    return -1;
}

Mat34 LocalFromSettings(const CameraSettings& s)
{
    return stereo::CameraLocalTransform(s.yawDeg * kDegToRad, s.pitchDeg * kDegToRad, s.rollDeg * kDegToRad,
        {s.offset[0], s.offset[1], s.offset[2]});
}

// Camera-local transform of the rendered view: overlay sliders (if on), then the eye (if head-tracked). eye -1 =
// the head (eye midpoint), for the helmet mesh.
Mat34 ViewLocal(int eye)
{
    const Mat34 sliders = g_settings.viewEnabled ? LocalFromSettings(g_settings) : stereo::Identity34();
    if (!g_head) {
        return sliders;
    }
    return stereo::Mul(sliders, eye == 0 || eye == 1 ? g_eyeLocal[eye] : g_headLocal);
}

// Eye rendered by DoUpdate view `view` this frame.
int EyeOfView(int view)
{
    return g_perEye.load(std::memory_order_relaxed) && view == 1 ? 1 : 0;
}

int EyeOfCameraIndex(const void* viewContext)
{
    const auto index = Read<std::uint16_t>(viewContext, kViewCameraIndex);
    return index < 2 ? static_cast<int>(index) : -1;
}

void RecordCamera(void* viewContext, const void* camera)
{
    CameraInfo& info = g_info;
    info.valid = true;
    ++info.updates;
    info.camera = camera;
    info.fov = Read<float>(camera, kCameraFov);
    info.nearZ = Read<float>(camera, kCameraNear);
    info.farZ = Read<float>(camera, kCameraFar);
    info.aspectMul = Read<float>(camera, kCameraAspectMul);
    info.frontFov = Read<float>(camera, kCameraFrontFov);
    info.frontNear = Read<float>(camera, kCameraFrontNear);
    info.frontFar = Read<float>(camera, kCameraFrontFar);
    info.frontPass = Read<std::uint8_t>(camera, kCameraFrontPass) != 0;
    info.viewportW = Read<int>(viewContext, kViewViewportW);
    info.viewportH = Read<int>(viewContext, kViewViewportH);
    const Mat34 world = stereo::ToMat34(Read<Mat44>(viewContext, kViewTransform + kTmWorld));
    std::memcpy(info.world, world.m, sizeof(info.world));
    if (camera != g_loggedCamera) {
        g_loggedCamera = camera;
        TT2VR_LOG_INFO("camera: main view camera {} (hfov {:.2f} deg, near {}, far {}, front pass {} fov {:.2f} deg "
                       "near {} far {}, aspect mul {}, viewport {}x{})",
            camera, info.fov * kRadToDeg, info.nearZ, info.farZ, info.frontPass, info.frontFov * kRadToDeg,
            info.frontNear, info.frontFar, info.aspectMul, info.viewportW, info.viewportH);
        TT2VR_LOG_INFO("camera world: [{:.4f} {:.4f} {:.4f} {:.3f}] [{:.4f} {:.4f} {:.4f} {:.3f}] "
                       "[{:.4f} {:.4f} {:.4f} {:.3f}]",
            world.m[0][0], world.m[0][1], world.m[0][2], world.m[0][3], world.m[1][0], world.m[1][1], world.m[1][2],
            world.m[1][3], world.m[2][0], world.m[2][1], world.m[2][2], world.m[2][3]);
    }
}

// Horizon lock (PLAN 8b), once per frame at the first main view: g_baseFix = D * HorizonLock(W * D), so the head pose
// goes onto a frame with (100 - horizonLock)% of the game camera's roll. W = the game camera (pool matrix, shakes
// included); the roll is measured, not rebuilt from the game's lean and noise fields (docs/re/camera.md 10.1).
// Shake scale (PLAN 8c): S = R(M)^T R(W) is the pure KTUCamera shake rotation (docs/re/camera.md 10.2);
// K = S^(k-1) in W's axes turns W into M * S^k. Rotations over 10 deg are not a shake (a mismatch): left alone.
void UpdateBaseFix(const Mat34& world)
{
    g_baseFix = g_autoLookFix;
    g_helmetFix = g_autoLookFix;
    g_info.shakeDeg = 0.0f;
    if (g_head) {
        const float k = g_settings.shake / 100.0f;
        Mat34 shake = stereo::Identity34(), keep = stereo::Identity34(), fix = stereo::Identity34();
        if (g_haveModeMatrix) {
            Mat34 rendered = world;
            for (int r = 0; r < 3; ++r) {
                rendered.m[r][3] = 0.0f;
            }
            shake = stereo::Mul(stereo::RigidInverse(g_modeMatrix), rendered);
            const float angle = stereo::RotationAngle(shake);
            if (angle < 10.0f / kRadToDeg) {
                g_info.shakeDeg = angle * kRadToDeg;
                keep = stereo::ScaleRotation(shake, k);
                fix = stereo::ScaleRotation(shake, k - 1.0f);
            }
        }
        const Mat34 look = stereo::Mul(fix, g_autoLookFix);
        const Mat34 lock = stereo::HorizonLock(stereo::Mul(world, look), g_settings.horizonLock / 100.0f);
        g_baseFix = stereo::Mul(look, lock);
        g_helmetFix = stereo::Mul(keep, stereo::Mul(g_autoLookFix, lock));
    }
    g_baseReady = true;
    const auto now = std::chrono::steady_clock::now();
    g_shake.Add(g_info.shakeDeg * std::clamp(g_settings.shake / 100.0f, 0.0f, 1.0f), now);
    g_info.shakeShownMaxDeg = g_shake.Max();
    g_info.gameRollDeg = stereo::HorizonRoll(world) * kRadToDeg;
    g_info.shownRollDeg = stereo::HorizonRoll(stereo::Mul(world, g_baseFix)) * kRadToDeg;
    g_gameRoll.Add(g_info.gameRollDeg, now);
    g_shownRoll.Add(g_info.shownRollDeg, now);
    g_info.gameRollMaxDeg = g_gameRoll.Max();
    g_info.shownRollMaxDeg = g_shownRoll.Max();
}

// Rebuilds the main ViewContext from world * local (CameraSettings). Same steps as the tail of InitFromCamera:
// camera position, view (= inverse world), SetViewProjection with the projection already built, RebuildFrustum.
void ApplyViewOverride(void* viewContext, int eye)
{
    const Mat34 world = stereo::ToMat34(Read<Mat44>(viewContext, kViewTransform + kTmWorld));
    const Mat34 moved = stereo::ComposeCameraLocal(world, stereo::Mul(g_baseFix, ViewLocal(eye)));
    alignas(16) const Mat44 view = stereo::ViewFromWorld(moved);
    alignas(16) const Mat44 proj = Read<Mat44>(viewContext, kViewTransform + kTmProj);
    const Vec3 oldPos = Read<Vec3>(viewContext, kViewCamPos);
    const Vec3 cullPos = Read<Vec3>(viewContext, kViewCullPos);
    const Vec3 newPos = stereo::Translation(moved);
    Write(viewContext, kViewCamPos, newPos);
    if (std::memcmp(&oldPos, &cullPos, sizeof(Vec3)) == 0) { // not frozen by g_bLockCullingQuery
        Write(viewContext, kViewCullPos, newPos);
    }
    g_setViewProjection(At(viewContext, kViewTransform), &view.m[0][0], &proj.m[0][0]);
    g_rebuildFrustum(viewContext);
}

// Thread: game main thread (GameContext::RenderOneFrame -> RendererWin32::OnUpdate -> Renderer::DoUpdate).
int HookedDoUpdate(void* renderer, void* gameContext, unsigned arg)
{
    d3d::NoteDoUpdate(); // frame-spike log: the game update ends here
    try {
        BeginFrame(renderer);
    }
    catch (...) {
        g_mainCamera = nullptr;
        g_projActive = false;
    }
    g_inDoUpdate = true;
    const int result = g_doUpdate(renderer, gameContext, arg);
    g_inDoUpdate = false;
    return result;
}

// Thread: game main thread (DoUpdate, UpdateShift, OnRender external scenes, probes, picking).
void HookedInitFromCamera(void* viewContext, const void* camera, const void* viewport, const void* offset)
{
    const bool main = g_inDoUpdate && camera && camera == g_mainCamera;
    const int view = main ? std::max(ViewIndex(viewContext), 0) : 0;
    const int eye = EyeOfView(view);
    const int savedEye = t_eye;
    if (main) {
        t_eye = eye; // the projection InitFromCamera builds is this eye's
    }
    g_initFromCamera(viewContext, camera, viewport, offset);
    t_eye = savedEye;
    if (!main) {
        return;
    }
    try {
        RecordCamera(viewContext, camera);
        if (view == 0) {
            OnMainCameraWorld(g_info.world);
        }
        if (!g_baseReady) {
            Mat34 world;
            std::memcpy(world.m, g_info.world, sizeof(world.m));
            UpdateBaseFix(world);
        }
        if (g_settings.viewEnabled || g_head) {
            ApplyViewOverride(viewContext, eye);
        }
        if (g_head) {
            // The headset gets this view's image with the pose and FOV it was rendered from.
            xr::MarkRendered(view, eye);
        }
        g_info.headTracked = g_head;
        if (view == 0 || view == 1) {
            const Vec3 pos = Read<Vec3>(viewContext, kViewCamPos);
            g_info.viewPos[view][0] = pos.x;
            g_info.viewPos[view][1] = pos.y;
            g_info.viewPos[view][2] = pos.z;
        }
        std::memcpy(g_info.proj, At(viewContext, kViewTransform + kTmProj), sizeof(g_info.proj));
        std::memcpy(g_info.jitter, At(viewContext, kViewProjOffset), sizeof(g_info.jitter));
    }
    catch (...) {
    }
}

// Thread: physics step (CameraManager AfterPhysicsStep; game main thread as far as known).
int HookedAfterPhysicsStep(void* ttCamera, void* gameContext)
{
    bool apply = false;
    {
        std::lock_guard lock(g_helmetMutex);
        apply = g_helmetApply;
    }
    t_helmetActor = apply && ttCamera ? Read<const void*>(ttCamera, kTTCameraHelmetActor) : nullptr;
    if (IsLocalRiderCamera(ttCamera)) {
        g_riderCamera.store(ttCamera, std::memory_order_relaxed);
    }
    const int result = g_afterPhysicsStep(ttCamera, gameContext);
    t_helmetActor = nullptr;
    return result;
}

// Thread: any caller of Actor::SetWorldMatrix. Acts only inside HookedAfterPhysicsStep for the HelmetView actor,
// where the game passes &mode->helmetMatrix (FUN_1403afc80, docs/re/camera.md section 7).
void HookedActorSetWorldMatrix(void* actor, const float* matrix)
{
    if (!t_helmetActor || actor != t_helmetActor || !matrix) {
        g_actorSetWorldMatrix(actor, matrix);
        return;
    }
    try {
        const char* mode = reinterpret_cast<const char*>(matrix) - kModeHelmetMatrix;
        const Mat34 helmet = Read<Mat34>(matrix, 0);
        const Mat34 pivot = Read<Mat34>(mode, kModeCameraMatrix); // the matrix HelmetSprings built `helmet` from
        Mat34 local;
        {
            std::lock_guard lock(g_helmetMutex);
            local = g_helmetLocal;
        }
        const Mat34 pivotInv = stereo::ToMat34(stereo::Inverse(stereo::ToMat44(pivot)));
        alignas(16) const Mat34 moved =
            stereo::Mul(stereo::ComposeCameraLocal(pivot, local), stereo::Mul(pivotInv, helmet));
        g_actorSetWorldMatrix(actor, &moved.m[0][0]);
        g_helmetCount.fetch_add(1, std::memory_order_relaxed);
    }
    catch (...) {
        g_actorSetWorldMatrix(actor, matrix);
    }
}

// Thread: any (main thread; render jobs such as CSM setup may call it too). Only atomics and thread-locals are read.
float* HookedPerspective(float* out, float hfov, float nearZ, float farZ, float aspect)
{
    float* result = g_perspective(out, hfov, nearZ, farZ, aspect);
    if (!g_projActive.load(std::memory_order_acquire) || !result) {
        return result;
    }
    const auto bits = std::bit_cast<std::uint32_t>(hfov);
    if (bits != g_mainFovBits.load(std::memory_order_relaxed) && bits != g_frontFovBits.load(std::memory_order_relaxed)) {
        return result;
    }
    Mat44 m;
    std::memcpy(&m, result, sizeof(m));
    int which = 0; // one view: eye 0 (or the only rendered view)
    if (g_perEye.load(std::memory_order_relaxed)) {
        which = t_eye == 0 || t_eye == 1 ? t_eye : 2;
        if (which == 2) {
            g_projUnion.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const stereo::FovTangents base =
        g_headProj.load(std::memory_order_relaxed) ? g_tan[which].Load() : stereo::TangentsFromProjection(m);
    const stereo::FovTangents t = stereo::OffsetTangents(base, g_shiftX.load(std::memory_order_relaxed),
        g_shiftY.load(std::memory_order_relaxed));
    stereo::ApplyTangents(m, t);
    std::memcpy(result, &m, sizeof(m));
    g_projCount.fetch_add(1, std::memory_order_relaxed);
    return result;
}

std::string Row(const float* r, int n)
{
    std::string s = "[";
    for (int i = 0; i < n; ++i) {
        s += fmt::format("{}{:.6g}", i ? " " : "", r[i]);
    }
    return s + "]";
}

// Thread: game main thread (DeferredRenderLoop::Render, per view on the main RenderContext; also every other
// ViewContext copy into a RenderContext, which is ignored).
void HookedPrepareView(void* renderContext, const void* viewContext)
{
    void* mainLoop = nullptr;
    if (g_renderer && viewContext) {
        void* loop = Read<void*>(g_renderer, kRendererRenderLoop);
        if (loop && renderContext == static_cast<char*>(loop) + kRenderLoopMainContext) {
            mainLoop = loop;
            t_eye = EyeOfCameraIndex(viewContext);
            GpuTimersView(viewContext);
        }
    }
    g_prepareView(renderContext, viewContext);
    if (mainLoop) {
        OnTaaViewPrepared(mainLoop, renderContext); // history reset: previous camera = this view's
    }
}

// Thread: game main thread (scene phase of DeferredRenderLoop::Render, per view, front pass on).
void HookedPrepareForCulling(void* renderLoop, void* viewContext)
{
    const int saved = t_eye;
    t_eye = viewContext ? EyeOfCameraIndex(viewContext) : -1;
    g_prepareForCulling(renderLoop, viewContext);
    t_eye = saved;
}

void HookedRestoreAfterCulling(void* renderLoop, void* viewContext)
{
    const int saved = t_eye;
    t_eye = viewContext ? EyeOfCameraIndex(viewContext) : -1;
    g_restoreAfterCulling(renderLoop, viewContext);
    t_eye = saved;
}

// Thread: game main thread (Renderer::OnRender, also the external-scene loop).
void HookedLoopRender(void* renderLoop, void* renderDesc)
{
    t_eye = -1; // the scene phase (culling, cascade fitting) serves both eyes
    void* renderer = g_renderer && Read<void*>(g_renderer, kRendererRenderLoop) == renderLoop ? g_renderer : nullptr;
    BeginLoopRender(renderer); // the main render only (not external scenes)
    BeginHudLoopRender(renderer, renderer ? renderDesc : nullptr);
    if (renderer) {
        ForceVrResolution(xr::SessionRunning());
        ApplyVrPostEffects(renderLoop, xr::SessionRunning());
        OnTaaLoopRender(renderLoop); // history reset: froxel index, last-frame buffers
        OnSlotMirrorLoopRender(renderLoop, SplitViewActive()); // visor drops, crash grey for the right eye
        OnTemporalDebugLoopRender(renderLoop);
    }
    GpuTimersLoopBegin(renderer);
    g_loopRender(renderLoop, renderDesc);
    GpuTimersLoopEnd();
    EndHudLoopRender();
    EndLoopRender(renderer);
    t_eye = -1;
}

} // namespace

CameraSettings& Settings()
{
    return g_settings;
}

void NudgeSeat(float forward, float up)
{
    g_settings.seat[0] = std::clamp(g_settings.seat[0] + forward, -0.5f, 0.5f);
    g_settings.seat[1] = std::clamp(g_settings.seat[1] + up, -0.5f, 0.5f);
    TT2VR_LOG_INFO("camera: seat offset forward {:.2f} up {:.2f} right {:.2f} m", g_settings.seat[0], g_settings.seat[1],
        g_settings.seat[2]);
}

void ResetSeat()
{
    g_settings.seat[0] = g_settings.seat[1] = g_settings.seat[2] = 0.0f;
    TT2VR_LOG_INFO("camera: seat offset reset");
}

CameraInfo LastCameraInfo()
{
    return g_info;
}

namespace {
// The view BeginHeadCenterView replaced: camera and cull positions, then the TransformMatrix.
unsigned char g_savedView[kViewTransform + kTransformMatrixSize];
} // namespace

bool BeginHeadCenterView(void* viewContext)
{
    if (!viewContext || !g_head || !g_info.valid) {
        return false;
    }
    std::memcpy(g_savedView, viewContext, sizeof(g_savedView));
    // g_info.world is the engine camera before our override (RecordCamera), the same for both views.
    Mat34 world;
    std::memcpy(world.m, g_info.world, sizeof(world.m));
    const Mat34 head = stereo::ComposeCameraLocal(world, stereo::Mul(g_baseFix, ViewLocal(-1)));
    alignas(16) const Mat44 view = stereo::ViewFromWorld(head);
    alignas(16) const Mat44 proj = Read<Mat44>(viewContext, kViewTransform + kTmProj);
    const Vec3 pos = stereo::Translation(head);
    Write(viewContext, kViewCamPos, pos);
    Write(viewContext, kViewCullPos, pos);
    g_setViewProjection(At(viewContext, kViewTransform), &view.m[0][0], &proj.m[0][0]);
    return true;
}

void EndHeadCenterView(void* viewContext)
{
    if (viewContext) {
        std::memcpy(viewContext, g_savedView, sizeof(g_savedView));
    }
}

bool InstallCameraHooks()
{
    static bool s_done = false;
    if (s_done) {
        return g_active;
    }
    s_done = true;
    g_settings.helmetFollows = core::GetConfig().camera.helmet_follows_view;
    g_settings.horizonLock = core::GetConfig().camera.horizon_lock;
    g_settings.shake = core::GetConfig().camera.shake;
    const auto& cc = core::GetConfig().camera;
    g_settings.seat[0] = cc.seat_forward;
    g_settings.seat[1] = cc.seat_up;
    g_settings.seat[2] = cc.seat_right;
    g_settings.worldScale = core::GetConfig().xr.world_scale;
    g_settings.ipdScale = core::GetConfig().xr.ipd_scale;
    g_savedUser = g_seenUser = UserValuesOf(g_settings);
    ResolveSignatures();
    for (const Sig required : {Sig::RendererDoUpdate, Sig::ViewContextInitFromCamera, Sig::MatrixPerspectiveFovRH,
             Sig::TransformMatrixSetViewProjection, Sig::ViewContextRebuildFrustum}) {
        if (!SignatureAddress(required)) {
            TT2VR_LOG_ERROR("camera: signatures missing; camera override disabled, game renders unmodified");
            return false;
        }
    }
    g_setViewProjection = reinterpret_cast<SetViewProjectionFn>(SignatureAddress(Sig::TransformMatrixSetViewProjection));
    g_rebuildFrustum = reinterpret_cast<RebuildFrustumFn>(SignatureAddress(Sig::ViewContextRebuildFrustum));
    using core::hooks::Result;
    // DoUpdate last: until it is hooked g_mainCamera stays null and the other two hooks pass through.
    const bool ok =
        core::hooks::Create("Math::MatrixPerspectiveFovRH", SignatureAddress(Sig::MatrixPerspectiveFovRH),
            &HookedPerspective, &g_perspective) == Result::Ok &&
        core::hooks::Create("ViewContext::InitFromCamera", SignatureAddress(Sig::ViewContextInitFromCamera),
            &HookedInitFromCamera, &g_initFromCamera) == Result::Ok &&
        core::hooks::Create("Renderer::DoUpdate", SignatureAddress(Sig::RendererDoUpdate), &HookedDoUpdate,
            &g_doUpdate) == Result::Ok;
    g_active = ok;
    if (ok) {
        TT2VR_LOG_INFO("camera: hooks installed (DoUpdate, InitFromCamera, MatrixPerspectiveFovRH)");
    }
    else {
        TT2VR_LOG_ERROR("camera: hook install failed; camera override disabled");
        return false;
    }
    // Optional: the helmet keeps the game's placement if these are missing.
    if (SignatureAddress(Sig::TTCameraAfterPhysicsStep) && SignatureAddress(Sig::ActorSetWorldMatrix)) {
        g_helmetActive =
            core::hooks::Create("Actor::SetWorldMatrix", SignatureAddress(Sig::ActorSetWorldMatrix),
                &HookedActorSetWorldMatrix, &g_actorSetWorldMatrix) == Result::Ok &&
            core::hooks::Create("TTCamera::AfterPhysicsStep", SignatureAddress(Sig::TTCameraAfterPhysicsStep),
                &HookedAfterPhysicsStep, &g_afterPhysicsStep) == Result::Ok;
    }
    if (g_helmetActive) {
        TT2VR_LOG_INFO("camera: helmet hooks installed (helmet follows view: {})", g_settings.helmetFollows);
    }
    else {
        TT2VR_LOG_WARN("camera: helmet hooks not installed; the helmet mesh keeps the game's placement");
    }
    // Phase 4b: per-eye projections need to know the eye of every projection rebuild. All four or none.
    void* prepare = SignatureAddress(Sig::RenderContextPrepareView);
    void* cull = SignatureAddress(Sig::RenderLoopPrepareForCulling);
    void* restore = SignatureAddress(Sig::RenderLoopRestoreAfterCulling);
    void* render = SignatureAddress(Sig::DeferredRenderLoopRender);
    if (prepare && cull && restore && render) {
        g_eyeHooks =
            core::hooks::Create("RenderContext::Prepare(ViewContext)", prepare, &HookedPrepareView, &g_prepareView) ==
                Result::Ok &&
            core::hooks::Create("RenderLoop::PrepareForCulling", cull, &HookedPrepareForCulling, &g_prepareForCulling) ==
                Result::Ok &&
            core::hooks::Create("RenderLoop::RestoreAfterCulling", restore, &HookedRestoreAfterCulling,
                &g_restoreAfterCulling) == Result::Ok &&
            core::hooks::Create("DeferredRenderLoop::Render", render, &HookedLoopRender, &g_loopRender) == Result::Ok;
    }
    if (g_eyeHooks) {
        TT2VR_LOG_INFO("camera: per-eye context hooks installed (two views = two eyes)");
    }
    else {
        TT2VR_LOG_WARN("camera: per-eye context hooks missing; both views use the left eye");
    }
    return ok;
}

bool CameraHooksActive()
{
    return g_active;
}

bool HelmetHooksActive()
{
    return g_helmetActive;
}

void OnPresent()
{
    SaveUserSettingsWhenStable();
    g_projActive = false;
    g_info.projOverrides = g_projCount.exchange(0);
    g_info.projUnion = g_projUnion.exchange(0);
    g_info.helmetMoves = g_helmetCount.exchange(0);
    std::lock_guard lock(g_helmetMutex);
    g_helmetApply = g_helmetActive && (g_settings.viewEnabled || g_head) && g_settings.helmetFollows;
    g_helmetLocal = stereo::Mul(g_helmetFix, ViewLocal(-1)); // this frame's head, applied at the next physics step
    g_head = false;
}

std::string ComfortCommand(const std::string& arg)
{
    std::istringstream in(arg);
    std::string name;
    float value = 0.0f;
    if (in >> name) {
        if (!(in >> value)) {
            return fmt::format("comfort FAILED: '{}' needs a value", name);
        }
        if (name == "lock") {
            g_settings.horizonLock = std::clamp(value, 0.0f, 100.0f);
        }
        else if (name == "shake") {
            g_settings.shake = std::clamp(value, 0.0f, 100.0f);
        }
        else if (name == "world") {
            g_settings.worldScale = std::clamp(value, 0.5f, 2.0f);
        }
        else if (name == "ipd") {
            g_settings.ipdScale = std::clamp(value, 0.5f, 2.0f);
        }
        else if (name == "forward" || name == "up" || name == "right") {
            g_settings.seat[name == "forward" ? 0 : name == "up" ? 1 : 2] = std::clamp(value, -0.5f, 0.5f);
        }
        else {
            return fmt::format("comfort FAILED: unknown setting '{}' (lock, shake, world, ipd, forward, up, right)", name);
        }
    }
    const CameraSettings& s = g_settings;
    return fmt::format("comfort lock {:.0f}% shake {:.0f}% world {:.2f} ipd {:.2f} seat forward {:.3f} up {:.3f} right "
                       "{:.3f} m",
        s.horizonLock, s.shake, s.worldScale, s.ipdScale, s.seat[0], s.seat[1], s.seat[2]);
}

std::string DescribeCamera()
{
    const CameraInfo& i = g_info;
    if (!i.valid) {
        return g_active ? "camera: no main view seen yet (menu or loading?)" : "camera: hooks not active";
    }
    const CameraSettings& s = g_settings;
    std::string out = fmt::format("camera {} after {} main-view updates\n", i.camera, i.updates);
    out += fmt::format("  Camera: hfov {:.4f} rad ({:.2f} deg), near {}, far {}, aspectMul {}\n", i.fov,
        i.fov * kRadToDeg, i.nearZ, i.farZ, i.aspectMul);
    out += fmt::format("  front pass {}: fov {:.4f} rad ({:.2f} deg), near {}, far {}\n", i.frontPass, i.frontFov,
        i.frontFov * kRadToDeg, i.frontNear, i.frontFar);
    out += fmt::format("  viewport {}x{}\n", i.viewportW, i.viewportH);
    for (int r = 0; r < 3; ++r) {
        out += fmt::format("  world row {} {}\n", r, Row(i.world[r], 4));
    }
    out += fmt::format("  forward (-col2) [{:.4f} {:.4f} {:.4f}]\n", -i.world[0][2], -i.world[1][2], -i.world[2][2]);
    for (int r = 0; r < 4; ++r) {
        out += fmt::format("  proj row {} {}\n", r, Row(i.proj[r], 4));
    }
    const float hfov = 2.0f * std::atan(1.0f / i.proj[0][0]) * kRadToDeg;
    const float vfov = 2.0f * std::atan(1.0f / i.proj[1][1]) * kRadToDeg;
    out += fmt::format("  proj hfov {:.2f} deg, vfov {:.2f} deg, jitter [{:.6g} {:.6g}]\n", hfov, vfov, i.jitter[0],
        i.jitter[1]);
    out += fmt::format("  head tracked {}, per-eye views {} ({} projections with the union frustum last frame)\n",
        i.headTracked, g_perEye.load(), i.projUnion);
    out += fmt::format("  rider view: {}\n", DescribeRiderView(LastRiderView()));
    out += fmt::format("  game look rotation removed: {:.2f} deg yaw\n", i.autoLookYawDeg);
    out += fmt::format("  horizon lock {:.0f}%: roll game {:.2f} deg (max {:.2f}), shown {:.2f} deg (max {:.2f})\n",
        s.horizonLock, i.gameRollDeg, i.gameRollMaxDeg, i.shownRollDeg, i.shownRollMaxDeg);
    const float ex = i.viewPos[1][0] - i.viewPos[0][0], ey = i.viewPos[1][1] - i.viewPos[0][1];
    const float ez = i.viewPos[1][2] - i.viewPos[0][2];
    out += fmt::format("  views at [{:.3f} {:.3f} {:.3f}] / [{:.3f} {:.3f} {:.3f}], {:.1f} mm apart; world {:.2f} ipd {:.2f} "
                       "seat forward {:.3f} up {:.3f} right {:.3f} m\n",
        i.viewPos[0][0], i.viewPos[0][1], i.viewPos[0][2], i.viewPos[1][0], i.viewPos[1][1], i.viewPos[1][2],
        std::sqrt(ex * ex + ey * ey + ez * ez) * 1000.0f, s.worldScale, s.ipdScale, s.seat[0], s.seat[1], s.seat[2]);
    out += fmt::format("  shake {:.0f}%: game shake {:.3f} deg, shown max {:.3f} deg; noise bob {:.4g} roll {:.4g}; {}\n",
        s.shake, i.shakeDeg, i.shakeShownMaxDeg, i.speedNoise, i.rollNoise, DescribeCameraNoise());
    out += fmt::format("  override: view {} (yaw {} pitch {} roll {} deg, offset [{} {} {}] m), proj {} (shift {} "
                       "{}), {} projection calls reshaped last frame; helmet follows {} (hooks {}), {} helmet "
                       "placements adjusted last frame",
        s.viewEnabled, s.yawDeg, s.pitchDeg, s.rollDeg, s.offset[0], s.offset[1], s.offset[2], s.projEnabled,
        s.shiftX, s.shiftY, i.projOverrides, s.helmetFollows, g_helmetActive, i.helmetMoves);
    return out;
}

std::string CameraSummary()
{
    const CameraInfo& i = g_info;
    if (!i.valid) {
        return g_active ? "camera: no main view yet" : "camera: hooks not active";
    }
    return fmt::format("camera ok hfov {:.2f} deg near {} far {} pos [{:.3f} {:.3f} {:.3f}] fwd [{:.3f} {:.3f} {:.3f}] "
                       "proj m00 {:.5g} m02 {:.5g} m11 {:.5g} m12 {:.5g} reshaped {}; {}; look removed {:.2f} deg; "
                       "roll game {:.2f} (max {:.2f}) shown {:.2f} (max {:.2f}) deg, lock {:.0f}%; shake {:.0f}% game "
                       "{:.3f} shown max {:.3f} deg, noise bob {:.4g} roll {:.4g}, {}",
        i.fov * kRadToDeg, i.nearZ, i.farZ, i.world[0][3], i.world[1][3], i.world[2][3], -i.world[0][2],
        -i.world[1][2], -i.world[2][2], i.proj[0][0], i.proj[0][2], i.proj[1][1], i.proj[1][2], i.projOverrides,
        DescribeRiderView(LastRiderView()), i.autoLookYawDeg, i.gameRollDeg, i.gameRollMaxDeg, i.shownRollDeg,
        i.shownRollMaxDeg, g_settings.horizonLock, g_settings.shake, i.shakeDeg, i.shakeShownMaxDeg, i.speedNoise,
        i.rollNoise, DescribeCameraNoise());
}

} // namespace game
