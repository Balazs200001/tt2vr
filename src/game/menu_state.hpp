// game::MenuState (Phase 3 interim menu screen, PLAN Phase 7): is a game menu open or the game paused?
// Read through KTUGuiManager's own exported accessors on g_pGuiManager (docs/re/frame-anatomy.md 8.4), never by
// writing anything. Semantics are `likely` until the runtime check (log lines "menu state: ...") confirms that a
// race shows neither flag and the main menu / pause screen show one of them.
#pragma once

namespace game {

struct MenuState {
    bool available = false; // signatures resolved
    bool menuOpen = false;
    bool paused = false;
    bool Active() const { return menuOpen || paused; }
};

// Resolves the accessors (after ResolveSignatures). Missing ones leave `available` false: never a menu.
void InitMenuState();
// Thread: game main thread. Reads the state and logs every change.
MenuState ReadMenuState();
MenuState LastMenuState();

} // namespace game
