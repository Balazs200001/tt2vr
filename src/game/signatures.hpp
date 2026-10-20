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
};

static_assert(std::size(kSignatures) == static_cast<std::size_t>(Sig::Count), "kSignatures must match Sig");

// Resolves every signature once (logs one line each). Call after the exe version is known, before installing
// engine hooks. Returns the number of unresolved entries.
int ResolveSignatures();
// Address of a resolved signature, or nullptr.
void* SignatureAddress(Sig id);

} // namespace game
