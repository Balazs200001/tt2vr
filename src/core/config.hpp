// TOML configuration (config/tt2vr.toml, deployed to <game>\tt2vr\tt2vr.toml) and hotkey parsing.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace core {

struct Hotkey {
    unsigned vk = 0; // Win32 virtual-key code; 0 = disabled
    bool ctrl = false, shift = false, alt = false;

    bool Valid() const { return vk != 0; }
    // Exact modifier match: Shift+F11 must NOT match a plain F11 hotkey and vice versa.
    bool Matches(unsigned pressedVk, bool ctrlDown, bool shiftDown, bool altDown) const
    {
        return vk != 0 && pressedVk == vk && ctrlDown == ctrl && shiftDown == shift && altDown == alt;
    }
};

// Case-insensitive, '+'-separated, whitespace tolerant: "F11", "ctrl+F11", "Ctrl + Shift + F12", "Alt+P",
// "Insert", "Delete", "Home", "End", "PageUp", "PageDown", "Pause", "ScrollLock", "Num0".."Num9", "A".."Z",
// "0".."9", "F1".."F24". "none" or "" gives Hotkey{} (disabled; a value, not nullopt).
// Unknown key, duplicate key or modifier, and modifier-only input give nullopt.
std::optional<Hotkey> ParseHotkey(std::string_view text);
// Canonical "Ctrl+Shift+F11"; "none" when disabled. Round-trips with ParseHotkey.
std::string FormatHotkey(const Hotkey& hk);

struct Config {
    struct Log {
        std::string level = "info"; // trace, debug, info, warn, error
        bool verbose_frames = false; // per-frame logging (CLAUDE.md: only behind a flag)
    } log;
    struct Hotkeys {
        Hotkey overlay{0x79}; // F10
        Hotkey frame_trace{0x7A}; // F11
        Hotkey texture_dump{0x7A, false, true}; // Shift+F11
        Hotkey renderdoc_capture{0x78}; // F9
    } hotkeys;
    struct Overlay {
        bool visible = true;
    } overlay;
    struct Camera {
        bool helmet_follows_view = true; // move the helmet mesh with the view/head override (Phase 2)
    } camera;
    struct Capture {
        std::string dir = "captures"; // relative to <game>\tt2vr\ unless absolute
        std::string renderdoc_dir; // empty = same as dir
        int max_textures = 64; // texture dump cap per request, clamp 1..1024
        bool include_depth = true;
        int cb_floats = 16; // floats of each constant-buffer update logged in a frame trace, clamp 0..1024
    } capture;
    struct Commands {
        bool enabled = true;
        int poll_frames = 10; // check tt2vr\commands\ every N presented frames, clamp 1..600
    } commands;
    struct Debug {
        std::string fake_exe_sha256; // non-empty: pretend TT2.exe has this hash (tests the unknown-version path)
    } debug;
};

struct ConfigResult {
    Config config;
    std::vector<std::string> warnings; // human readable: parse errors, unknown keys, bad values, clamps
};

// Never throws. Syntax error: all defaults + a warning (with line/column). Unknown tables/keys: warning, ignored.
// Wrong type or invalid value for a key: that key keeps its default + warning. Out of range: clamped + warning.
ConfigResult ParseConfig(std::string_view tomlText);
// Missing file: defaults + warning "config file not found: <path>". Read error: defaults + warning.
ConfigResult LoadConfig(const std::filesystem::path& file);

// Process-wide config: set once at startup by the init thread, read afterwards. Reads are thread-safe;
// a reference returned by GetConfig stays valid for the life of the process, even across SetConfig calls.
const Config& GetConfig();
void SetConfig(const Config& config);

// Warnings from the startup load, shown by the overlay. Written once by the init thread before the
// game creates its device; read-only afterwards.
void SetConfigWarnings(std::vector<std::string> warnings);
const std::vector<std::string>& ConfigWarnings();

} // namespace core
