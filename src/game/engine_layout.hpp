// Engine struct offsets we read or write (build 4989319). Source of truth: docs/re/symbols.md sections 4a, 5a
// and the Renderer table; evidence in docs/re/camera.md. Offsets are data layout, not code addresses, so they
// are not signatures; a new exe build must re-check them (version table in game/version.hpp).
#pragma once

#include <cstddef>
#include <cstring>

namespace game::layout {

// Graphics::Camera (GraphNode-derived)
inline constexpr std::ptrdiff_t kCameraProjType = 0x58; // int, 0 = perspective
inline constexpr std::ptrdiff_t kCameraFov = 0x78; // float, HORIZONTAL fov in radians
inline constexpr std::ptrdiff_t kCameraNear = 0x7c;
inline constexpr std::ptrdiff_t kCameraFar = 0x80;
inline constexpr std::ptrdiff_t kCameraAspectMul = 0x84; // final aspect = vpW / vpH * this
inline constexpr std::ptrdiff_t kCameraFrontFov = 0x8c; // front pass (bike/rider foreground)
inline constexpr std::ptrdiff_t kCameraFrontNear = 0x90;
inline constexpr std::ptrdiff_t kCameraFrontFar = 0x94;
inline constexpr std::ptrdiff_t kCameraFrontPass = 0x98; // byte

// ViewContext (size 0x620)
inline constexpr std::ptrdiff_t kViewCamPos = 0x00; // float3, camera world position
inline constexpr std::ptrdiff_t kViewCullPos = 0x0c; // float3, frozen while g_bLockCullingQuery
inline constexpr std::ptrdiff_t kViewTransform = 0x18; // TransformMatrix (size kTransformMatrixSize)
inline constexpr std::size_t kTransformMatrixSize = 0x190;
inline constexpr std::ptrdiff_t kViewViewportW = 0x340; // int
inline constexpr std::ptrdiff_t kViewViewportH = 0x344; // int
inline constexpr std::ptrdiff_t kViewProjOffset = 0x448; // float2, TAA jitter / Ansel offset
inline constexpr std::ptrdiff_t kViewCamera = 0x450; // Camera*
inline constexpr std::ptrdiff_t kViewCullingPass = 0x460; // int (ECullingPass), 0 = main view, else shadow etc.
inline constexpr std::ptrdiff_t kViewCameraIndex = 0x464; // ushort, selects the per-slot history (0 / 1)
inline constexpr std::ptrdiff_t kViewHudList = 0x5e8; // HudItemList* (3D HUD flags 8 / 0x14, per-view 2D HUD 0x22)

// TransformMatrix slots (4x4, engine layout, stride 0x40)
inline constexpr std::ptrdiff_t kTmView = 0x00;
inline constexpr std::ptrdiff_t kTmWorld = 0x40; // inverse of view = camera world
inline constexpr std::ptrdiff_t kTmProj = 0xc0; // including jitter

// TT2 player camera object (FUN_1403afc80's `this`) and TTSpringCameraMode
inline constexpr std::ptrdiff_t kTTCameraGraphicsCamera = 0x10; // Graphics::Camera* (Game::Camera, as KTUCamera)
inline constexpr std::ptrdiff_t kTTCameraPlayer = 0x38; // player/vehicle component
inline constexpr std::ptrdiff_t kTTCameraOwnMeshes = 0x148; // RenderEntity*[3]: Pilot_Suit, Pilot_Helmet, Helmet_01
inline constexpr std::ptrdiff_t kTTCameraShadowDecals = 0x170; // DecalComponent*[2], hidden per index in first person
inline constexpr std::ptrdiff_t kTTCameraHelmetActor = 0x160; // HelmetView actor
inline constexpr std::ptrdiff_t kPlayerKind = 0x790; // int, 0 = local player (FUN_1403aec70 gate)
inline constexpr std::ptrdiff_t kPlayerCameraIndex = 0x794; // ushort, the player's camera index
// RenderEntity / DecalComponent / BoundedVolume (per-camera-index visibility, docs/re/frame-anatomy.md 5.2)
inline constexpr std::ptrdiff_t kRenderEntityFlags = 0x74; // ushort[2] render flags per camera index
inline constexpr std::ptrdiff_t kDecalVolume = 0xb0; // BoundedVolume*
inline constexpr std::ptrdiff_t kVolumeOwner = 0x58; // -> +0x48 visibility byte array
inline constexpr std::ptrdiff_t kVolumeOwnerVisibility = 0x48;
inline constexpr std::ptrdiff_t kVolumeIndex = 0x64; // int, this volume's byte; bit i = visible in camera index i
// SkinMesh: main-pass motion bookkeeping per camera index (SkinMesh::UpdateRenderData 0x140906b90, renderer.md 4)
inline constexpr std::ptrdiff_t kSkinMeshLastFrame = 0x158; // uint32[2], frameId of the index's last main pass
inline constexpr std::ptrdiff_t kSkinMeshLastLod = 0x160; // uint16[2], LOD of that pass
inline constexpr std::ptrdiff_t kModeCameraMatrix = 0x8; // Matrix34 the mode outputs
inline constexpr std::ptrdiff_t kModeHelmetMatrix = 0x4c4; // Matrix34 from HelmetSprings (built from +0x8)
inline constexpr std::ptrdiff_t kModePreLookMatrix = 0x3e4; // Matrix34: +0x8 before LookAroundInput (auto + stick look)

// KTUCamera / KTUCameraMode (game side; read only, never written)
inline constexpr std::ptrdiff_t kKTUCameraCamera = 0x10; // Graphics::Camera* it drives (SetFov/SetClips target)
inline constexpr std::ptrdiff_t kKTUCameraMode = 0x48; // active KTUCameraMode*; KTUCamera::Update calls its Manage
inline constexpr std::ptrdiff_t kKTUCameraOverrideMode = 0x68; // Ansel / free camera mode; non-null replaces +0x48
inline constexpr int kModeManageSlot = 6; // vtable slot (+0x30): Manage, called at 0x1409d9125

// PlayAll::Game::Renderer
inline constexpr std::ptrdiff_t kRendererSplitMode = 0xe40; // cameraDesc.splitMode (0 = one view)
inline constexpr std::ptrdiff_t kRendererSplitCameras = 0xe48; // Camera*[2]
inline constexpr std::ptrdiff_t kRendererCameraMask = 0xe58; // single-camera index = ctz(mask)
// CameraDesc {split mode, Camera*[2], mask} at +0xe40..+0xe58 (Renderer::SetCameraDesc copies it; DoUpdate derives
// +0xc90 / +0xd18 from it every frame). docs/re/frame-anatomy.md 5.1.
inline constexpr std::ptrdiff_t kRendererViewContexts = 0x30; // ViewContext[2], stride kViewContextSize
inline constexpr std::ptrdiff_t kViewContextSize = 0x620;
inline constexpr std::ptrdiff_t kRendererRenderLoop = 0xd30; // active RenderLoop*
inline constexpr std::ptrdiff_t kRendererScenes = 0xd80; // Scene*[2], per view (lighting, fog)
// GPU frame timer: ring of 4 slots of ID3D11Query* {TIMESTAMP start, TIMESTAMP end, TIMESTAMP_DISJOINT}; slot =
// frame counter & 3 (OnRender 0x1405af5e3: Begin(disjoint), End(start); OnFlushRender: End(end), End(disjoint)).
inline constexpr std::ptrdiff_t kRendererGpuTimerSlots = 0x1cd * 8;
inline constexpr std::ptrdiff_t kRendererGpuTimerSlotSize = 3 * 8;
inline constexpr std::ptrdiff_t kRendererGpuTimerFrame = 0xec8; // uint32, +1 per rendered OnFlip
inline constexpr std::ptrdiff_t kRenderLoopDeferredLighting = 0x1e78; // DeferredLighting* (GetDeferredLighting)
inline constexpr std::ptrdiff_t kRenderLoopMainContext = 0x1f00; // RenderContext re-prepared for every view
inline constexpr std::ptrdiff_t kRenderLoopTaaReset = 0xd08; // byte, TAA history reset (shared; cleared per RenderTAA)
inline constexpr std::ptrdiff_t kRenderLoopTaaHistory = 0xce8; // RenderTarget* pair per slot (+idx*0x10); slot 0 at +0
inline constexpr int kRenderTargetTextureSlot = 8; // vtable +0x40: BaseTexture* (int index)
inline constexpr std::ptrdiff_t kTexture2DDeviceTexture = 0x68; // Texture2DDX11: ID3D11Texture2D* (GetDeviceTexture)
inline constexpr std::ptrdiff_t kRenderContextCameraIndex = 0x594; // ushort (= its ViewContext+0x464)
// CascadedShadowContext (built at the end of CascadedShadowMapRenderer::Render, copied whole into RenderContext+0x5d0
// by SetCascadedShadowContext through FUN_1404a10b0): atlas texture, cascade crop matrices, splits, static map.
inline constexpr std::size_t kCascadedShadowContextSize = 0x228;
// PlayAll::Array<T*>: count at +4, element pointer at +8 (CSM::SetupRenderContext's view array).
inline constexpr std::ptrdiff_t kArrayCount = 0x4;
inline constexpr std::ptrdiff_t kArrayData = 0x8;
// DeferredLighting, per camera index (set by FUN_1403aec70 on view changes)
inline constexpr std::ptrdiff_t kLightingFrontPassSsr = 0x1e; // byte[2], 0 in first person
inline constexpr std::ptrdiff_t kLightingProbeFactor = 0x34; // float[2], 0.35 in first person, else 1.0

// Tonemap object (*(RenderLoop+0x1ed0), FUN_140914680 / FUN_1409120b0 / FUN_1409131d0)
inline constexpr std::ptrdiff_t kTonemapOutput = 0x130; // RT* the luminance chain writes and HDR_FinalCompose reads
inline constexpr std::ptrdiff_t kTonemapPrevious = 0x138; // RT*[2] previous luminance per camera index
inline constexpr std::ptrdiff_t kTonemapLuminanceGate = 0x150; // byte, 1 = run HDR_Luminance* (FUN_1409120b0)

// RenderLayer (RenderLayerDX11 instance): vtable slot 14 (+0x70) = constant buffer data allocation
// (layer, flags, buffer, -1, 0) -> CPU memory, uploaded when the recorded pass executes.
inline constexpr int kRenderLayerAllocateCbSlot = 14;
inline constexpr std::ptrdiff_t kRenderLayerRenderId = 0x30; // uint32, +1 per BeginRender (the fill jobs' frameId)

// DeferredRenderLoop
inline constexpr std::ptrdiff_t kDeferredLoopSplitBorder = 0x1e71; // byte, 1 = draw 'Split border' (0x140849344)
// Engine.Renderer.Split (Lua, RenderLoop::Create 0x1408da8d5): with >= 2 views Render caps every view's CSM cascade
// count (ViewContext+0x4dc) to this (unsigned min, 0x140846ab0). Then FXAA / SSAO split bytes (readers not found).
inline constexpr std::ptrdiff_t kRenderLoopSplitCascadeCount = 0x1e88;
inline constexpr std::ptrdiff_t kRenderLoopSplitFxaa = 0x1e8c;
inline constexpr std::ptrdiff_t kRenderLoopSplitSsao = 0x1e8d;
// Core::CVar<bool>: per-camera-index value bytes at +0x80 / +0x81 (slot 0 is what the static getters read).
inline constexpr std::ptrdiff_t kCVarBoolValue = 0x80;
// Core::CVar<int/uint>: per-camera-index values +0x80 / +0x84, base value +0x8c, min / max pointers +0xb0 / +0xb8
// (CheckAndSetValue FUN_140560bf0); vtable slot 12 = per-slot update (cvar, index, notify) FUN_1405aadb0.
inline constexpr std::ptrdiff_t kCVarIntSlots = 0x80;
inline constexpr std::ptrdiff_t kCVarIntBase = 0x8c;
inline constexpr std::ptrdiff_t kCVarIntMin = 0xb0;
inline constexpr std::ptrdiff_t kCVarIntMax = 0xb8;
inline constexpr int kCVarUpdateSlot = 12; // also the CVar<bool> per-slot update (same template layout)

// RenderTargetManager
inline constexpr std::ptrdiff_t kRtmPossibleSplitMode = 0x8;
inline constexpr std::ptrdiff_t kRtmSplitMode = 0xc;

// EffectorMapManager
inline constexpr std::ptrdiff_t kEffectorSlots = 0x8; // per-view effector texture objects [2]
inline constexpr std::ptrdiff_t kEffectorSlotCenter = 0x80; // Vector3 world centre (SetCenterWorldPos)

template <typename T>
T Read(const void* base, std::ptrdiff_t offset)
{
    T value;
    std::memcpy(&value, static_cast<const char*>(base) + offset, sizeof(T));
    return value;
}

template <typename T>
void Write(void* base, std::ptrdiff_t offset, const T& value)
{
    std::memcpy(static_cast<char*>(base) + offset, &value, sizeof(T));
}

} // namespace game::layout
