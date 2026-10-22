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
        int perf_interval_s = 10; // one fps / frame-time line every N seconds, 0 = off, clamp 0..600
    } log;
    struct Hotkeys {
        Hotkey overlay{0x79}; // F10
        Hotkey frame_trace{0x7A}; // F11
        Hotkey texture_dump{0x7A, false, true}; // Shift+F11
        Hotkey renderdoc_capture{0x78}; // F9
        Hotkey cursor{0x77}; // F8: show/hide the overlay's own mouse cursor
        Hotkey recenter{0x76}; // F7: recenter the headset view (Phase 3)
        // Seated calibration (PLAN 8d): move the eye point 1 cm per press (numpad, NumLock on), Num5 = back to 0.
        Hotkey seat_forward{0x68}, seat_back{0x62}, seat_up{0x69}, seat_down{0x63}, seat_reset{0x65};
    } hotkeys;
    struct Overlay {
        bool visible = true;
        bool show_cursor = false; // draw the overlay's mouse cursor at startup (the game hides the OS cursor)
        bool options_tab = true; // PLAN 8i: a VR tab in the game's Options > Display with the runtime settings
    } overlay;
    struct Camera {
        bool helmet_follows_view = true; // move the helmet mesh with the view/head override (Phase 2)
        bool remove_auto_look = true; // head-tracked: drop the game's look-into-corner (Dynamic View) and stick look
        float horizon_lock = 100.0f; // head-tracked: percent of the camera's roll against the horizon removed, 0..100
        float shake = 0.0f; // in VR: percent of the game's camera shake and speed noise kept, 0..100
        // Eye point offset from the recentered head (PLAN 8d), metres in the camera's axes, clamp -0.5..0.5.
        float seat_forward = 0.0f, seat_up = 0.0f, seat_right = 0.0f;
    } camera;
    struct Xr {
        bool enabled = true; // start OpenXR when a runtime and headset are available (Phase 3)
        float resolution_scale = 1.0f; // eye swapchain size = runtime recommendation * this, clamp 0.25..2.0
        float render_scale = 0.9f; // PLAN 9e: game renders each eye at this fraction of it, FSR1 upscales, 0.5..1
        float fsr_sharpness = 0.0f; // PLAN 9e: FSR1 RCAS sharpening in stops (0 = strongest, 2 = mild), 0..2
        bool desktop_vsync_off = true; // Present with sync interval 0 while the XR session runs (xrWaitFrame paces)
        bool desktop_mirror = true; // false: skip the desktop Present while the XR session runs (window freezes)
        bool menu_screen = true; // menus / pause: show the flat game on a world-fixed screen instead of head tracking
        float screen_distance = 2.0f; // metres in front of the recentered head, clamp 0.5..10
        float screen_width = 2.4f; // metres (height follows the game window's aspect), clamp 0.25..10
        bool force_render_size = true; // render the game at the eye swapchain size while a headset is connected
        bool stereo = true; // both eyes as the engine's two split-screen views (Phase 4); false = left eye only
        std::string desktop_view = "left"; // window while the session runs: "left" eye, "both" eyes, "game" (raw)
        bool desktop_fill = true; // desktop image covers the window (cropped, no bars); false = fits (letterboxed)
        float shadow_scale = 1.5f; // cascade shadow maps at this multiple of the game's size (1..4; static map: next
                                   // power of two)
        bool ssao = false; // keep the game's SSAO while the headset session runs (screen-space AO swims in VR)
        bool motion_blur = false; // keep the game's motion blur while the headset session runs (smears in VR)
        bool fxaa = false; // PLAN 9c: keep the game's FXAA before TAA in VR (the menu's "TAA" runs both)
        bool vignette = false; // PLAN 9c: keep the game's screen-edge vignette in VR
        bool shared_shadows = true; // one cascaded shadow render per frame for both eyes (read at startup)
        float world_scale = 1.0f; // PLAN 8e: the world looks this much larger (head motion and IPD / this), 0.5..2
        float ipd_scale = 1.0f; // PLAN 8e: eye separation multiplier on top of world_scale, 0.5..2
        bool visor_drops_shared = true; // PLAN 8f: the right eye shows the left eye's visor drops
        bool occlusion = true; // false: no occlusion culling while the headset session runs (A/B, see-through flashes)
        bool visibility_mask = true; // PLAN 9: skip the pixels the lenses never show (XR_KHR_visibility_mask)
        bool depth_submit = true; // PLAN 9f: submit each eye's depth with its image (XR_KHR_composition_layer_depth)
    } xr;
    struct Hud {
        bool enabled = true; // the game's 2D HUD on its own panel while riding in the headset (Phase 7)
        int pixels = 1920; // panel texture width, clamp 512..4096 (height = pixels / aspect)
        float aspect = 1.78f; // layout width / height, clamp 1.0..2.4 (narrower pulls the corner gauges inward)
        float spread = 1.3f; // layout height = 1080 x this, clamp 1.0..2.0: top and bottom elements further apart
        float distance = 1.0f; // metres in front of the recentered head, clamp 0.3..5
        float width = 0.9f; // metres, clamp 0.2..4
        float height = -0.1f; // metres above (+) or below (-) eye height, clamp -1..1
        float tilt = 0.0f; // degrees the panel's top leans away from you, clamp -60..60
        std::string follow = "seat"; // "seat": fixed to the bike (LOCAL space); "head": follows the head
        bool desktop = true; // also draw the captured HUD over the desktop mirror
    } hud;
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
        bool input_log = false; // log focus changes and XInput pad state once per second (diagnostics, read-only)
        bool flash_detect = false; // log (and save thumbnails of) single-frame glitches in the rendered eyes
    } debug;
};

struct ConfigResult {
    Config config;
    std::vector<std::string> warnings; // human readable: parse errors, unknown keys, bad values, clamps
};

// Never throws. Syntax error: all defaults + a warning (with line/column). Unknown tables/keys: warning, ignored.
// Wrong type or invalid value for a key: that key keeps its default + warning. Out of range: clamped + warning.
ConfigResult ParseConfig(std::string_view tomlText);
// The same, on top of `base` instead of the defaults: keys missing from the text keep base's values (user.toml over
// tt2vr.toml). A syntax error returns `base` unchanged + a warning.
ConfigResult ParseConfig(std::string_view tomlText, const Config& base);
// Missing file: defaults + warning "config file not found: <path>". Read error: defaults + warning.
ConfigResult LoadConfig(const std::filesystem::path& file);
// On top of `base`; a missing file gives `base` without a warning when `missingOk`.
ConfigResult LoadConfig(const std::filesystem::path& file, const Config& base, bool missingOk);

// Process-wide config: set once at startup by the init thread, read afterwards. Reads are thread-safe;
// a reference returned by GetConfig stays valid for the life of the process, even across SetConfig calls.
const Config& GetConfig();
void SetConfig(const Config& config);
// tt2vr.toml alone (before user.toml): what the runtime settings are compared with when saving user.toml.
const Config& GetFileConfig();
void SetFileConfig(const Config& config);

// Warnings from the startup load, shown by the overlay. Written once by the init thread before the
// game creates its device; read-only afterwards.
void SetConfigWarnings(std::vector<std::string> warnings);
const std::vector<std::string>& ConfigWarnings();

} // namespace core
