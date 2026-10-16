#include "game/split_view.hpp"

#include "core/config.hpp"
#include "core/log.hpp"
#include "game/engine_layout.hpp"
#include "game/signatures.hpp"

#include <spdlog/fmt/fmt.h>

#include <windows.h>

#include <bit>
#include <cstdint>

namespace game {
namespace {

using namespace layout;

using SetPossibleSplitModeFn = void (*)(void* rtm, int mode);
using SetCenterWorldPosFn = void (*)(void* effectorMaps, const float* position, unsigned short index);

void* const* g_rtm = nullptr; // &RenderTargetManager::s_pSingletonInstance
SetPossibleSplitModeFn g_setPossibleSplitMode = nullptr;
void* const* g_effectorMaps = nullptr; // &EffectorMapManager::s_pSingletonInstance
SetCenterWorldPosFn g_setCenterWorldPos = nullptr;
const void* g_deferredVftable = nullptr;
bool g_available = false;

// Renderer CameraDesc fields (the qword halves next to mode and mask are left alone).
struct Desc {
    int mode = 0;
    const void* camera[2] = {};
    std::uint32_t mask = 0;
    bool operator==(const Desc&) const = default;
};

Desc ReadDesc(const void* renderer)
{
    Desc d;
    d.mode = Read<int>(renderer, kRendererSplitMode);
    d.camera[0] = Read<const void*>(renderer, kRendererSplitCameras);
    d.camera[1] = Read<const void*>(renderer, kRendererSplitCameras + 8);
    d.mask = Read<std::uint32_t>(renderer, kRendererCameraMask);
    return d;
}

void WriteDesc(void* renderer, const Desc& d)
{
    Write(renderer, kRendererSplitMode, d.mode);
    Write(renderer, kRendererSplitCameras, d.camera[0]);
    Write(renderer, kRendererSplitCameras + 8, d.camera[1]);
    Write(renderer, kRendererCameraMask, d.mask);
}

// Main thread state.
bool g_haveGame = false;
Desc g_game; // what the game wants
bool g_writing = false; // our two-view desc is in the Renderer
Desc g_written;
const void* g_savedScene1 = nullptr; // Renderer+0xd88 before we pointed it at view 0's scene
const void* g_writtenScene1 = nullptr;
void* g_borderLoop = nullptr; // DeferredRenderLoop whose 'Split border' byte we cleared (was 1)
bool g_possibleSet = false;
bool g_active = false;
int g_ownerResult = -1; // last SyncOwnerState result code (logged on change)
const void* g_ownerRider = nullptr;
void* g_cascadeLoop = nullptr; // DeferredRenderLoop whose split cascade cap we lifted
std::uint32_t g_savedCascadeCap = 0;
std::uint64_t g_frames = 0; // two-view frames so far

std::string Describe(const Desc& d)
{
    return fmt::format("mode {} cameras {} / {} mask 0x{:x}", d.mode, d.camera[0], d.camera[1], d.mask);
}

void* RenderLoop(void* renderer)
{
    void* loop = Read<void*>(renderer, kRendererRenderLoop);
    return loop && *static_cast<const void* const*>(loop) == g_deferredVftable ? loop : nullptr;
}

// Effector (vegetation bend) map slot 1 only gets a centre from a second game camera: give it slot 0's.
void SyncEffectorCenter()
{
    void* maps = *g_effectorMaps;
    if (!maps) {
        return;
    }
    const void* slot0 = Read<const void*>(maps, kEffectorSlots);
    if (!slot0) {
        return;
    }
    float center[3];
    std::memcpy(center, static_cast<const char*>(slot0) + kEffectorSlotCenter, sizeof(center));
    g_setCenterWorldPos(maps, center, 1); // no-op while slot 1 does not exist yet (DoUpdate creates it)
}

bool EnsurePossibleSplitMode()
{
    if (g_possibleSet) {
        return true;
    }
    void* rtm = *g_rtm;
    if (!rtm) {
        return false;
    }
    const int before = Read<int>(rtm, kRtmPossibleSplitMode);
    g_setPossibleSplitMode(rtm, 1); // recreates the 'Split' RTs if it changed
    g_possibleSet = true;
    TT2VR_LOG_INFO("split view: RenderTargetManager possible split mode {} -> 1", before);
    return true;
}

void StartOrKeep(void* renderer, const void* camera)
{
    const Desc two{1, {camera, camera}, 3u};
    if (!g_writing) {
        g_savedScene1 = Read<const void*>(renderer, kRendererScenes + 8);
        TT2VR_LOG_INFO("split view: two views on (camera {}); game desc {}", camera, Describe(g_game));
    }
    else if (Read<const void*>(renderer, kRendererScenes + 8) != g_writtenScene1) {
        g_savedScene1 = Read<const void*>(renderer, kRendererScenes + 8); // the game set view 1's scene itself
    }
    WriteDesc(renderer, two);
    g_written = two;
    g_writing = true;
    g_writtenScene1 = Read<const void*>(renderer, kRendererScenes);
    Write(renderer, kRendererScenes + 8, g_writtenScene1);
    SyncEffectorCenter();
    if (void* loop = RenderLoop(renderer); loop && Read<std::uint8_t>(loop, kDeferredLoopSplitBorder) == 1) {
        Write<std::uint8_t>(loop, kDeferredLoopSplitBorder, 0);
        g_borderLoop = loop;
    }
}

enum OwnerResult { kOwnerOk, kOwnerNoRider, kOwnerNoPlayer, kOwnerNotLocal, kOwnerIndex, kOwnerFault };
const char* OwnerResultName(int r)
{
    static const char* const names[] = {"ok", "no rider camera", "no player", "player not local (kind != 0)",
        "player camera index != 0", "access fault"};
    return r >= 0 && r < static_cast<int>(std::size(names)) ? names[r] : "?";
}

// The game hides the rider's own suit and helmet (render flags), the shadow decal and switches first-person SSR /
// probe factor for the player's camera index only (FUN_1403aea40, FUN_1403aec70). View 1 is a second index, so it
// gets index 0's values every frame (the game rewrites index 0 on every view change). Index 1 is unused in one-view
// frames, so nothing is restored. `rider` is the object FUN_1403afc80 / FUN_1403aea40 get (a TT2 camera controller;
// runtime 2026-10-16: not the KTUCamera, and its +0x10 is not the rendered Graphics::Camera).
int SyncOwnerState(void* renderer, const void* rider)
{
    __try {
        if (!rider) {
            return kOwnerNoRider;
        }
        const void* player = Read<const void*>(rider, kTTCameraPlayer);
        if (!player) {
            return kOwnerNoPlayer;
        }
        if (Read<int>(player, kPlayerKind) != 0) {
            return kOwnerNotLocal;
        }
        if (Read<std::uint16_t>(player, kPlayerCameraIndex) != 0) {
            return kOwnerIndex;
        }
        for (int i = 0; i < 3; ++i) {
            if (void* entity = Read<void*>(rider, kTTCameraOwnMeshes + i * 8)) {
                Write(entity, kRenderEntityFlags + 2, Read<std::uint16_t>(entity, kRenderEntityFlags));
            }
        }
        for (int i = 0; i < 2; ++i) {
            const void* decal = Read<const void*>(rider, kTTCameraShadowDecals + i * 8);
            const void* volume = decal ? Read<const void*>(decal, kDecalVolume) : nullptr;
            const void* owner = volume ? Read<const void*>(volume, kVolumeOwner) : nullptr;
            auto* bytes = owner ? Read<std::uint8_t*>(owner, kVolumeOwnerVisibility) : nullptr;
            if (bytes) {
                std::uint8_t& b = bytes[Read<int>(volume, kVolumeIndex)];
                b = static_cast<std::uint8_t>((b & ~2u) | ((b & 1u) << 1));
            }
        }
        if (void* loop = RenderLoop(renderer)) {
            if (void* lighting = Read<void*>(loop, kRenderLoopDeferredLighting)) {
                Write(lighting, kLightingFrontPassSsr + 1, Read<std::uint8_t>(lighting, kLightingFrontPassSsr));
                Write(lighting, kLightingProbeFactor + 4, Read<float>(lighting, kLightingProbeFactor));
            }
        }
        return kOwnerOk;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return kOwnerFault;
    }
}

// Split screen trades quality for two players (fewer CSM cascades per view); two eyes need the one-view quality.
void LiftSplitCaps(void* renderer)
{
    void* loop = RenderLoop(renderer);
    if (!loop || loop == g_cascadeLoop) {
        return;
    }
    g_cascadeLoop = loop;
    g_savedCascadeCap = Read<std::uint32_t>(loop, kRenderLoopSplitCascadeCount);
    Write<std::uint32_t>(loop, kRenderLoopSplitCascadeCount, 0xffffffffu);
    TT2VR_LOG_INFO("split view: split cascade cap {} lifted (split FXAA {}, SSAO {})", g_savedCascadeCap,
        Read<std::uint8_t>(loop, kRenderLoopSplitFxaa), Read<std::uint8_t>(loop, kRenderLoopSplitSsao));
}

void RestoreSplitCaps(void* renderer)
{
    if (g_cascadeLoop && g_cascadeLoop == RenderLoop(renderer)) {
        Write<std::uint32_t>(g_cascadeLoop, kRenderLoopSplitCascadeCount, g_savedCascadeCap);
    }
    g_cascadeLoop = nullptr;
}

void Stop(void* renderer)
{
    if (!g_writing) {
        return;
    }
    WriteDesc(renderer, g_game);
    if (Read<const void*>(renderer, kRendererScenes + 8) == g_writtenScene1) {
        Write(renderer, kRendererScenes + 8, g_savedScene1);
    }
    if (g_borderLoop && g_borderLoop == RenderLoop(renderer)) {
        Write<std::uint8_t>(g_borderLoop, kDeferredLoopSplitBorder, 1);
    }
    g_borderLoop = nullptr;
    RestoreSplitCaps(renderer);
    g_writing = false;
    TT2VR_LOG_INFO("split view: two views off after {} frames; game desc {} restored", g_frames, Describe(g_game));
}

} // namespace

void InitSplitView()
{
    g_rtm = static_cast<void* const*>(SignatureAddress(Sig::RenderTargetManagerInstance));
    g_setPossibleSplitMode =
        reinterpret_cast<SetPossibleSplitModeFn>(SignatureAddress(Sig::RenderTargetManagerSetPossibleSplitMode));
    g_effectorMaps = static_cast<void* const*>(SignatureAddress(Sig::EffectorMapManagerInstance));
    g_setCenterWorldPos = reinterpret_cast<SetCenterWorldPosFn>(SignatureAddress(Sig::EffectorMapManagerSetCenterWorldPos));
    g_deferredVftable = SignatureAddress(Sig::DeferredRenderLoopVftable);
    if (!g_rtm || !g_setPossibleSplitMode || !g_effectorMaps || !g_setCenterWorldPos || !g_deferredVftable) {
        TT2VR_LOG_ERROR("split view: signatures missing; left eye only (Phase 3 mode)");
        return;
    }
    g_available = core::GetConfig().xr.stereo;
    TT2VR_LOG_INFO("split view: {}", g_available ? "ready (both eyes as split-screen views 0 and 1)"
                                                 : "off ([xr] stereo = false); left eye only");
}

bool SplitViewAvailable()
{
    return g_available;
}

const void* GameMainCamera(void* renderer)
{
    const Desc cur = ReadDesc(renderer);
    if (!g_haveGame) {
        g_haveGame = true;
        g_game = cur;
        TT2VR_LOG_INFO("split view: game camera desc {}", Describe(g_game));
    }
    else if (g_writing ? cur != g_written : cur != g_game) {
        // The game wrote a new desc. Under our override CameraManager::SetActiveCamera starts from the Renderer's
        // current (our) desc and swaps camera 0, keeping our mode and mask: take only the camera then.
        Desc next = cur;
        if (g_writing && cur.mode == g_written.mode && cur.mask == g_written.mask) {
            next = g_game;
            next.camera[0] = cur.camera[0];
        }
        if (next != g_game) { // a game that re-sends the same desc every frame logs nothing
            TT2VR_LOG_INFO("split view: game camera desc changed to {}", Describe(next));
        }
        g_game = next;
    }
    if (g_game.mode != 0 || g_game.mask == 0) {
        return nullptr; // the game's own split screen (not used offline) or no view
    }
    const int index = std::countr_zero(g_game.mask);
    return index <= 1 ? g_game.camera[index] : nullptr;
}

void ApplySplitView(void* renderer, const void* camera, bool twoViews, const void* riderCamera)
{
    twoViews = twoViews && g_available && camera && g_game.mode == 0 && g_game.mask == 1u &&
               camera == g_game.camera[0] && EnsurePossibleSplitMode();
    if (twoViews) {
        StartOrKeep(renderer, camera);
        LiftSplitCaps(renderer);
        const int owner = SyncOwnerState(renderer, riderCamera);
        if (owner != g_ownerResult || riderCamera != g_ownerRider) {
            g_ownerResult = owner;
            g_ownerRider = riderCamera;
            if (owner == kOwnerOk) {
                TT2VR_LOG_INFO("split view: rider state mirrored to view 1 (rider camera {})", riderCamera);
            }
            else {
                TT2VR_LOG_WARN("split view: rider state not mirrored ({}; rider camera {}); view 1 may show the rider's "
                               "own helmet",
                    OwnerResultName(owner), riderCamera);
            }
        }
        ++g_frames;
    }
    else {
        Stop(renderer);
    }
    g_active = twoViews;
}

bool SplitViewActive()
{
    return g_active;
}

std::string SplitViewSummary()
{
    if (!g_available) {
        return "split view: off";
    }
    void* rtm = g_rtm ? *g_rtm : nullptr;
    return fmt::format("split view: {} ({} two-view frames), RTM possible {} active {}", g_active ? "two views" : "one view",
        g_frames, rtm ? Read<int>(rtm, kRtmPossibleSplitMode) : -1, rtm ? Read<int>(rtm, kRtmSplitMode) : -1);
}

} // namespace game
