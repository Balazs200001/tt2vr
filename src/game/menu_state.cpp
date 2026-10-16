#include "game/menu_state.hpp"

#include "core/log.hpp"
#include "game/engine_layout.hpp"
#include "game/signatures.hpp"

#include <windows.h>

namespace game {
namespace {

using BoolGetterFn = bool (*)(const void* guiManager);
using GetCameraFn = void* (*)(void* gameEngine, int player);

void* const* g_gui = nullptr; // &g_pGuiManager
BoolGetterFn g_isMenuOpen = nullptr;
BoolGetterFn g_isGamePaused = nullptr;
void* const* g_engine = nullptr; // &g_pGameEngine
GetCameraFn g_getCamera = nullptr;
const void* g_springManage = nullptr;
const bool* g_motionPause = nullptr;
int g_motionPauseFrames = 0; // consecutive DoUpdate calls with game time stopped
constexpr int kMotionPauseFrames = 5; // a single dt == 0 frame must not flip the headset to the screen
MenuState g_last;

// Player-0 camera mode is TTSpringCameraMode and its Graphics::Camera is the rendered view. SEH guard: these are
// game objects we only read.
bool SpringCameraActive(const void* mainCamera)
{
    __try {
        void* engine = *g_engine;
        if (!engine) {
            return false;
        }
        const void* camera = g_getCamera(engine, 0);
        if (!camera || layout::Read<const void*>(camera, layout::kKTUCameraCamera) != mainCamera) {
            return false;
        }
        const void* mode = layout::Read<const void*>(camera, layout::kKTUCameraMode);
        if (!mode) {
            return false;
        }
        const void* const* vtable = *static_cast<const void* const* const*>(mode);
        return vtable && vtable[layout::kModeManageSlot] == g_springManage;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

void InitMenuState()
{
    g_gui = static_cast<void* const*>(SignatureAddress(Sig::GuiManagerInstance));
    g_isMenuOpen = reinterpret_cast<BoolGetterFn>(SignatureAddress(Sig::GuiManagerIsMenuOpen));
    g_isGamePaused = reinterpret_cast<BoolGetterFn>(SignatureAddress(Sig::GuiManagerIsGamePaused));
    g_engine = static_cast<void* const*>(SignatureAddress(Sig::GameEngineInstance));
    g_getCamera = reinterpret_cast<GetCameraFn>(SignatureAddress(Sig::GameEngineGetCamera));
    g_springManage = SignatureAddress(Sig::SpringCameraModeManage);
    g_motionPause = static_cast<const bool*>(SignatureAddress(Sig::MotionPause));
    if (!g_gui || !g_isMenuOpen || !g_isGamePaused || !g_engine || !g_getCamera || !g_springManage || !g_motionPause) {
        g_gui = nullptr;
        TT2VR_LOG_WARN("menu state: signals missing; menus are head-tracked like riding");
        return;
    }
    TT2VR_LOG_INFO("menu state: KTUGuiManager menu/pause, motion pause, player camera (TTSpringCameraMode and "
                   "rendered = riding)");
}

MenuState ReadMenuState(const void* mainCamera)
{
    MenuState s;
    if (g_gui) {
        s.available = true;
        if (const void* gui = *g_gui) {
            s.menuOpen = g_isMenuOpen(gui);
            s.paused = g_isGamePaused(gui);
        }
        s.springCamera = mainCamera && SpringCameraActive(mainCamera);
        g_motionPauseFrames = *g_motionPause ? g_motionPauseFrames + 1 : 0;
        s.motionPaused = g_motionPauseFrames >= kMotionPauseFrames;
    }
    if (s.menuOpen != g_last.menuOpen || s.paused != g_last.paused || s.springCamera != g_last.springCamera ||
        s.motionPaused != g_last.motionPaused) {
        TT2VR_LOG_INFO("menu state: menu open {}, paused {}, time stopped {}, riding camera {} -> {}", s.menuOpen,
            s.paused, s.motionPaused, s.springCamera, s.Active() ? "screen" : "head-tracked");
    }
    g_last = s;
    return s;
}

MenuState LastMenuState()
{
    return g_last;
}

} // namespace game
