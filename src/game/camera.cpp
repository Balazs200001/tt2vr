#include "game/camera.hpp"

#include "core/config.hpp"
#include "core/hooks.hpp"
#include "core/log.hpp"
#include "game/engine_layout.hpp"
#include "game/menu_state.hpp"
#include "game/signatures.hpp"
#include "game/split_view.hpp"
#include "stereo/math/projection.hpp"
#include "stereo/math/xr_to_engine.hpp"
#include "xr/session.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <mutex>
#include <numbers>

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

DoUpdateFn g_doUpdate = nullptr;
InitFromCameraFn g_initFromCamera = nullptr;
PerspectiveFn g_perspective = nullptr;
SetViewProjectionFn g_setViewProjection = nullptr;
RebuildFrustumFn g_rebuildFrustum = nullptr;
AfterPhysicsStepFn g_afterPhysicsStep = nullptr;
ActorSetWorldMatrixFn g_actorSetWorldMatrix = nullptr;
bool g_active = false;
bool g_helmetActive = false;

// Main thread state.
CameraSettings g_settings;
CameraInfo g_info;
const void* g_mainCamera = nullptr; // this frame's main view camera, set in DoUpdate
void* g_renderer = nullptr; // DoUpdate's Renderer (views 0 and 1 are its ViewContexts)
const void* g_loggedCamera = nullptr;
bool g_inDoUpdate = false;
// Head tracking (Phase 3): this frame's eye 0 transform in camera-local axes, valid while g_head.
bool g_head = false;
Mat34 g_eyeLocal = stereo::Identity34();

// Read by MatrixPerspectiveFovRH, which may also run on render job threads.
std::atomic<bool> g_projActive{false};
std::atomic<std::uint32_t> g_mainFovBits{0};
std::atomic<std::uint32_t> g_frontFovBits{0};
std::atomic<float> g_shiftX{0};
std::atomic<float> g_shiftY{0};
std::atomic<bool> g_headProj{false}; // reshape to the eye's XrFovf tangents instead of shifting the engine's
std::atomic<float> g_tanL{0}, g_tanR{0}, g_tanU{0}, g_tanD{0};
std::atomic<std::uint32_t> g_projCount{0};

// Helmet: snapshot of the camera-local transform, published at Present (main thread) and read in the physics
// step (main thread as far as known; the mutex keeps it safe if the physics step ever runs elsewhere).
std::mutex g_helmetMutex;
bool g_helmetApply = false;
Mat34 g_helmetLocal = stereo::Identity34();
std::atomic<std::uint32_t> g_helmetCount{0};
thread_local const void* t_helmetActor = nullptr; // non-null only inside TTCamera::AfterPhysicsStep
// Last TT2 player camera seen in its physics step; split view mirrors its per-camera-index state into view 1.
std::atomic<const void*> g_riderCamera{nullptr};

constexpr float kDegToRad = std::numbers::pi_v<float> / 180.0f;
constexpr float kRadToDeg = 180.0f / std::numbers::pi_v<float>;

void* At(void* base, std::ptrdiff_t offset)
{
    return static_cast<char*>(base) + offset;
}

// Sets up this frame's main view from `camera`. Returns true when it should be rendered as two views (Phase 4).
bool BeginMainView(const void* camera)
{
    g_mainCamera = camera;
    g_mainFovBits = std::bit_cast<std::uint32_t>(Read<float>(camera, kCameraFov));
    g_frontFovBits = std::bit_cast<std::uint32_t>(Read<float>(camera, kCameraFrontFov));
    g_shiftX = g_settings.projEnabled ? g_settings.shiftX : 0.0f;
    g_shiftY = g_settings.projEnabled ? g_settings.shiftY : 0.0f;
    // Phase 3: left eye only. Pose and FOV come from xrLocateViews at this frame's predicted display time.
    // Menus and the pause screen are not head-tracked: xr shows the flat frame on a world-fixed screen instead.
    const bool menu = core::GetConfig().xr.menu_screen && ReadMenuState(camera).Active();
    xr::FrameViews views;
    g_head = !menu && xr::LocateViews(views);
    if (g_head) {
        const xr::EyeView& eye = views.eye[0];
        g_eyeLocal = stereo::EyeLocal(views.recenter, eye.pose);
        const stereo::FovTangents t = stereo::TangentsFromXrFov(eye.angleLeft, eye.angleRight, eye.angleUp, eye.angleDown);
        g_tanL = t.left;
        g_tanR = t.right;
        g_tanU = t.up;
        g_tanD = t.down;
    }
    g_headProj = g_head;
    g_projActive.store(g_settings.projEnabled || g_head, std::memory_order_release);
    return !menu && xr::SessionRunning();
}

void BeginFrame(void* renderer)
{
    g_mainCamera = nullptr;
    g_projActive = false;
    g_head = false;
    g_renderer = renderer;
    // The game's own CameraDesc, not the Renderer fields: while two views run those hold our desc.
    const void* camera = GameMainCamera(renderer);
    bool twoViews = false;
    if (camera && Read<int>(camera, kCameraProjType) == 0) {
        twoViews = BeginMainView(camera);
    }
    ApplySplitView(renderer, camera, twoViews, g_riderCamera.load(std::memory_order_relaxed));
    xr::SetSideBySide(SplitViewActive());
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

// Camera-local transform of the rendered view: overlay sliders (if on), then the eye (if head-tracked).
Mat34 ViewLocal()
{
    const Mat34 sliders = g_settings.viewEnabled ? LocalFromSettings(g_settings) : stereo::Identity34();
    return g_head ? stereo::Mul(sliders, g_eyeLocal) : sliders;
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

// Rebuilds the main ViewContext from world * local (CameraSettings). Same steps as the tail of InitFromCamera:
// camera position, view (= inverse world), SetViewProjection with the projection already built, RebuildFrustum.
void ApplyViewOverride(void* viewContext)
{
    const Mat34 world = stereo::ToMat34(Read<Mat44>(viewContext, kViewTransform + kTmWorld));
    const Mat34 moved = stereo::ComposeCameraLocal(world, ViewLocal());
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
    g_initFromCamera(viewContext, camera, viewport, offset);
    if (!g_inDoUpdate || !camera || camera != g_mainCamera) {
        return;
    }
    try {
        RecordCamera(viewContext, camera);
        if (g_settings.viewEnabled || g_head) {
            ApplyViewOverride(viewContext);
        }
        if (g_head) {
            // The headset gets this view's image with the pose it was rendered from: eye 0 for both views until
            // per-eye poses (Phase 4b).
            xr::MarkRendered(std::max(ViewIndex(viewContext), 0), 0);
        }
        g_info.headTracked = g_head;
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
    g_riderCamera.store(ttCamera, std::memory_order_relaxed);
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

// Thread: any (main thread; render jobs such as CSM setup may call it too). Only atomics are read.
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
    const stereo::FovTangents base = g_headProj.load(std::memory_order_relaxed)
                                         ? stereo::FovTangents{g_tanL.load(std::memory_order_relaxed),
                                               g_tanR.load(std::memory_order_relaxed), g_tanU.load(std::memory_order_relaxed),
                                               g_tanD.load(std::memory_order_relaxed)}
                                         : stereo::TangentsFromProjection(m);
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

} // namespace

CameraSettings& Settings()
{
    return g_settings;
}

CameraInfo LastCameraInfo()
{
    return g_info;
}

bool InstallCameraHooks()
{
    static bool s_done = false;
    if (s_done) {
        return g_active;
    }
    s_done = true;
    g_settings.helmetFollows = core::GetConfig().camera.helmet_follows_view;
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
    g_projActive = false;
    g_info.projOverrides = g_projCount.exchange(0);
    g_info.helmetMoves = g_helmetCount.exchange(0);
    std::lock_guard lock(g_helmetMutex);
    g_helmetApply = g_helmetActive && (g_settings.viewEnabled || g_head) && g_settings.helmetFollows;
    g_helmetLocal = ViewLocal(); // this frame's view, applied at the next physics step
    g_head = false;
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
    out += fmt::format("  head tracked {} (left eye, Phase 3)\n", i.headTracked);
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
                       "proj m00 {:.5g} m02 {:.5g} m11 {:.5g} m12 {:.5g} reshaped {}",
        i.fov * kRadToDeg, i.nearZ, i.farZ, i.world[0][3], i.world[1][3], i.world[2][3], -i.world[0][2],
        -i.world[1][2], -i.world[2][2], i.proj[0][0], i.proj[0][2], i.proj[1][1], i.proj[1][2], i.projOverrides);
}

} // namespace game
