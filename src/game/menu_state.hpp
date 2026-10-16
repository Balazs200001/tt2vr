// game::MenuState (Phase 3 interim menu screen, PLAN Phase 7): should the headset show the flat game on a screen
// (menus, intros, pause, replays) or the head-tracked view (riding)?
// Signals, all read-only (docs/re/frame-anatomy.md 8.4, docs/re/camera.md):
//   1. KTUGuiManager::IsMenuOpen / IsGamePaused on g_pGuiManager (exports). Runtime 2026-10-16: paused is true
//      during the startup intros; neither is set in TT2's main menu, which runs on its own menu state machine.
//   2. Riding camera: the player-0 KTUCamera (KTUGameEngine::GetCamera(0) on g_pGameEngine) has an active mode
//      (KTUCamera+0x48) whose Manage slot is TTSpringCameraMode::Manage, the on-bike camera for every ride view,
//      AND the view being rendered is that KTUCamera's Graphics::Camera (KTUCamera+0x10). Runtime 2026-10-16: the
//      spring mode starts ~5 s before the race view while a loading/menu camera still renders.
//      Menu, intro and replay cameras use other modes.
//   3. Game time stopped: ViewContext::s_bMotionPause (export, written by RenderLoop::SetTime). Runtime 2026-10-16:
//      the time-attack pause menu sets neither KTUGuiManager flag.
// Screen = menu open || paused || time stopped || the riding camera is not the rendered view.
#pragma once

namespace game {

struct MenuState {
    bool available = false; // signals resolved (else never a menu: head tracking as in Phase 3)
    bool menuOpen = false;
    bool paused = false;
    bool springCamera = false; // player camera is in TTSpringCameraMode and is the rendered view (riding)
    bool motionPaused = false; // game time stopped
    bool Active() const { return available && (menuOpen || paused || motionPaused || !springCamera); }
};

// Resolves the accessors (after ResolveSignatures). Missing ones leave `available` false.
void InitMenuState();
// Thread: game main thread (Renderer::DoUpdate). `mainCamera` is the Graphics::Camera of the view about to be built.
// Reads the state and logs every change.
MenuState ReadMenuState(const void* mainCamera);
MenuState LastMenuState();

} // namespace game
