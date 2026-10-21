// game::VirtualPad: a scripted XInput pad, installed in test mode only (core::TestMode; PLAN Decisions log
// 2026-10-21, exception to CLAUDE.md rule 5). XInput user 0 is replaced by the virtual pad (connected, neutral when no
// script runs), so Claude can navigate menus and restart races without anyone at the PC. The wheel (DirectInput) is
// not touched. Hooks: MinHook on the xinput1_3.dll exports the game calls (XInputGetState, XInputGetCapabilities,
// XInputGetKeystroke; button edges are also queued as keystrokes).
//
// Script (remote "pad <steps>"): steps separated by spaces or commas, run one after another.
//   a b x y start back lb rb ls rs up down left right   button tap: held 120 ms, then 200 ms released
//   <step>:<ms>                                         held for <ms> instead of 120
//   lt=<0..1> rt=<0..1> lx=<-1..1> ly= rx= ry=          axis (released to 0 afterwards, no gap)
//   <part>+<part>[:<ms>]                                several at once, e.g. rt=1+lx=-0.5:2000
//   wait:<ms> or <ms>                                   nothing pressed
#pragma once

#include <functional>
#include <string>

namespace game {

// Thread: game main thread. No-op outside test mode.
void InstallVirtualPad();
// Thread: present thread. Replaces a running script (that one reports "superseded").
void RunPadScript(const std::string& script, std::function<void(const std::string&)> done);
// Thread: present thread, once per frame (Present post-hook).
void TickVirtualPad();

} // namespace game
