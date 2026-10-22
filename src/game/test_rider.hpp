// game::TestRider (test mode only, 2026-10-22): remote "ai on|off" hands the local rider to the game's own AI (and
// back), so automated test rides keep going at speed instead of crashing every few seconds. Uses the game's setter
// for the rider's "Control" property (TTVehicle vtable +0x2c0: 0 human, 1 AI), as the game does when an online player
// disconnects, without that path's network checks. Never active outside -tt2vr-test.
#pragma once

#include <string>

namespace game {

// Thread: game main thread (remote commands). Returns the result line.
std::string AiRiderCommand(const std::string& arg);

} // namespace game
