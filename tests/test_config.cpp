#include <doctest/doctest.h>

#include "core/config.hpp"
#include "core/user_toml.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <windows.h>

using core::Config;
using core::FormatHotkey;
using core::Hotkey;
using core::ParseConfig;
using core::ParseHotkey;

namespace {

bool ContainsWarning(const core::ConfigResult& r, const std::string& needle)
{
    for (const auto& w : r.warnings) {
        if (w.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool SameHotkey(const Hotkey& a, const Hotkey& b)
{
    return a.vk == b.vk && a.ctrl == b.ctrl && a.shift == b.shift && a.alt == b.alt;
}

bool SameConfig(const Config& a, const Config& b)
{
    return a.log.level == b.log.level && a.log.verbose_frames == b.log.verbose_frames &&
           a.log.perf_interval_s == b.log.perf_interval_s &&
           SameHotkey(a.hotkeys.overlay, b.hotkeys.overlay) && SameHotkey(a.hotkeys.frame_trace, b.hotkeys.frame_trace) &&
           SameHotkey(a.hotkeys.texture_dump, b.hotkeys.texture_dump) &&
           SameHotkey(a.hotkeys.renderdoc_capture, b.hotkeys.renderdoc_capture) &&
           SameHotkey(a.hotkeys.cursor, b.hotkeys.cursor) && SameHotkey(a.hotkeys.recenter, b.hotkeys.recenter) &&
           a.xr.enabled == b.xr.enabled && a.xr.resolution_scale == b.xr.resolution_scale &&
           a.xr.desktop_vsync_off == b.xr.desktop_vsync_off && a.xr.desktop_mirror == b.xr.desktop_mirror &&
           a.xr.menu_screen == b.xr.menu_screen && a.xr.screen_distance == b.xr.screen_distance &&
           a.xr.screen_width == b.xr.screen_width && a.xr.force_render_size == b.xr.force_render_size && a.overlay.show_cursor == b.overlay.show_cursor &&
           a.overlay.visible == b.overlay.visible && a.overlay.options_tab == b.overlay.options_tab &&
           a.camera.helmet_follows_view == b.camera.helmet_follows_view &&
           a.camera.remove_auto_look == b.camera.remove_auto_look && a.camera.horizon_lock == b.camera.horizon_lock &&
           a.camera.shake == b.camera.shake && a.camera.seat_forward == b.camera.seat_forward &&
           a.camera.seat_up == b.camera.seat_up && a.camera.seat_right == b.camera.seat_right &&
           a.xr.world_scale == b.xr.world_scale && a.xr.ipd_scale == b.xr.ipd_scale &&
           a.xr.visor_drops_shared == b.xr.visor_drops_shared && a.xr.fxaa == b.xr.fxaa &&
           a.xr.vignette == b.xr.vignette && a.xr.shadow_scale == b.xr.shadow_scale &&
           a.xr.render_scale == b.xr.render_scale && a.xr.fsr_sharpness == b.xr.fsr_sharpness &&
           a.xr.occlusion == b.xr.occlusion &&
           SameHotkey(a.hotkeys.seat_forward, b.hotkeys.seat_forward) &&
           SameHotkey(a.hotkeys.seat_back, b.hotkeys.seat_back) && SameHotkey(a.hotkeys.seat_up, b.hotkeys.seat_up) &&
           SameHotkey(a.hotkeys.seat_down, b.hotkeys.seat_down) &&
           SameHotkey(a.hotkeys.seat_reset, b.hotkeys.seat_reset) &&
           a.capture.dir == b.capture.dir &&
           a.capture.renderdoc_dir == b.capture.renderdoc_dir && a.capture.max_textures == b.capture.max_textures &&
           a.capture.include_depth == b.capture.include_depth && a.capture.cb_floats == b.capture.cb_floats &&
           a.commands.enabled == b.commands.enabled && a.commands.poll_frames == b.commands.poll_frames &&
           a.debug.fake_exe_sha256 == b.debug.fake_exe_sha256 && a.debug.input_log == b.debug.input_log &&
           a.hud.enabled == b.hud.enabled && a.hud.pixels == b.hud.pixels && a.hud.aspect == b.hud.aspect && a.hud.spread == b.hud.spread &&
           a.hud.distance == b.hud.distance && a.hud.width == b.hud.width && a.hud.height == b.hud.height &&
           a.hud.tilt == b.hud.tilt && a.hud.follow == b.hud.follow && a.hud.desktop == b.hud.desktop;
}

// Finds config/tt2vr.toml: TT2VR_SOURCE_DIR, else walk up from the current directory and the exe directory.
std::filesystem::path FindShippedConfig()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    char* env = nullptr;
    size_t envLen = 0;
    if (_dupenv_s(&env, &envLen, "TT2VR_SOURCE_DIR") == 0 && env != nullptr) {
        const fs::path p = fs::path(env) / "config" / "tt2vr.toml";
        free(env);
        if (fs::exists(p, ec)) return p;
    }
    wchar_t exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const fs::path starts[] = {fs::current_path(ec), fs::path(std::wstring(exe, n)).parent_path()};
    for (fs::path dir : starts) {
        for (int depth = 0; depth < 12 && !dir.empty(); ++depth) {
            const fs::path p = dir / "config" / "tt2vr.toml";
            if (fs::exists(p, ec)) return p;
            const fs::path parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------- hotkeys

TEST_CASE("ParseHotkey valid inputs")
{
    SUBCASE("plain keys")
    {
        CHECK(SameHotkey(*ParseHotkey("F11"), Hotkey{0x7A}));
        CHECK(SameHotkey(*ParseHotkey("F1"), Hotkey{0x70}));
        CHECK(SameHotkey(*ParseHotkey("F24"), Hotkey{0x87}));
        CHECK(SameHotkey(*ParseHotkey("A"), Hotkey{0x41}));
        CHECK(SameHotkey(*ParseHotkey("Z"), Hotkey{0x5A}));
        CHECK(SameHotkey(*ParseHotkey("0"), Hotkey{0x30}));
        CHECK(SameHotkey(*ParseHotkey("9"), Hotkey{0x39}));
        CHECK(SameHotkey(*ParseHotkey("Num0"), Hotkey{0x60}));
        CHECK(SameHotkey(*ParseHotkey("Num9"), Hotkey{0x69}));
        CHECK(SameHotkey(*ParseHotkey("Insert"), Hotkey{0x2D}));
        CHECK(SameHotkey(*ParseHotkey("Delete"), Hotkey{0x2E}));
        CHECK(SameHotkey(*ParseHotkey("Home"), Hotkey{0x24}));
        CHECK(SameHotkey(*ParseHotkey("End"), Hotkey{0x23}));
        CHECK(SameHotkey(*ParseHotkey("PageUp"), Hotkey{0x21}));
        CHECK(SameHotkey(*ParseHotkey("PageDown"), Hotkey{0x22}));
        CHECK(SameHotkey(*ParseHotkey("Pause"), Hotkey{0x13}));
        CHECK(SameHotkey(*ParseHotkey("ScrollLock"), Hotkey{0x91}));
    }
    SUBCASE("every function key")
    {
        for (unsigned n = 1; n <= 24; ++n) {
            const auto hk = ParseHotkey("F" + std::to_string(n));
            REQUIRE(hk.has_value());
            CHECK(hk->vk == 0x70 + n - 1);
        }
    }
    SUBCASE("modifiers")
    {
        CHECK(SameHotkey(*ParseHotkey("ctrl+F11"), Hotkey{0x7A, true, false, false}));
        CHECK(SameHotkey(*ParseHotkey("Shift+F11"), Hotkey{0x7A, false, true, false}));
        CHECK(SameHotkey(*ParseHotkey("Alt+P"), Hotkey{0x50, false, false, true}));
        CHECK(SameHotkey(*ParseHotkey("Ctrl + Shift + F12"), Hotkey{0x7B, true, true, false}));
        CHECK(SameHotkey(*ParseHotkey("F12+Alt+Ctrl+Shift"), Hotkey{0x7B, true, true, true}));
        CHECK(SameHotkey(*ParseHotkey("control+a"), Hotkey{0x41, true, false, false}));
    }
    SUBCASE("case and whitespace")
    {
        CHECK(SameHotkey(*ParseHotkey("  f11  "), Hotkey{0x7A}));
        CHECK(SameHotkey(*ParseHotkey("PAGEUP"), Hotkey{0x21}));
        CHECK(SameHotkey(*ParseHotkey("scrolllock"), Hotkey{0x91}));
        CHECK(SameHotkey(*ParseHotkey("CTRL+SHIFT+num5"), Hotkey{0x65, true, true, false}));
    }
    SUBCASE("none and empty disable the hotkey")
    {
        for (const char* s : {"none", "None", "NONE", "", "   "}) {
            const auto hk = ParseHotkey(s);
            REQUIRE(hk.has_value());
            CHECK_FALSE(hk->Valid());
            CHECK(SameHotkey(*hk, Hotkey{}));
        }
    }
}

TEST_CASE("ParseHotkey invalid inputs")
{
    CHECK_FALSE(ParseHotkey("F25").has_value());
    CHECK_FALSE(ParseHotkey("F0").has_value());
    CHECK_FALSE(ParseHotkey("F011").has_value());
    CHECK_FALSE(ParseHotkey("Banana").has_value());
    CHECK_FALSE(ParseHotkey("Ctrl+Banana").has_value());
    CHECK_FALSE(ParseHotkey("Num10").has_value());
    CHECK_FALSE(ParseHotkey("AB").has_value());
    CHECK_FALSE(ParseHotkey("F10+F11").has_value()); // two keys
    CHECK_FALSE(ParseHotkey("A+A").has_value());
    CHECK_FALSE(ParseHotkey("Ctrl+Ctrl+F1").has_value()); // duplicate modifier
    CHECK_FALSE(ParseHotkey("Ctrl").has_value()); // modifier only
    CHECK_FALSE(ParseHotkey("Ctrl+Shift").has_value());
    CHECK_FALSE(ParseHotkey("Ctrl+").has_value());
    CHECK_FALSE(ParseHotkey("+F1").has_value());
    CHECK_FALSE(ParseHotkey("Ctrl++F1").has_value());
    CHECK_FALSE(ParseHotkey("Ctrl+none").has_value());
}

TEST_CASE("FormatHotkey and round trip")
{
    CHECK(FormatHotkey(Hotkey{}) == "none");
    CHECK(FormatHotkey(Hotkey{0x7A}) == "F11");
    CHECK(FormatHotkey(Hotkey{0x7A, true, true, false}) == "Ctrl+Shift+F11");
    CHECK(FormatHotkey(Hotkey{0x50, true, true, true}) == "Ctrl+Shift+Alt+P");
    CHECK(FormatHotkey(Hotkey{0x65}) == "Num5");
    CHECK(FormatHotkey(Hotkey{0x21}) == "PageUp");

    for (const char* s : {"F1", "F24", "Ctrl+F11", "Shift+F11", "Alt+P", "Ctrl+Shift+Alt+Num7", "Insert", "Delete", "Home",
                          "End", "PageUp", "PageDown", "Pause", "ScrollLock", "0", "9", "A", "Z", "none"}) {
        const auto hk = ParseHotkey(s);
        REQUIRE(hk.has_value());
        CHECK(FormatHotkey(*hk) == s);
        const auto again = ParseHotkey(FormatHotkey(*hk));
        REQUIRE(again.has_value());
        CHECK(SameHotkey(*hk, *again));
    }
}

TEST_CASE("Hotkey::Matches uses exact modifiers")
{
    const Hotkey plain{0x7A};
    const Hotkey shifted{0x7A, false, true, false};
    CHECK(plain.Matches(0x7A, false, false, false));
    CHECK_FALSE(plain.Matches(0x7A, false, true, false));
    CHECK_FALSE(plain.Matches(0x7A, true, false, false));
    CHECK_FALSE(plain.Matches(0x7A, false, false, true));
    CHECK_FALSE(plain.Matches(0x79, false, false, false));

    CHECK(shifted.Matches(0x7A, false, true, false));
    CHECK_FALSE(shifted.Matches(0x7A, false, false, false));
    CHECK_FALSE(shifted.Matches(0x7A, true, true, false));

    const Hotkey disabled{};
    CHECK_FALSE(disabled.Matches(0, false, false, false));
    CHECK_FALSE(disabled.Matches(0x7A, false, false, false));
}

// ---------------------------------------------------------------- config

TEST_CASE("ParseConfig empty text gives defaults without warnings")
{
    const auto r = ParseConfig("");
    CHECK(r.warnings.empty());
    CHECK(SameConfig(r.config, Config{}));
    CHECK(r.config.log.level == "info");
    CHECK(r.config.hotkeys.texture_dump.shift);
    CHECK(r.config.hotkeys.texture_dump.vk == 0x7A);

    const auto r2 = ParseConfig("# only a comment\n\n");
    CHECK(r2.warnings.empty());
}

TEST_CASE("ParseConfig full valid file")
{
    const auto r = ParseConfig(R"(
[log]
level = "DEBUG"
verbose_frames = true
perf_interval_s = 30

[hotkeys]
overlay = "Ctrl+F1"
frame_trace = "none"
texture_dump = "alt + Num3"
renderdoc_capture = "Pause"
cursor = "Ctrl+M"
recenter = "Home"

[overlay]
visible = false
show_cursor = true

[camera]
helmet_follows_view = false
horizon_lock = 40
shake = 25

[xr]
enabled = false
resolution_scale = 1.25
desktop_vsync_off = false
desktop_mirror = false
menu_screen = false
screen_distance = 3
screen_width = 1.5
force_render_size = false

[capture]
dir = "C:/caps"
renderdoc_dir = "C:/dev/tt2vr-re/rdc"
max_textures = 100
include_depth = false
cb_floats = 32

[commands]
enabled = false
poll_frames = 30

[debug]
fake_exe_sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
input_log = true
)");
    CHECK(r.warnings.empty());
    const Config& c = r.config;
    CHECK(c.log.level == "debug");
    CHECK(c.log.verbose_frames);
    CHECK(c.log.perf_interval_s == 30);
    CHECK(SameHotkey(c.hotkeys.overlay, Hotkey{0x70, true, false, false}));
    CHECK_FALSE(c.hotkeys.frame_trace.Valid());
    CHECK(SameHotkey(c.hotkeys.texture_dump, Hotkey{0x63, false, false, true}));
    CHECK(SameHotkey(c.hotkeys.renderdoc_capture, Hotkey{0x13}));
    CHECK(SameHotkey(c.hotkeys.cursor, Hotkey{0x4D, true, false, false}));
    CHECK(c.overlay.show_cursor);
    CHECK_FALSE(c.overlay.visible);
    CHECK_FALSE(c.camera.helmet_follows_view);
    CHECK(c.camera.horizon_lock == doctest::Approx(40.0));
    CHECK(c.camera.shake == doctest::Approx(25.0));
    CHECK(SameHotkey(c.hotkeys.recenter, Hotkey{0x24}));
    CHECK_FALSE(c.xr.enabled);
    CHECK(c.xr.resolution_scale == doctest::Approx(1.25));
    CHECK_FALSE(c.xr.desktop_vsync_off);
    CHECK_FALSE(c.xr.desktop_mirror);
    CHECK_FALSE(c.xr.menu_screen);
    CHECK(c.xr.screen_distance == doctest::Approx(3.0));
    CHECK(c.xr.screen_width == doctest::Approx(1.5));
    CHECK_FALSE(c.xr.force_render_size);
    CHECK(c.capture.dir == "C:/caps");
    CHECK(c.capture.renderdoc_dir == "C:/dev/tt2vr-re/rdc");
    CHECK(c.capture.max_textures == 100);
    CHECK_FALSE(c.capture.include_depth);
    CHECK(c.capture.cb_floats == 32);
    CHECK_FALSE(c.commands.enabled);
    CHECK(c.commands.poll_frames == 30);
    CHECK(c.debug.fake_exe_sha256 == "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    CHECK(c.debug.input_log);
}

TEST_CASE("ParseConfig syntax error gives defaults and a warning with position")
{
    const auto r = ParseConfig("[log\nlevel = \"debug\"\n");
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0].find("line") != std::string::npos);
    CHECK(r.warnings[0].find("column") != std::string::npos);
    CHECK(SameConfig(r.config, Config{}));

    const auto r2 = ParseConfig("[capture]\nmax_textures = 5\nbroken = = 3\n");
    CHECK(r2.warnings.size() == 1);
    CHECK(SameConfig(r2.config, Config{})); // nothing partially applied
}

TEST_CASE("ParseConfig unknown tables and keys warn and are ignored")
{
    const auto r = ParseConfig("[log]\nlevl = \"debug\"\nlevel = \"warn\"\n[nonsense]\nx = 1\n[capture]\nfoo = 1\n");
    CHECK(r.warnings.size() == 3);
    CHECK(ContainsWarning(r, "levl"));
    CHECK(ContainsWarning(r, "nonsense"));
    CHECK(ContainsWarning(r, "foo"));
    CHECK(r.config.log.level == "warn");

    const auto top = ParseConfig("stray = 1\n");
    CHECK(top.warnings.size() == 1);
    CHECK(ContainsWarning(top, "stray"));
}

TEST_CASE("ParseConfig wrong types keep the default and warn")
{
    const auto r = ParseConfig(R"(
[log]
level = 3
verbose_frames = "yes"
[hotkeys]
overlay = 5
[overlay]
visible = 1
[capture]
dir = 7
max_textures = "many"
include_depth = 0
cb_floats = 1.5
[commands]
enabled = "true"
poll_frames = true
[debug]
fake_exe_sha256 = 1
)");
    CHECK(r.warnings.size() == 11);
    CHECK(SameConfig(r.config, Config{}));
}

TEST_CASE("ParseConfig invalid values keep the default and warn")
{
    const auto r = ParseConfig("[log]\nlevel = \"loud\"\n[hotkeys]\nframe_trace = \"Ctrl+Banana\"\n");
    CHECK(r.warnings.size() == 2);
    CHECK(ContainsWarning(r, "loud"));
    CHECK(ContainsWarning(r, "Banana"));
    CHECK(r.config.log.level == "info");
    CHECK(SameHotkey(r.config.hotkeys.frame_trace, Config{}.hotkeys.frame_trace));
}

TEST_CASE("ParseConfig clamps out-of-range numbers")
{
    SUBCASE("too high")
    {
        const auto r = ParseConfig("[capture]\nmax_textures = 5000\ncb_floats = 99999\n[commands]\npoll_frames = 100000\n");
        CHECK(r.warnings.size() == 3);
        CHECK(r.config.capture.max_textures == 1024);
        CHECK(r.config.capture.cb_floats == 1024);
        CHECK(r.config.commands.poll_frames == 600);
    }
    SUBCASE("too low")
    {
        const auto r = ParseConfig("[capture]\nmax_textures = 0\ncb_floats = -4\n[commands]\npoll_frames = -1\n");
        CHECK(r.warnings.size() == 3);
        CHECK(r.config.capture.max_textures == 1);
        CHECK(r.config.capture.cb_floats == 0);
        CHECK(r.config.commands.poll_frames == 1);
    }
    SUBCASE("boundaries are accepted silently")
    {
        const auto r = ParseConfig("[capture]\nmax_textures = 1\ncb_floats = 0\n[commands]\npoll_frames = 600\n");
        CHECK(r.warnings.empty());
        CHECK(r.config.capture.max_textures == 1);
        CHECK(r.config.capture.cb_floats == 0);
        CHECK(r.config.commands.poll_frames == 600);
    }
    SUBCASE("huge 64-bit value")
    {
        const auto r = ParseConfig("[capture]\nmax_textures = 9223372036854775807\n");
        CHECK(r.warnings.size() == 1);
        CHECK(r.config.capture.max_textures == 1024);
    }
}

TEST_CASE("ParseConfig [xr] resolution_scale")
{
    SUBCASE("integer accepted")
    {
        const auto r = ParseConfig("[xr]\nresolution_scale = 1\n");
        CHECK(r.warnings.empty());
        CHECK(r.config.xr.resolution_scale == doctest::Approx(1.0));
    }
    SUBCASE("clamped")
    {
        const auto hi = ParseConfig("[xr]\nresolution_scale = 3.5\n");
        CHECK(hi.warnings.size() == 1);
        CHECK(hi.config.xr.resolution_scale == doctest::Approx(2.0));
        const auto lo = ParseConfig("[xr]\nresolution_scale = 0.1\n");
        CHECK(lo.warnings.size() == 1);
        CHECK(lo.config.xr.resolution_scale == doctest::Approx(0.25));
    }
    SUBCASE("wrong type and NaN keep the default")
    {
        const auto t = ParseConfig("[xr]\nresolution_scale = \"big\"\n");
        CHECK(t.warnings.size() == 1);
        CHECK(t.config.xr.resolution_scale == doctest::Approx(1.0));
        const auto n = ParseConfig("[xr]\nresolution_scale = nan\n");
        CHECK(n.warnings.size() == 1);
        CHECK(n.config.xr.resolution_scale == doctest::Approx(1.0));
    }
}

TEST_CASE("ParseConfig [xr] render_scale and fsr_sharpness")
{
    const auto d = ParseConfig("");
    CHECK(d.config.xr.render_scale == doctest::Approx(1.0));
    CHECK(d.config.xr.fsr_sharpness == doctest::Approx(0.2));
    const auto r = ParseConfig("[xr]\nrender_scale = 0.75\nfsr_sharpness = 0.5\n");
    CHECK(r.warnings.empty());
    CHECK(r.config.xr.render_scale == doctest::Approx(0.75));
    CHECK(r.config.xr.fsr_sharpness == doctest::Approx(0.5));
    const auto c = ParseConfig("[xr]\nrender_scale = 1.5\nfsr_sharpness = -1\n");
    CHECK(c.warnings.size() == 2);
    CHECK(c.config.xr.render_scale == doctest::Approx(1.0));
    CHECK(c.config.xr.fsr_sharpness == doctest::Approx(0.0));
    const auto lo = ParseConfig("[xr]\nrender_scale = 0.2\n");
    CHECK(lo.warnings.size() == 1);
    CHECK(lo.config.xr.render_scale == doctest::Approx(0.5));
}

TEST_CASE("ParseConfig [hud]")
{
    const auto r = ParseConfig(R"(
[hud]
enabled = false
pixels = 100
aspect = 1.5
spread = 1.6
distance = 1.2
width = 0.7
height = -0.2
tilt = 15
follow = "head"
desktop = false
)");
    CHECK(ContainsWarning(r, "pixels"));
    CHECK(r.config.hud.enabled == false);
    CHECK(r.config.hud.pixels == 512);
    CHECK(r.config.hud.aspect == doctest::Approx(1.5));
    CHECK(r.config.hud.spread == doctest::Approx(1.6));
    CHECK(r.config.hud.distance == doctest::Approx(1.2));
    CHECK(r.config.hud.width == doctest::Approx(0.7));
    CHECK(r.config.hud.height == doctest::Approx(-0.2));
    CHECK(r.config.hud.tilt == doctest::Approx(15.0));
    CHECK(r.config.hud.follow == "head");
    CHECK(r.config.hud.desktop == false);
    const auto bad = ParseConfig("[hud]\nfollow = \"wall\"\n");
    CHECK(ContainsWarning(bad, "follow"));
    CHECK(bad.config.hud.follow == "seat");
}

TEST_CASE("ParseConfig fake_exe_sha256 validation")
{
    SUBCASE("bad hex is ignored")
    {
        const auto r = ParseConfig("[debug]\nfake_exe_sha256 = \"xyz\"\n");
        CHECK(r.warnings.size() == 1);
        CHECK(r.config.debug.fake_exe_sha256.empty());
    }
    SUBCASE("wrong length is ignored")
    {
        const auto r = ParseConfig("[debug]\nfake_exe_sha256 = \"abcdef\"\n");
        CHECK(r.warnings.size() == 1);
        CHECK(r.config.debug.fake_exe_sha256.empty());
    }
    SUBCASE("64 chars but not hex is ignored")
    {
        const auto r = ParseConfig("[debug]\nfake_exe_sha256 = \"" + std::string(64, 'g') + "\"\n");
        CHECK(r.warnings.size() == 1);
        CHECK(r.config.debug.fake_exe_sha256.empty());
    }
    SUBCASE("upper case is lower-cased")
    {
        const std::string upper(64, 'A');
        const auto r = ParseConfig("[debug]\nfake_exe_sha256 = \"" + upper + "\"\n");
        CHECK(r.warnings.empty());
        CHECK(r.config.debug.fake_exe_sha256 == std::string(64, 'a'));
    }
    SUBCASE("empty is fine")
    {
        const auto r = ParseConfig("[debug]\nfake_exe_sha256 = \"\"\n");
        CHECK(r.warnings.empty());
    }
}

TEST_CASE("LoadConfig")
{
    namespace fs = std::filesystem;
    SUBCASE("missing file")
    {
        const fs::path missing = fs::temp_directory_path() / "tt2vr_test_no_such_config.toml";
        const auto r = core::LoadConfig(missing);
        REQUIRE(r.warnings.size() == 1);
        CHECK(r.warnings[0].find("config file not found:") == 0);
        CHECK(SameConfig(r.config, Config{}));
    }
    SUBCASE("existing file, with a UTF-8 BOM")
    {
        const fs::path file = fs::temp_directory_path() / "tt2vr_test_config.toml";
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << "\xEF\xBB\xBF[overlay]\nvisible = false\n";
        }
        const auto r = core::LoadConfig(file);
        fs::remove(file);
        CHECK(r.warnings.empty());
        CHECK_FALSE(r.config.overlay.visible);
    }
}

TEST_CASE("user.toml: FormatUserToml parsed on top of a base overrides only its keys")
{
    const auto base = ParseConfig("[camera]\nhorizon_lock = 80\nshake = 60\n[xr]\nstereo = false\n");
    REQUIRE(base.warnings.empty());
    const std::string text = core::FormatUserToml({{"camera", "shake", 25.0}, {"camera", "seat_up", -0.03},
        {"xr", "world_scale", 1.25}, {"xr", "occlusion", 0.0, true}});
    const auto r = ParseConfig(text, base.config);
    CHECK(r.warnings.empty());
    CHECK_FALSE(r.config.xr.occlusion);
    CHECK(r.config.camera.horizon_lock == doctest::Approx(80.0)); // from the base
    CHECK(r.config.camera.shake == doctest::Approx(25.0));
    CHECK(r.config.camera.seat_up == doctest::Approx(-0.03));
    CHECK(r.config.xr.world_scale == doctest::Approx(1.25));
    CHECK_FALSE(r.config.xr.stereo); // from the base
    // A broken user file keeps the base.
    const auto broken = ParseConfig("[camera\nshake = ", base.config);
    CHECK_FALSE(broken.warnings.empty());
    CHECK(broken.config.camera.shake == doctest::Approx(60.0));
}

TEST_CASE("SetConfig / GetConfig")
{
    const Config before = core::GetConfig();
    Config c;
    c.capture.max_textures = 7;
    core::SetConfig(c);
    CHECK(core::GetConfig().capture.max_textures == 7);
    core::SetConfig(before);
    CHECK(SameConfig(core::GetConfig(), before));
}

TEST_CASE("the shipped config/tt2vr.toml parses cleanly and equals the defaults")
{
    const auto path = FindShippedConfig();
    if (path.empty()) {
        MESSAGE("config/tt2vr.toml not found (set TT2VR_SOURCE_DIR); skipping");
        return;
    }
    const auto r = core::LoadConfig(path);
    for (const auto& w : r.warnings) MESSAGE("warning: " << w);
    CHECK(r.warnings.empty());
    CHECK(SameConfig(r.config, Config{}));
}
