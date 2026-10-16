#include "game/menu_state.hpp"

#include "core/log.hpp"
#include "game/signatures.hpp"

namespace game {
namespace {

using BoolGetterFn = bool (*)(const void* guiManager);

void* const* g_instance = nullptr; // &g_pGuiManager
BoolGetterFn g_isMenuOpen = nullptr;
BoolGetterFn g_isGamePaused = nullptr;
MenuState g_last;

} // namespace

void InitMenuState()
{
    g_instance = static_cast<void* const*>(SignatureAddress(Sig::GuiManagerInstance));
    g_isMenuOpen = reinterpret_cast<BoolGetterFn>(SignatureAddress(Sig::GuiManagerIsMenuOpen));
    g_isGamePaused = reinterpret_cast<BoolGetterFn>(SignatureAddress(Sig::GuiManagerIsGamePaused));
    if (!g_instance || !g_isMenuOpen || !g_isGamePaused) {
        g_instance = nullptr;
        TT2VR_LOG_WARN("menu state: KTUGuiManager accessors missing; menus are treated like riding");
        return;
    }
    TT2VR_LOG_INFO("menu state: reading KTUGuiManager::IsMenuOpen / IsGamePaused");
}

MenuState ReadMenuState()
{
    MenuState s;
    if (g_instance) {
        s.available = true;
        if (const void* gui = *g_instance) {
            s.menuOpen = g_isMenuOpen(gui);
            s.paused = g_isGamePaused(gui);
        }
    }
    if (s.menuOpen != g_last.menuOpen || s.paused != g_last.paused) {
        TT2VR_LOG_INFO("menu state: menu open {}, paused {}", s.menuOpen, s.paused);
    }
    g_last = s;
    return s;
}

MenuState LastMenuState()
{
    return g_last;
}

} // namespace game
