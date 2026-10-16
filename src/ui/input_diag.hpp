// ui::InputDiagnostics ([debug] input_log): read-only logging to diagnose "steering stops responding".
// Logs, with timestamps in tt2vr.log:
//   - every change of the Windows foreground window (game window or not, process name, title);
//   - XInput pads connecting / disconnecting (slots 0..3);
//   - once per second while on: each connected pad's left stick X/Y, triggers and buttons, and game focus.
// Never changes input (CLAUDE.md rule 5): only GetForegroundWindow and XInputGetState are called.
#pragma once

#include <windows.h>

namespace ui {

// Thread: game main thread (Present post-hook). Cheap when [debug] input_log is off (focus check only).
void TickInputDiagnostics(HWND gameWindow);

} // namespace ui
