// Engine signatures (CLAUDE.md hard rule 3: no hard-coded addresses).
// Resolution order per entry: the mangled export name in TT2.exe's export table (PlayAll engine code is
// exported), then the IDA-style pattern in .text as a fallback. When both resolve, the pattern is also checked
// at the export address so a drifting pattern shows up in the log before it is ever needed.
// Each entry records our name, export, pattern, how it was found and the exe SHA-256 it was validated on.
#pragma once

#include <cstddef>
#include <iterator>

namespace game {

struct Signature {
    const char* name; // our name, as in docs/re/symbols.md
    const char* exportName; // mangled TT2.exe export, or nullptr
    const char* pattern; // IDA style, see core::ParsePattern; nullptr = export only
    std::ptrdiff_t offset; // added to the pattern match address (e.g. to land on a rel32)
    const char* howFound; // short evidence note
    const char* validatedOn; // exe SHA-256
};

enum class Sig {
    RendererDoUpdate,
    ViewContextInitFromCamera,
    MatrixPerspectiveFovRH,
    TransformMatrixSetViewProjection,
    ViewContextRebuildFrustum,
    TTCameraAfterPhysicsStep,
    ActorSetWorldMatrix,
    GuiManagerInstance,
    GuiManagerIsMenuOpen,
    GuiManagerIsGamePaused,
    GameEngineInstance,
    GameEngineGetCamera,
    SpringCameraModeManage,
    WindowClientSize,
    GetVideoDevice,
    SetDeviceResetNeeded,
    VideoDeviceDX11WindowsVftable,
    GameContextInstance,
    GameContextGetTimeFactor,
    RenderTargetManagerInstance,
    RenderTargetManagerSetPossibleSplitMode,
    EffectorMapManagerInstance,
    EffectorMapManagerSetCenterWorldPos,
    DeferredRenderLoopVftable,
    VideoDeviceUpdateWindowResolution,
    StaticShadowEnabledInSplit,
    CascadeShadowMapSize,
    StaticShadowMapSize,
    GameEngineConfigureRenderer,
    RendererUpdateShadowQuality,
    CsmCreateStaticShadowMap,
    IntelSsaoUpdateConstants,
    RenderLayerInstance,
    SsaoEnable,
    MotionBlurEnable,
    RenderContextPrepareView,
    RenderLoopPrepareForCulling,
    RenderLoopRestoreAfterCulling,
    DeferredRenderLoopRender,
    TonemapRender,
    RenderLoopRenderTaa,
    SkinMeshUpdateRenderData,
    RenderLayerAddProfilingEvent,
    CsmSetupRenderContext,
    CsmRender,
    RenderContextSetCascadedShadowContext,
    HHParticlesManagerEnableMedium,
    ResolutionScaling,
    DynamicResolutionEnabled,
    RenderLoopPostUpdateLastFrameCamera,
    HudRendererRender,
    HudNodeGetTargetInfo,
    RenderTargetManagerCreateRenderTarget,
    WaterDropRender,
    PostProcessIsEnabled,
    PostProcessEnable,
    PostProcessDisable,
    MaterialGetFloat,
    MaterialGetVector,
    MaterialSetFloat,
    MaterialSetVector,
    StringIdFromChars,
    OptionsCreate,
    OptionsSwitchTab,
    OptionsRowTypes,
    OptionsFillRows,
    OptionsListChange,
    OptionsGaugeValidate,
    OptionsOpenRow,
    LocaleGetString16,
    SettingsManagerInstance,
    SettingBoolVftable,
    SettingFloatVftable,
    MemoryAllocAlignedInfo,
    MemoryFreeAlignedInfo,
    RenderLayerDrawCommandBuffer,
    RenderLayerDispatch,
    RenderLayerCopyTextureSlice,
    RenderLoopRenderFXAA,
    PostProcessGetIndex,
    InputIsXInputDevice,
    RenderLoopIsUsingTAA,
    VisibilityCullerPrepareOccluder,
    VisibilityCullerAddCullingData,
    GenericInstanceSpawnerInstance,
    GenericInstanceSpawnerStartCulling,
    Count,
};

inline constexpr const char* kBuild4989319 = "103d4b2e4d86786a7eb1810faa280a41cec6cadce1e8865165c3de8ba87a601a";

// Order must match `Sig`. Patterns were checked unique in .text of build 4989319 (prologue bytes from Ghidra,
// rip-relative displacements and call targets wildcarded).
inline constexpr Signature kSignatures[] = {
    {"Renderer::DoUpdate", "?DoUpdate@Renderer@Game@PlayAll@@IEAAHPEAVGameContext@23@I@Z",
        "48 89 5C 24 18 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 81 EC 80 00 00 00 "
        "48 8B 99 D8 0D 00 00 4C 8B FA 48 8B F1",
        0, "export; 0x1405addd0, builds the ViewContexts from Renderer+0xe48/+0xe50 (docs/re/camera.md)",
        kBuild4989319},
    {"ViewContext::InitFromCamera",
        "?InitFromCamera@ViewContext@Graphics@PlayAll@@QEAAXPEBVCamera@23@AEBUViewport@23@AEBVVector2@Math@3@@Z",
        "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 48 8D 6C 24 A9 48 81 EC F0 00 00 00 48 89 91 50 04 00 00 "
        "48 8B D9 8B 42 7C",
        0, "export; 0x14091aeb0, camera world (pool) -> view, MatrixPerspectiveFovRH + jitter -> proj", kBuild4989319},
    {"Math::MatrixPerspectiveFovRH", "?MatrixPerspectiveFovRH@Math@PlayAll@@YAPEAVMatrix44@12@PEAV312@MMMM@Z",
        "40 53 48 83 EC 40 F3 0F 59 0D ?? ?? ?? ?? 48 8B D9 0F 29 74 24 30 0F 28 F2 0F 29 7C 24 20 0F 28 FB "
        "0F 28 C1 E8 ?? ?? ?? ?? F3 0F 10 2D ?? ?? ?? ?? 33 C0 0F 28 CE 48 89 43 0C F3 0F 5C CF",
        0, "export; 0x140813390, 20 callers; differs from MatrixPerspectiveFovLH 0x1408132f0 at +0x33", kBuild4989319},
    {"TransformMatrix::SetViewProjection",
        "?SetViewProjection@TransformMatrix@Graphics@PlayAll@@QEAAXAEBVMatrix44@Math@3@0@Z",
        "48 89 5C 24 08 57 48 83 EC 20 49 8B F8 48 8B D9 E8 ?? ?? ?? ?? 48 8B D7 48 8B CB 48 8B 5C 24 30 "
        "48 83 C4 20 5F E9",
        0, "export; 0x140915260, RecomputeView(view) then SetProjection(proj)", kBuild4989319},
    {"ViewContext::RebuildFrustum", "?RebuildFrustum@ViewContext@Graphics@PlayAll@@QEAAXXZ",
        "40 53 48 83 EC 40 F3 0F 10 81 40 04 00 00 48 8B D9 F3 0F 10 89 44 04 00 00 BA 04 00 00 00", 0,
        "export; 0x14091b4e0, frustum from ViewProj (slot 0) + clip bounds", kBuild4989319},
    {"TTCamera::AfterPhysicsStep", nullptr,
        "40 53 48 83 EC 30 48 8B D9 FF 15 ?? ?? ?? ?? 85 C0 75 ?? 48 83 7B 38 00 74 ?? 48 8D 4C 24 20 "
        "FF 15 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 44 24 20 48 8D 4C 24 20 8B 83 38 01 00 00 89 44 24 28 E8",
        0, "TT2 code, not exported; 0x1403afc80 (vtable slot 0x140f60b00), places the HelmetView actor at mode+0x4c4",
        kBuild4989319},
    {"Actor::SetWorldMatrix", "?SetWorldMatrix@Actor@GameServices@PlayAll@@QEAAXAEBVMatrix34@Math@3@@Z", nullptr, 0,
        "export; 0x1403d77f0, mov rcx,[rcx+0x70]; jmp GraphNode::SetWorldMatrix (too short for a pattern)",
        kBuild4989319},
    {"g_pGuiManager", "?g_pGuiManager@@3PEAVKTUGuiManager@@EA", nullptr, 0,
        "data export; 0x1414153c8, KTUGuiManager* (docs/re/frame-anatomy.md 8.4)", kBuild4989319},
    {"KTUGuiManager::IsMenuOpen", "?IsMenuOpen@KTUGuiManager@@QEBA_NXZ", nullptr, 0,
        "export; 0x14043db20, cmp qword [rcx+0xf0],0; setne al (current menu != null; too short for a pattern)",
        kBuild4989319},
    {"KTUGuiManager::IsGamePaused", "?IsGamePaused@KTUGuiManager@@QEBA_NXZ", nullptr, 0,
        "export; 0x14043b420, movzx eax, byte [rcx+0xe8] (set by PauseGame; too short for a pattern)", kBuild4989319},
    {"g_pGameEngine", "?g_pGameEngine@@3PEAVKTUGameEngine@@EA", nullptr, 0,
        "data export; 0x1414150b0, KTUGameEngine* (read by KTUCamera::Update at 0x1409d906d)", kBuild4989319},
    {"KTUGameEngine::GetCamera", "?GetCamera@KTUGameEngine@@UEAAPEAVKTUCamera@@H@Z", nullptr, 0,
        "export; 0x140651200, player i (count +0x3b4) -> GetPlayerStruct(i)+0x18 = KTUCamera* (too short for a pattern)",
        kBuild4989319},
    {"TTSpringCameraMode::Manage", nullptr,
        "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B D9 48 8B F2 48 8B 89 38 01 00 00 0F B6 93 40 01 00 00",
        0, "TT2 code, not exported; 0x14019d700 = slot 6 (+0x30) of the TTSpringCameraMode vftable 0x140f338f8; "
           "first 20 bytes already unique in .text",
        kBuild4989319},
    {"VideoDevice client size (FUN_140959f80)", nullptr,
        "40 56 57 41 56 48 81 EC 80 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 70 4D 8B F0 48 8B F2 48 8B F9 "
        "FF 15 ?? ?? ?? ?? 48 8B CF 85 C0 74 ?? BA F0 FF FF FF",
        0, "not exported; 0x140959f80 (HWND, int* w, int* h): IsIconic ? placement rect : GetClientRect; only callers "
           "DeviceReset(WindowWin32*) 0x140958c7e and UpdateWindowResolution 0x14095c2cd",
        kBuild4989319},
    {"Graphics::GetVideoDevice", "?GetVideoDevice@Graphics@PlayAll@@YAPEAVVideoDevice@12@XZ", nullptr, 0,
        "export; 0x140919d90, the VideoDeviceDX11Windows instance", kBuild4989319},
    {"VideoDeviceDX11Windows::SetDeviceResetNeeded",
        "?SetDeviceResetNeeded@VideoDeviceDX11Windows@Graphics@PlayAll@@QEAAXPEAVWindowWin32@23@@Z", nullptr, 0,
        "export; 0x14095b6e0, nullptr window -> this+0x4d8 = 1 (PostFlip then runs DeviceReset)", kBuild4989319},
    {"VideoDeviceDX11Windows vftable", "??_7VideoDeviceDX11Windows@Graphics@PlayAll@@6B@", nullptr, 0,
        "data export; 0x1410328f0, checked against *GetVideoDevice() before calling SetDeviceResetNeeded",
        kBuild4989319},
    {"GameContext::s_pInstance", "?s_pInstance@GameContext@Game@PlayAll@@1PEAV123@EA", nullptr, 0,
        "data export; 0x14139a590, GameContext*", kBuild4989319},
    {"GameContext::GetTimeFactor", "?GetTimeFactor@GameContext@Game@PlayAll@@QEBAMXZ", nullptr, 0,
        "export; 0x14012dbc0 (ICF-shared), movss xmm0,[rcx+0x48]: the factor PushTimeFactor sets; "
        "TTMenuManager::PauseGame 0x140246b70 pushes 0.0 offline",
        kBuild4989319},
    {"RenderTargetManager::s_pSingletonInstance", "?s_pSingletonInstance@RenderTargetManager@Graphics@PlayAll@@1PEAV123@EA",
        nullptr, 0, "data export; 0x141408500, read directly by Renderer::DoUpdate 0x1405adf53 (frame-anatomy.md 5.1)",
        kBuild4989319},
    {"RenderTargetManager::SetPossibleSplitMode",
        "?SetPossibleSplitMode@RenderTargetManager@Graphics@PlayAll@@QEAAXW4SplitMode@23@@Z", nullptr, 0,
        "export; 0x1408e2080, +8 = mode and RecreateSplitRenderTargetForAllWindows if it changed (13 bytes + jmp)",
        kBuild4989319},
    {"EffectorMapManager::s_pSingletonInstance", "?s_pSingletonInstance@EffectorMapManager@Graphics@PlayAll@@1PEAV123@EA",
        nullptr, 0, "data export; 0x1413ccd28, vegetation bend maps, slot textures at +8 / +0x10", kBuild4989319},
    {"EffectorMapManager::SetCenterWorldPos",
        "?SetCenterWorldPos@EffectorMapManager@Graphics@PlayAll@@QEAAXAEBVVector3@Math@3@G@Z", nullptr, 0,
        "export; 0x14084e650, slot = [this+8+idx*8]; if non-null writes the Vector3 to slot+0x80 (too short for a "
        "pattern)",
        kBuild4989319},
    {"DeferredRenderLoop vftable", "??_7DeferredRenderLoop@Graphics@PlayAll@@6B@", nullptr, 0,
        "data export; 0x140f7bfc8, checked against Renderer+0xd30 before touching DeferredRenderLoop fields",
        kBuild4989319},
    {"VideoDeviceDX11Windows::UpdateWindowResolution",
        "?UpdateWindowResolution@VideoDeviceDX11Windows@Graphics@PlayAll@@QEAAXPEAUHWND__@@@Z", nullptr, 0,
        "export; 0x14095c270, WM_SIZE in Windowed mode (RenderWindowWin32::DefaultWndProc 0x140709899): reads the "
        "client size (FUN_140959f80) and SetSize 0x14095bc20 -> SetWindowPos to that size",
        kBuild4989319},
    {"CascadedShadowMapRenderer::s_bStaticShadowMapEnabledInSplit",
        "?s_bStaticShadowMapEnabledInSplit@CascadedShadowMapRenderer@Graphics@PlayAll@@1V?$CVar@_N@Core@3@A", nullptr, 0,
        "data export; 0x1413ca8e0, CVar<bool> Engine.Renderer.StaticShadow.EnabledInSplit (ctor FUN_14008a690); value "
        "byte +0x80 read by IsStaticShadowMapEnabledInSplit 0x1408261f0, DoUpdate 0x1405ae4fc in split mode",
        kBuild4989319},
    {"Renderer::s_iCascadeShadowMapSize", "?s_iCascadeShadowMapSize@Renderer@Game@PlayAll@@2V?$CVar@H@Core@3@A", nullptr,
        0,
        "data export; 0x1413a7a20, CVar<int> Engine.Renderer.Shadow.CascadeShadowMapSize (ctor FUN_14007ab40), read "
        "when the renderer creates the CSM ('ShadowMap size: N*3'); set like PlatformSettings 0x140561d90",
        kBuild4989319},
    {"CascadedShadowMapRenderer::s_iStaticShadowMapSize",
        "?s_iStaticShadowMapSize@CascadedShadowMapRenderer@Graphics@PlayAll@@1V?$CVar@I@Core@3@A", nullptr, 0,
        "data export; 0x1413ca990, CVar<uint> Engine.Renderer.StaticShadow.Size (ctor FUN_14008ade0), slot 0 read "
        "by CreateStaticShadowMap 0x140825c20 (halved per view in split)",
        kBuild4989319},
    {"GameEngine::ConfigureRenderer",
        "?ConfigureRenderer@GameEngine@Game@PlayAll@@UEBAHPEAURendererDesc@23@AEBUGraphicsDescWIN@Graphics@3@@Z", nullptr,
        0, "export; 0x1404a9e30, startup: reads CascadeShadowMapSize slot 0 (0x1404a9f1b) into the renderer desc",
        kBuild4989319},
    {"Renderer::UpdateShadowQuality", "?UpdateShadowQuality@Renderer@Game@PlayAll@@QEAAHXZ", nullptr, 0,
        "export; 0x1405b1780, shadow quality change: reads CascadeShadowMapSize slot 0 (0x1405b1803), "
        "SetupCascadedShadowMap",
        kBuild4989319},
    {"CascadedShadowMapRenderer::CreateStaticShadowMap", "?CreateStaticShadowMap@CascadedShadowMapRenderer@Graphics@PlayAll@@QEAAHXZ",
        nullptr, 0, "export; 0x140825c20, reads StaticShadow.Size slot 0 (DAT_1413caa10), / 2 per view in split",
        kBuild4989319},
    {"IntelSSAO::UpdateConstants", "?UpdateConstants@IntelSSAO@Graphics@PlayAll@@AEAAXHAEBVMatrix44@Math@3@@Z", nullptr,
        0,
        "export; 0x140927e20 (this, pass, const Matrix44& proj): g_ASSAOConsts from 1/m00, 1/m11 only (symmetric), "
        "written into RenderLayer vtbl+0x70 memory",
        kBuild4989319},
    {"RenderLayer::s_pInstance", "?s_pInstance@RenderLayer@Graphics@PlayAll@@2PEAV123@EA", nullptr, 0,
        "data export; 0x141407290, RenderLayerDX11*; its vtable slot +0x70 allocates constant buffer data",
        kBuild4989319},
    {"IntelSSAO::s_bSSAOEnable", "?s_bSSAOEnable@IntelSSAO@Graphics@PlayAll@@2V?$CVar@_N@Core@3@A", nullptr, 0,
        "data export; 0x14140c730, CVar<bool> Engine.Renderer.SSAO.Enable; value bytes +0x80/+0x81 per camera index "
        "checked per view in DeferredRenderLoop::Render (0x140847c91)",
        kBuild4989319},
    {"RenderLoop::s_bMotionBlurEnable", "?s_bMotionBlurEnable@RenderLoop@Graphics@PlayAll@@2V?$CVar@_N@Core@3@A", nullptr,
        0,
        "data export; 0x1414077b0, CVar<bool> Engine.Renderer.MotionBlur.Enable; per-slot bytes 0x141407830[idx] gate "
        "RenderMotionBlur and (with TAA off) the MV pass (docs/re/temporal.md P6)",
        kBuild4989319},
    {"RenderContext::Prepare(const ViewContext&)", "?Prepare@RenderContext@Graphics@PlayAll@@QEAAXAEBVViewContext@23@@Z",
        nullptr, 0,
        "export; 0x1408cfad0, 12 bytes (add rcx,0x130; jmp ViewContext::operator=); per view on "
        "DeferredRenderLoop+0x1f00 at the start of each view's chain",
        kBuild4989319},
    {"RenderLoop::PrepareForCulling", "?PrepareForCulling@RenderLoop@Graphics@PlayAll@@QEAAXAEAVViewContext@23@@Z",
        nullptr, 0, "export; 0x1408dc850, widened projection for culling (front pass on), per view",
        kBuild4989319},
    {"RenderLoop::RestoreAfterCulling", "?RestoreAfterCulling@RenderLoop@Graphics@PlayAll@@QEAAXAEAVViewContext@23@@Z",
        nullptr, 0, "export; 0x1408ded00, re-installs the view projection after culling (MatrixPerspectiveFovRH)",
        kBuild4989319},
    {"DeferredRenderLoop::Render", "?Render@DeferredRenderLoop@Graphics@PlayAll@@UEAAXAEAURenderDesc@RenderLoop@23@@Z",
        nullptr, 0, "export; 0x1408469c0, all views in one call (scene phase, then the per-view loop)",
        kBuild4989319},
    {"Tonemap (FUN_140914680)", nullptr,
        "4C 8B DC 55 53 56 57 41 56 41 57 49 8D AB C8 FA FF FF 48 81 EC 08 06 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 "
        "48 89 85 70 04 00 00 44 0F B7 BD 60 05 00 00 48 8B F1",
        0, "not exported; 0x140914680 (tm, sceneHdr, dst, ctx, u16 camIdx, vc), per view from "
           "DeferredRenderLoop::Render 0x140848768; prologue unique in .text",
        kBuild4989319},
    {"RenderLoop::RenderTAA", "?RenderTAA@RenderLoop@Graphics@PlayAll@@IEAAXAEAVRenderContext@23@@Z", nullptr, 0,
        "export; 0x1408ddf70, per view at record time (call 0x140848c03); clears the reset flag RenderLoop+0xd08",
        kBuild4989319},
    {"SkinMesh::UpdateRenderData", "?UpdateRenderData@SkinMesh@Graphics@PlayAll@@UEAA_NAEBVViewContext@23@IH@Z",
        "48 89 5C 24 18 44 89 4C 24 20 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 30 "
        "44 0F B7 AA 64 04 00 00 45 8B E0 48 8B 79 68 48 8B F2",
        0, "export; 0x140906b90 (mesh, vc, frameId, dbIdx), per visible view from the fill job FUN_1408d3370; "
           "pattern unique in .text (docs/re/renderer.md 4)",
        kBuild4989319},
    {"RenderLayer::AddProfilingEvent",
        "?AddProfilingEvent@RenderLayer@Graphics@PlayAll@@IEAAXPEAUSimpleGPUPerformanceQuery@23@PEBD@Z",
        "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 80 B9 68 02 00 00 00 49 8B F0 4C 63 4A 0C 48 8B FA 48 8B D9 74",
        0, "export; 0x1408d3aa0 (layer, query, name), queues a kind-5 pass: End(query) + Begin/EndEvent when its job "
           "executes (Begin/EndProfilingEvent are jmp thunks to it); pattern unique in .text (docs/re/renderer.md 7)",
        kBuild4989319},
    {"CascadedShadowMapRenderer::SetupRenderContext",
        "?SetupRenderContext@CascadedShadowMapRenderer@Graphics@PlayAll@@QEAAXAEBV?$Array@PEAUCameraRenderDesc@Graphics@"
        "PlayAll@@$0A@$08@3@PEAULightRenderInstanceCollection@23@AEBUShiftInfo@23@@Z",
        "4C 8B DC 55 56 41 56 49 8D AB D8 FB FF FF 48 81 EC 10 05 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 40 03 00 "
        "00 80 B9 C2 25 00 00 00 4C 8B F2",
        0, "export; 0x140828320 (csm, views, lights, shift), once per scene from DeferredLighting::SetupLights "
           "(call 0x14083d427): fits each camera index's cascades from that view; pattern unique (renderer.md 3)",
        kBuild4989319},
    {"CascadedShadowMapRenderer::Render",
        "?Render@CascadedShadowMapRenderer@Graphics@PlayAll@@QEAAXAEAVRenderContext@23@AEBUCascadedShadowRenderDesc@23@@Z",
        "40 55 56 41 55 41 56 48 8D AC 24 48 F5 FF FF 48 81 EC B8 0B 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 80 0A "
        "00 00 41 83 78 6C 00 4D 8B F0",
        0, "export; 0x140826230 (csm, rc, desc), per view (only caller DeferredLighting::RenderDirectionalCascadedShadow"
           "Map 0x14083a7df): atlas clear, static map, cascades, SetCascadedShadowContext; pattern unique",
        kBuild4989319},
    {"RenderContext::SetCascadedShadowContext",
        "?SetCascadedShadowContext@RenderContext@Graphics@PlayAll@@QEAAXAEBUCascadedShadowContext@23@@Z",
        "48 89 5C 24 08 57 48 83 EC 40 48 8B D9 48 8B FA 48 81 C1 D0 05 00 00 E8 ?? ?? ?? ?? 48 8B 4B 10",
        0, "export; 0x1408cfb00 (rc, ctx): ctx (0x228 bytes) -> rc+0x5d0, builds cbPSCascadedShadows; pattern unique",
        kBuild4989319},
    {"HHParticlesManager::EnableMedium", "?EnableMedium@HHParticlesManager@Graphics@PlayAll@@QEAAXH_N@Z", nullptr, 0,
        "export; 0x1409a3100, mgr[0x2c+medium] = enable (9 bytes, too short for a pattern); only caller "
        "RenderOneFrame: medium 2 = GameEngine+0x184 each frame (docs/re/renderer.md 6 A)",
        kBuild4989319},
    {"DrawPass::s_vResolutionScaling", "?s_vResolutionScaling@DrawPass@Graphics@PlayAll@@2VVector2@Math@3@A", nullptr, 0,
        "data export; 0x1413602d8, Vector2 dynamic-resolution scale; written only by the controller FUN_1405b1270 "
        "(Renderer::OnRender, before the render loop); mode 0 = 1.0 / 1.0 (docs/re/temporal.md 10.3)",
        kBuild4989319},
    {"DrawPass::s_bDynamicResolutionEnabled", "?s_bDynamicResolutionEnabled@DrawPass@Graphics@PlayAll@@2_NA", nullptr, 0,
        "data export; 0x1414082ea, bool; same writer; gates UpscaleDynamicResolution and GetDrawPassViewport",
        kBuild4989319},
    {"RenderLoop::PostUpdateLastFrameCamera",
        "?PostUpdateLastFrameCamera@RenderLoop@Graphics@PlayAll@@IEAAXAEBVRenderContext@23@@Z",
        nullptr, 0,
        "export; 0x1408dc2f0 (loop, rc): latches slot rc+0x594's previous VP / camera position / jitter (and the "
        "front-pass set) from the prepared RenderContext, unless s_bMotionPause (docs/re/temporal.md 10.1)",
        kBuild4989319},
    {"HudRenderer::Render",
        "?Render@HudRenderer@Graphics@PlayAll@@QEAAXPEAVRenderTarget@23@PEAVHudItemList@23@PEAVRenderLoop@23@AEAVRender"
        "Context@23@PEBUCameraRenderDesc@23@H_N@Z",
        nullptr, 0,
        "export; 0x14088b530 (hud, rt, list, loop, rc, cameraDesc, flags, bool); the global 2D list after the view loop "
        "at 0x14084953a (list RenderDesc+0xc50, flags 0x22) (docs/re/frame-anatomy.md 8.6)",
        kBuild4989319},
    {"HudNode::GetTargetInfo", "?GetTargetInfo@HudNode@Graphics@PlayAll@@SA?AUTargetInfo@123@XZ", nullptr, 0,
        "export; 0x140881ae0, static, struct returned through a hidden pointer: the 2D HUD layout for the current "
        "window (1920x1080 widened to its aspect) when HudRenderer+4/+8 are -1",
        kBuild4989319},
    {"RenderTargetManager::CreateRenderTarget",
        "?CreateRenderTarget@RenderTargetManager@Graphics@PlayAll@@QEAAHAEAV?$StrongPtr@VRenderTarget@Graphics@PlayAll@@"
        "@Core@3@AEBUDesc@RenderTarget@23@@Z",
        nullptr, 0,
        "export; 0x1408e1a30 (rtm, StrongPtr<RenderTarget>&, const Desc&) -> 0 on success; TextureBaker::Create makes "
        "its sRGB 'TextureBakeSRGB' target with it (docs/re/frame-anatomy.md 8.6)",
        kBuild4989319},
    {"WaterDrop::Render", "?Render@WaterDrop@Graphics@PlayAll@@QEAAXMPEAVBaseTexture@23@G@Z", nullptr, 0,
        "export; 0x14092a9c0 (wd, dt, last-frame colour, slot): simulates and draws one slot's visor drops, called "
        "per view at 0x1408486f6 (docs/re/renderer.md 6 B)",
        kBuild4989319},
    {"PostProcessManager::IsPostProcessEnabled", "?IsPostProcessEnabled@PostProcessManager@Graphics@PlayAll@@QEBA_NIG@Z",
        nullptr, 0, "export; 0x140451470 (ppm, index, slot): active byte +0x80+slot of the post process (renderer.md 6 D)",
        kBuild4989319},
    {"PostProcessManager::EnablePostProcess", "?EnablePostProcess@PostProcessManager@Graphics@PlayAll@@QEAAXIF@Z",
        nullptr, 0, "export; 0x1408bde30 (ppm, index, short slot; < 0 = both)", kBuild4989319},
    {"PostProcessManager::DisablePostProcess", "?DisablePostProcess@PostProcessManager@Graphics@PlayAll@@QEAAXIF@Z",
        nullptr, 0, "export; 0x1408bdca0 (ppm, index, short slot; < 0 = both)", kBuild4989319},
    {"MaterialDefinition::GetFloat", "?GetFloat@MaterialDefinition@Graphics@PlayAll@@QEBA_NVStringId@3@AEAM@Z", nullptr,
        0, "export; 0x14089e570 (material, StringId by value, float&)", kBuild4989319},
    {"MaterialDefinition::GetVector",
        "?GetVector@MaterialDefinition@Graphics@PlayAll@@QEBA_NVStringId@3@AEAVVector4@Math@3@@Z", nullptr, 0,
        "export; 0x14089e690 (material, StringId, Vector4&)", kBuild4989319},
    {"MaterialDefinition::SetFloat", "?SetFloat@MaterialDefinition@Graphics@PlayAll@@QEAA_NVStringId@3@M@Z", nullptr, 0,
        "export; 0x14089e720 (material, StringId, float)", kBuild4989319},
    {"MaterialDefinition::SetVector",
        "?SetVector@MaterialDefinition@Graphics@PlayAll@@QEAA_NVStringId@3@AEBVVector4@Math@3@@Z", nullptr, 0,
        "export; 0x14089e820 (material, StringId, const Vector4&)", kBuild4989319},
    {"StringId::StringId(const char*, StringType)", "??0StringId@PlayAll@@QEAA@PEBDW4StringType@01@@Z", nullptr, 0,
        "export; 0x1403cc610; TTCollisionEffect's Manage builds its material parameter ids with type 1 (\"fBlend\", 1)",
        kBuild4989319},
    // PLAN 8i, the VR tab in the game's Options (docs/re/options-menu.md). TT2 code, not exported; every pattern was
    // checked unique in .text and starts at the function entry.
    {"TTOptionsState::Create", nullptr,
        "48 89 5C 24 18 48 89 74 24 20 55 57 41 55 41 56 41 57 48 8D 6C 24 C9 48 81 EC C0 00 00 00 45 33 ED C7 44 24 20 "
        "49 00 00 00 48 8B F1 4C 89 6D BF",
        0, "0x140300e20, int(state), 0 = ok: loads OPTIONS.UIML, tab labels state+0x4c0, settings groups +0x654..+0x65c",
        kBuild4989319},
    {"TTOptionsState::SwitchTab", nullptr,
        "48 89 5C 24 20 48 89 54 24 10 55 56 57 41 54 41 57 48 8B EC 48 83 EC 70 48 8B 05 ?? ?? ?? ?? 4C 8B F9 49 63 D8 "
        "4C 8B E2 41 B0 01 48 8B FB 48 8B",
        0, "0x140307730, (state, SimpleStringEnum* page, int tab); settings-driven tabs read group state+0x650[tab]",
        kBuild4989319},
    {"TTOptionsState::SetRowTypes", nullptr,
        "40 56 41 55 41 57 48 83 EC 30 48 8B 05 ?? ?? ?? ?? 4C 8B E9 48 8B 08 49 63 85 98 05 00 00 41 8B 94 85 50 06 00 00",
        0, "0x140300590, void(state): row widget kind (list / slider) and title per setting of the current tab's group",
        kBuild4989319},
    {"TTOptionsState::FillRows", nullptr,
        "40 55 56 41 55 48 83 EC 60 48 8B 05 ?? ?? ?? ?? 4C 8B E9 48 8B 08 49 63 85 98 05 00 00 41 8B 94",
        0, "0x1403085e0, void(state): value text of every row of the current tab's group", kBuild4989319},
    {"TTOptionsState::OnListChange", nullptr,
        "48 89 5C 24 18 55 56 57 48 8B EC 48 83 EC 60 41 8B F0 48 8B DA 48 8B F9 FF 15 ?? ?? ?? ?? 83 BF 90 05 00 00 01",
        0, "0x140305970, (state, widget, uint index, ptr): list / bool change, SetSetting only for tabs 1..3",
        kBuild4989319},
    {"TTOptionsState::OnGaugeValidate", nullptr,
        "48 89 5C 24 10 48 89 7C 24 18 55 48 8B EC 48 83 EC 50 0F 29 74 24 40 48 8B FA 0F 28 F2 48 8B D9 FF 15 ?? ?? ?? ?? "
        "83 BB 90 05 00 00 01",
        0, "0x140305c40, (state, widget, float value 0..100, ptr): slider change, SetSetting only for tab 1",
        kBuild4989319},
    {"TTOptionsState::OpenRow", nullptr,
        "4C 89 44 24 18 48 89 4C 24 08 55 53 41 54 41 55 41 57 48 8D 6C 24 C9 48 81 EC 00 01 00 00 4D 8B E8 48 63 DA "
        "4C 8B E1 FF 15 ?? ?? ?? ??",
        0, "0x140305e10, (state, int row, widget): opens a row's list or gauge; the gauge takes the setting's +0x38 as "
           "its value, the CVar range as min / max and a step of 1",
        kBuild4989319},
    {"LocaleManager::GetString16(int)", "?GetString16@LocaleManager@Core@PlayAll@@QEBAPEB_WH@Z", nullptr, 0,
        "export; 0x1407547b0 (lm, id) -> text, L\"NOT FOUND\" for unknown ids; tab and row titles go through it",
        kBuild4989319},
    {"SettingsManager::s_pSingletonInstance", "?s_pSingletonInstance@SettingsManager@Core@PlayAll@@1PEAV123@EA", nullptr,
        0, "data export; 0x1413ad228: {+0 capacity, +4 count, +8 Settings*[]}", kBuild4989319},
    {"SettingBool vftable", "??_7SettingBool@Core@PlayAll@@6B@", nullptr, 0, "export; 0x140f76b18, 9 slots",
        kBuild4989319},
    {"SettingFloat vftable", "??_7SettingFloat@Core@PlayAll@@6B@", nullptr, 0, "export; 0x140f76ba8, 9 slots",
        kBuild4989319},
    {"Memory::AllocAlignedInfo", "?AllocAlignedInfo@Memory@PlayAll@@YAPEAX_KHIPEBDH@Z", nullptr, 0,
        "export; 0x14081b740 (size, align, tag, file, line); Array growth uses (n*8, 8, 9, array.inl, 0x76)",
        kBuild4989319},
    {"Memory::FreeAlignedInfo", "?FreeAlignedInfo@Memory@PlayAll@@YAXPEAX_N@Z", nullptr, 0,
        "export; 0x14081b830 (ptr, bool)", kBuild4989319},
    // PLAN Phase 9, game::PassProfiler: the three pass appenders that cost GPU time (docs/re/renderer.md 9).
    {"RenderLayer::DrawCommandBuffer", "?DrawCommandBuffer@RenderLayer@Graphics@PlayAll@@QEAAXPEAVDrawPass@23@@Z",
        nullptr, 0, "export; 0x1408d6a10 (rl, DrawPass*): kind 0, queue 0; name at pass+0x18", kBuild4989319},
    {"RenderLayer::Dispatch", "?Dispatch@RenderLayer@Graphics@PlayAll@@QEAAXPEAUDispatchCommand@23@@Z", nullptr, 0,
        "export; 0x1408d68d0 (rl, DispatchCommand*): kind 1, queue 0; name at cmd+0x3b8", kBuild4989319},
    {"RenderLayer::CopyTextureSlice",
        "?CopyTextureSlice@RenderLayer@Graphics@PlayAll@@QEAAXPEAUCopySubRessourcePass@23@@Z", nullptr, 0,
        "export; 0x1408d5db0 (rl, CopySubRessourcePass*): kind 4, queue 0; unnamed", kBuild4989319},
    // PLAN 9c, game::VrEffects
    {"RenderLoop::RenderFXAA",
        "?RenderFXAA@RenderLoop@Graphics@PlayAll@@IEAAXAEAVRenderContext@23@AEBUCameraRenderDescOption@23@@Z", nullptr, 0,
        "export; 0x1408dd260 (loop, rc, option): FXAA when RenderLoop+1000 exists and the AA mode is 1 or 2 "
        "(renderer.md 9)",
        kBuild4989319},
    {"PostProcessManager::GetPostProcessIndex",
        "?GetPostProcessIndex@PostProcessManager@Graphics@PlayAll@@QEBAHAEBVStringId@3@@Z", nullptr, 0,
        "export; 0x1408be260 (ppm, const StringId&) -> index or -1; Vignette::ApplyParams and the Ansel session use it "
        "for \"Vignette\"",
        kBuild4989319},
    // Test mode only (game::VirtualPad): the game makes an XInput slot only for a DirectInput device this accepts.
    {"InputDriverPC::IsXInputDevice", nullptr,
        "40 55 41 56 48 8D 6C 24 A8 48 81 EC 58 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 30 80 3D ?? ?? ?? ?? 01 "
        "4C 8B F1 75 07 32 C0",
        0, "TT2 code, not exported; 0x1409ce2d0 (const GUID* guidProduct) -> bool: WMI 'IG_' match on VID/PID; only "
           "caller InputDriverPC::EnumControllerCallback 0x1409cbffc",
        kBuild4989319},
    // PLAN 9c fix, game::VrEffects: FXAA may only be skipped when TAA runs after it (renderer.md 9).
    {"RenderLoop::IsUsingTAA", "?IsUsingTAA@RenderLoop@Graphics@PlayAll@@SA_NXZ", nullptr, 0,
        "export; 0x1404fd660 () -> AA mode 2 or 3; RenderTAA also needs RenderLoop+0x20 (float frame dt) != 0",
        kBuild4989319},
    // [xr] occlusion, game::VrEffects: the per-view occluder lists (terrain occlusion boxes; renderer.md 10).
    {"VisibilityCuller::PrepareOccluder", "?PrepareOccluder@VisibilityCuller@Graphics@PlayAll@@QEAAXPEAUVolumePool@23@@Z",
        nullptr, 0,
        "export; 0x14091d490 (culler, VolumePool*): after ClearOccluder, fills each view's occluder lists "
        "(+0xdc8 all, +0xbc8 GIS terrain); skipped = no occlusion culling, frustum culling unchanged",
        kBuild4989319},
    // [debug] flash_detect, game::CullProbe: what the culler gets per view.
    {"VisibilityCuller::AddCullingData",
        "?AddCullingData@VisibilityCuller@Graphics@PlayAll@@QEAAXPEBVViewContext@23@PEBVPlane@Math@3@1IPEAVRenderInstanceManager@23@@Z",
        nullptr, 0, "export; 0x14091c850 (culler, ViewContext*, Plane*, Plane*, flags, RenderInstanceManager*)",
        kBuild4989319},
    {"AbstractGenericInstanceSpawner::s_pInstance", "?s_pInstance@AbstractGenericInstanceSpawner@Graphics@PlayAll@@1PEAV123@EA",
        nullptr, 0, "data export; 0x1413c9de0, the GenericInstanceSpawner (trees, bushes; renderer.md 11)", kBuild4989319},
    {"GenericInstanceSpawner::StartCulling",
        "?StartCulling@GenericInstanceSpawner@Game@PlayAll@@UEAAXPEAVSceneManager@Graphics@3@PEBUCameraRenderDesc@53@AEBV?$Array@PEAUCameraRenderDesc@Graphics@PlayAll@@$0A@$08@3@PEAUWaitGroup@enki@@@Z",
        nullptr, 0, "export, vtbl +0x20; 0x14062f980 (spawner, scene, desc, const Array<CameraRenderDesc*>& views, WaitGroup*)",
        kBuild4989319},
};

static_assert(std::size(kSignatures) == static_cast<std::size_t>(Sig::Count), "kSignatures must match Sig");

// Resolves every signature (logs one line each) on the first call; later calls return the same count. Call after
// the exe version is known, before installing engine hooks. Returns the number of unresolved entries.
int ResolveSignatures();
// Address of a resolved signature, or nullptr.
void* SignatureAddress(Sig id);

} // namespace game
