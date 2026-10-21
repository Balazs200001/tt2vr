#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace core {

// Remote requests: a tool writes <game>\tt2vr\commands\<id>.txt; the mod picks it up on the present
// thread, deletes it, runs the commands and writes <id>.done with one result line per command.
enum class CommandKind {
    FrameTrace,       // "frametrace" / "trace"
    TextureDump,      // "dump"
    RenderDocCapture, // "capture [name]" / "renderdoc [name]"
    ShaderRegistry,   // "shaders"
    Camera,           // "camera": log the main view camera (Phase 2) and return a summary line
    XrProbe,          // "xrprobe": alpha of the back buffer and of the images handed to the headset (next frame)
    LeakTest,         // "leaktest [taa|ssr|froxel]": tint eye 0's history of that row once, report each half's red
    TaaReset,         // "taareset": reset both eyes' TAA history next frame
    Convergence,      // "convergence": per-eye frame-to-frame change over 10 frames (static TAA convergence test)
    CutTest,          // "cuttest [noreset]": frame-to-frame change after the next camera cut (game::RequestCutTest)
    ShiftTest,        // "shifttest": no reset, rebased previous cameras at the next World Shift (RequestShiftTest)
    Views,            // "views on|off": the overlay's per-eye history views (game::TemporalDebug)
    Freeze,           // "freeze on|off": [debug] freeze eye R's TAA history (game::TemporalDebug)
    Screenshot,       // "screenshot [name]" / "screen [name]": the engine's back buffer as a PNG (next frame)
    Pad,              // "pad <steps>": virtual XInput pad script (test mode only, game::RunPadScript)
    Head,             // "head still | <yaw> [pitch] [roll] | shake <deg> <period s> [seconds]": simulated headset
    Unknown,
};

struct Command {
    CommandKind kind = CommandKind::Unknown;
    std::string arg;  // capture / screenshot name ([A-Za-z0-9_-], max 64 chars, others become '_'); pad / head: raw;
                      // frametrace: frame count (same sanitizing)
    std::string text; // the original line
};

// One command per non-empty line; '#' starts a comment; keywords are case-insensitive.
std::vector<Command> ParseCommands(std::string_view text);

struct CommandFile {
    std::filesystem::path path; // the request file (already deleted)
    std::vector<Command> commands;
};

// Reads and deletes every *.txt request in `dir`, oldest first. Never throws.
std::vector<CommandFile> TakeCommandFiles(const std::filesystem::path& dir);

// Writes <request stem>.done next to the request.
void WriteDone(const std::filesystem::path& request, const std::vector<std::string>& results);

} // namespace core
