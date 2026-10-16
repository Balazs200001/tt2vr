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
    Unknown,
};

struct Command {
    CommandKind kind = CommandKind::Unknown;
    std::string arg;  // capture name ([A-Za-z0-9_-], max 64 chars; other characters become '_')
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
