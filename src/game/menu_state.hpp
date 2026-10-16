// game::MenuState (Phase 3 interim menu screen, PLAN Phase 7): should the headset show the flat game on a screen
// (menus, intros, pause, replays) or the head-tracked view (riding)?
// Signals, all read-only (docs/re/frame-anatomy.md 8.4, docs/re/camera.md):
//   1. KTUGuiManager::IsMenuOpen / IsGamePaused on g_pGuiManager (exports). Runtime 2026-10-16: paused is true
//      during the startup intros; neither is set in TT2's main menu, which runs on its own menu state machine.
//   2. Riding camera: the player-0 KTUCamera (KTUGameEngine::GetCamera(0) on g_pGameEngine) has an active mode
//      (KTUCamera+0x48) whose Manage slot is TTSpringCameraMode::Manage, the on-bike camera for every ride view.
//      Menu, intro and replay cameras use other modes.
// Screen = menu open || paused || the riding camera is not active.
#pragma once

namespace game {

struct MenuState {
    bool available = false; // signals resolved (else never a menu: head tracking as in Phase 3)
    bool menuOpen = false;
    bool paused = false;
    bool springCamera = false; // player camera is in TTSpringCameraMode (riding)
    bool Active() const { return available && (menuOpen || paused || !springCamera); }
};

// Resolves the accessors (after ResolveSignatures). Missing ones leave `available` false.
void InitMenuState();
// Thread: game main thread. Reads the state and logs every change.
MenuState ReadMenuState();
MenuState LastMenuState();

} // namespace game
