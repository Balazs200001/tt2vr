// Test mode (PLAN Decisions log 2026-10-21, exception to CLAUDE.md rule 5): the game was started with the command-line
// flag -tt2vr-test (tools/tt2vr-test.sh launch). It turns on the simulated headset (no OpenXR, xr::Simulated) and the
// virtual XInput pad (game::InstallVirtualPad), so the in-game tests run without anyone wearing the headset. A normal
// launch never has the flag, so nothing here changes normal play.
#pragma once

namespace core {

// Thread: any. Read once from the process command line.
bool TestMode();

} // namespace core
