#include "game/menu_state.hpp"

#include "core/log.hpp"
#include "game/engine_layout.hpp"
#include "game/signatures.hpp"

#include <windows.h>

namespace game {
namespace {

using BoolGetterFn = bool (*)(const void* guiManager);
using GetCameraFn = void* (*)(void* gameEngine, int player);
using GetTimeFactorFn = float (*)(const void* gameContext);

void* const* g_gui = nullptr; // &g_pGuiManager
BoolGetterFn g_isMenuOpen = nullptr;
BoolGetterFn g_isGamePaused = nullptr;
void* const* g_engine = nullptr; // &g_pGameEngine
GetCameraFn g_getCamera = nullptr;
const void* g_springManage = nullptr;
void* const* g_context = nullptr; // &GameContext::s_pInstance
GetTimeFactorFn g_getTimeFactor = nullptr;
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
    g_context = static_cast<void* const*>(SignatureAddress(Sig::GameContextInstance));
    g_getTimeFactor = reinterpret_cast<GetTimeFactorFn>(SignatureAddress(Sig::GameContextGetTimeFactor));
    if (!g_gui || !g_isMenuOpen || !g_isGamePaused || !g_engine || !g_getCamera || !g_springManage || !g_context ||
        !g_getTimeFactor) {
        g_gui = nullptr;
        TT2VR_LOG_WARN("menu state: signals missing; menus are head-tracked like riding");
        return;
    }
    TT2VR_LOG_INFO("menu state: KTUGuiManager menu/pause, game time factor, player camera (TTSpringCameraMode and "
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
        if (const void* context = *g_context) {
            s.timeStopped = g_getTimeFactor(context) == 0.0f;
        }
    }
    if (s.menuOpen != g_last.menuOpen || s.paused != g_last.paused || s.springCamera != g_last.springCamera ||
        s.timeStopped != g_last.timeStopped) {
        TT2VR_LOG_INFO("menu state: menu open {}, paused {}, time stopped {}, riding camera {} -> {}", s.menuOpen,
            s.paused, s.timeStopped, s.springCamera, s.Active() ? "screen" : "head-tracked");
    }
    g_last = s;
    return s;
}

MenuState LastMenuState()
{
    return g_last;
}

} // namespace game
