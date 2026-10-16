// ui::InputDiagnostics ([debug] input_log): read-only logging to diagnose "steering stops responding".
// Logs, with timestamps in tt2vr.log:
//   - every change of the Windows foreground window (game window or not, process name, title);
//   - XInput pads connecting / disconnecting (slots 0..3);
//   - DirectInput game controllers (wheels, pedals) attaching / detaching, checked every 2 s;
//   - once per second while on: each connected pad's left stick X/Y, triggers and buttons, each DirectInput
//     device's axes, and game focus.
// Never changes input (CLAUDE.md rule 5): GetForegroundWindow, XInputGetState, and our own DirectInput devices opened
// non-exclusive in the background (no force feedback, never exclusive), so the game's own access is unaffected.
#pragma once

#include <windows.h>

namespace ui {

// Thread: game main thread (Present post-hook). Cheap when [debug] input_log is off (focus check only).
void TickInputDiagnostics(HWND gameWindow);

} // namespace ui
