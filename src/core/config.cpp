#include "core/config.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <sstream>

namespace core {

namespace {

// ---------------------------------------------------------------- hotkeys

struct KeyName {
    const char* name; // canonical spelling (also the lookup key, compared case-insensitively)
    unsigned vk;
};

constexpr KeyName kNamedKeys[] = {
    {"Insert", 0x2D}, {"Delete", 0x2E}, {"Home", 0x24},   {"End", 0x23},
    {"PageUp", 0x21}, {"PageDown", 0x22}, {"Pause", 0x13}, {"ScrollLock", 0x91},
};

std::string Lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view Trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool AllDigits(std::string_view s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// Lower-case key name to virtual-key code, 0 if unknown.
unsigned KeyFromName(const std::string& lower)
{
    if (lower.size() == 1) {
        const char c = lower[0];
        if (c >= 'a' && c <= 'z') return static_cast<unsigned>(c - 'a') + 0x41;
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0') + 0x30;
    }
    if (lower.size() >= 2 && lower[0] == 'f' && AllDigits(std::string_view(lower).substr(1))) {
        if (lower.size() <= 3) {
            const int n = std::stoi(lower.substr(1));
            if (n >= 1 && n <= 24 && !(lower.size() == 3 && lower[1] == '0')) return 0x70u + static_cast<unsigned>(n - 1);
        }
        return 0;
    }
    if (lower.size() == 4 && lower.compare(0, 3, "num") == 0 && lower[3] >= '0' && lower[3] <= '9') {
        return 0x60u + static_cast<unsigned>(lower[3] - '0');
    }
    for (const KeyName& k : kNamedKeys) {
        if (lower == Lower(k.name)) return k.vk;
    }
    return 0;
}

// Canonical name for a virtual-key code, empty if we have none.
std::string NameFromKey(unsigned vk)
{
    if (vk >= 0x41 && vk <= 0x5A) return std::string(1, static_cast<char>('A' + (vk - 0x41)));
    if (vk >= 0x30 && vk <= 0x39) return std::string(1, static_cast<char>('0' + (vk - 0x30)));
    if (vk >= 0x70 && vk <= 0x87) return "F" + std::to_string(vk - 0x70 + 1);
    if (vk >= 0x60 && vk <= 0x69) return "Num" + std::to_string(vk - 0x60);
    for (const KeyName& k : kNamedKeys) {
        if (k.vk == vk) return k.name;
    }
    return {};
}

// ---------------------------------------------------------------- config parsing

using Warnings = std::vector<std::string>;

std::string Where(const toml::node& node)
{
    const auto& b = node.source().begin;
    return " (line " + std::to_string(b.line) + ", column " + std::to_string(b.column) + ")";
}

// One [section] being read. Reports problems as "[section] key: message (line L, column C)".
class Section {
public:
    Section(std::string_view name, Warnings& warnings) : m_name(name), m_warnings(warnings) {}

    void Warn(std::string_view key, std::string_view msg, const toml::node& node) const
    {
        m_warnings.push_back("[" + m_name + "] " + std::string(key) + ": " + std::string(msg) + Where(node));
    }

    void Bool(std::string_view key, const toml::node& node, bool& out) const
    {
        if (const auto* v = node.as_boolean()) {
            out = v->get();
        } else {
            Warn(key, "expected true or false, keeping default", node);
        }
    }

    void String(std::string_view key, const toml::node& node, std::string& out) const
    {
        if (const auto* v = node.as_string()) {
            out = v->get();
        } else {
            Warn(key, "expected a string, keeping default", node);
        }
    }

    void Int(std::string_view key, const toml::node& node, int& out, int lo, int hi) const
    {
        const auto* v = node.as_integer();
        if (v == nullptr) {
            Warn(key, "expected an integer, keeping default", node);
            return;
        }
        const std::int64_t value = v->get();
        const std::int64_t clamped = std::clamp<std::int64_t>(value, lo, hi);
        if (clamped != value) {
            Warn(key,
                 "value " + std::to_string(value) + " out of range " + std::to_string(lo) + ".." + std::to_string(hi) +
                     ", clamped to " + std::to_string(clamped),
                 node);
        }
        out = static_cast<int>(clamped);
    }

    void Float(std::string_view key, const toml::node& node, float& out, double lo, double hi) const
    {
        double value = 0.0;
        if (const auto* f = node.as_floating_point()) {
            value = f->get();
        } else if (const auto* i = node.as_integer()) {
            value = static_cast<double>(i->get());
        } else {
            Warn(key, "expected a number, keeping default", node);
            return;
        }
        if (!(value == value)) { // NaN
            Warn(key, "expected a number, keeping default", node);
            return;
        }
        const double clamped = std::clamp(value, lo, hi);
        if (clamped != value) {
            Warn(key,
                 "value " + std::to_string(value) + " out of range " + std::to_string(lo) + ".." + std::to_string(hi) +
                     ", clamped to " + std::to_string(clamped),
                 node);
        }
        out = static_cast<float>(clamped);
    }

    void Key(std::string_view key, const toml::node& node, Hotkey& out) const
    {
        const auto* v = node.as_string();
        if (v == nullptr) {
            Warn(key, "expected a string like \"Ctrl+F11\", keeping default", node);
            return;
        }
        if (const auto hk = ParseHotkey(v->get())) {
            out = *hk;
        } else {
            Warn(key, "invalid hotkey \"" + v->get() + "\", keeping default", node);
        }
    }

private:
    std::string m_name;
    Warnings& m_warnings;
};

bool IsHex64(std::string_view s)
{
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

void ReadLog(const toml::table& t, Config::Log& out, Warnings& w)
{
    const Section s("log", w);
    for (const auto& [k, node] : t) {
        const std::string_view key = k.str();
        if (key == "level") {
            std::string level;
            if (const auto* v = node.as_string()) {
                level = Lower(v->get());
                if (level == "trace" || level == "debug" || level == "info" || level == "warn" || level == "error") {
                    out.level = level;
                } else {
                    s.Warn(key, "unknown level \"" + v->get() + "\" (use trace, debug, info, warn or error), keeping default", node);
                }
            } else {
                s.Warn(key, "expected a string, keeping default", node);
            }
        } else if (key == "verbose_frames") {
            s.Bool(key, node, out.verbose_frames);
        } else if (key == "perf_interval_s") {
            s.Int(key, node, out.perf_interval_s, 0, 600);
        } else {
            s.Warn(key, "unknown key, ignored", node);
        }
    }
}

void ReadHotkeys(const toml::table& t, Config::Hotkeys& out, Warnings& w)
{
    const Section s("hotkeys", w);
    for (const auto& [k, node] : t) {
        const std::string_view key = k.str();
        if (key == "overlay") s.Key(key, node, out.overlay);
        else if (key == "frame_trace") s.Key(key, node, out.frame_trace);
        else if (key == "texture_dump") s.Key(key, node, out.texture_dump);
        else if (key == "renderdoc_capture") s.Key(key, node, out.renderdoc_capture);
        else if (key == "cursor") s.Key(key, node, out.cursor);
        else if (key == "recenter") s.Key(key, node, out.recenter);
        else s.Warn(key, "unknown key, ignored", node);
    }
}

void ReadOverlay(const toml::table& t, Config::Overlay& out, Warnings& w)
{
    const Section s("overlay", w);
    for (const auto& [k, node] : t) {
        const std::string_view key = k.str();
        if (key == "visible") s.Bool(key, node, out.visible);
        else if (key == "show_cursor") s.Bool(key, node, out.show_cursor);
        else s.Warn(key, "unknown key, ignored", node);
    }
}

void ReadCamera(const toml::table& t, Config::Camera& out, Warnings& w)
{
    const Section s("camera", w);
    for (const auto& [k, node] : t) {
        const std::string_view key = k.str();
        if (key == "helmet_follows_view") s.Bool(key, node, out.helmet_follows_view);
        else s.Warn(key, "unknown key, ignored", node);
    }
}

void ReadXr(const toml::table& t, Config::Xr& out, Warnings& w)
{
    const Section s("xr", w);
    for (const auto& [k, node] : t) {
        const std::string_view key = k.str();
        if (key == "enabled") s.Bool(key, node, out.enabled);
        else if (key == "resolution_scale") s.Float(key, node, out.resolution_scale, 0.25, 2.0);
        else if (key == "desktop_vsync_off") s.Bool(key, node, out.desktop_vsync_off);
        else if (key == "desktop_mirror") s.Bool(key, node, out.desktop_mirror);
        else if (key == "menu_screen") s.Bool(key, node, out.menu_screen);
        else if (key == "screen_distance") s.Float(key, node, out.screen_distance, 0.5, 10.0);
        else if (key == "screen_width") s.Float(key, node, out.screen_width, 0.25, 10.0);
        else if (key == "force_render_size") s.Bool(key, node, out.force_render_size);
        else if (key == "stereo") s.Bool(key, node, out.stereo);
        else s.Warn(key, "unknown key, ignored", node);
    }
}

void ReadCapture(const toml::table& t, Config::Capture& out, Warnings& w)
{
    const Section s("capture", w);
    for (const auto& [k, node] : t) {
        const std::string_view key = k.str();
        if (key == "dir") s.String(key, node, out.dir);
        else if (key == "renderdoc_dir") s.String(key, node, out.renderdoc_dir);
        else if (key == "max_textures") s.Int(key, node, out.max_textures, 1, 1024);
        else if (key == "include_depth") s.Bool(key, node, out.include_depth);
        else if (key == "cb_floats") s.Int(key, node, out.cb_floats, 0, 1024);
        else s.Warn(key, "unknown key, ignored", node);
    }
}

void ReadCommands(const toml::table& t, Config::Commands& out, Warnings& w)
{
    const Section s("commands", w);
    for (const auto& [k, node] : t) {
        const std::string_view key = k.str();
        if (key == "enabled") s.Bool(key, node, out.enabled);
        else if (key == "poll_frames") s.Int(key, node, out.poll_frames, 1, 600);
        else s.Warn(key, "unknown key, ignored", node);
    }
}

void ReadDebug(const toml::table& t, Config::Debug& out, Warnings& w)
{
    const Section s("debug", w);
    for (const auto& [k, node] : t) {
        const std::string_view key = k.str();
        if (key == "fake_exe_sha256") {
            if (const auto* v = node.as_string()) {
                const std::string& text = v->get();
                if (text.empty() || IsHex64(text)) {
                    out.fake_exe_sha256 = Lower(text);
                } else {
                    s.Warn(key, "must be empty or 64 hex characters, ignored", node);
                }
            } else {
                s.Warn(key, "expected a string, keeping default", node);
            }
        } else if (key == "input_log") {
            s.Bool(key, node, out.input_log);
        } else {
            s.Warn(key, "unknown key, ignored", node);
        }
    }
}

void ReadTables(const toml::table& root, Config& cfg, Warnings& w)
{
    for (const auto& [k, node] : root) {
        const std::string_view name = k.str();
        const toml::table* t = node.as_table();
        if (t == nullptr) {
            w.push_back("top-level key \"" + std::string(name) + "\" is not inside a [table], ignored" + Where(node));
            continue;
        }
        if (name == "log") ReadLog(*t, cfg.log, w);
        else if (name == "hotkeys") ReadHotkeys(*t, cfg.hotkeys, w);
        else if (name == "overlay") ReadOverlay(*t, cfg.overlay, w);
        else if (name == "camera") ReadCamera(*t, cfg.camera, w);
        else if (name == "xr") ReadXr(*t, cfg.xr, w);
        else if (name == "capture") ReadCapture(*t, cfg.capture, w);
        else if (name == "commands") ReadCommands(*t, cfg.commands, w);
        else if (name == "debug") ReadDebug(*t, cfg.debug, w);
        else w.push_back("unknown table [" + std::string(name) + "], ignored" + Where(node));
    }
}

std::string SyntaxWarning(const toml::parse_error& err)
{
    const auto& b = err.source().begin;
    return "config syntax error at line " + std::to_string(b.line) + ", column " + std::to_string(b.column) + ": " +
           std::string(err.description()) + "; using defaults";
}

// Process-wide config. Published through an atomic pointer; old values are intentionally leaked
// (a few hundred bytes, only on repeated SetConfig) so references handed out stay valid.
const Config kDefaultConfig{};
std::atomic<const Config*> g_config{&kDefaultConfig};

} // namespace

std::optional<Hotkey> ParseHotkey(std::string_view text)
{
    try {
        text = Trim(text);
        if (text.empty() || Lower(text) == "none") return Hotkey{};

        Hotkey hk;
        bool haveKey = false;
        bool haveCtrl = false, haveShift = false, haveAlt = false;

        size_t pos = 0;
        while (true) {
            const size_t plus = text.find('+', pos);
            const std::string tok = Lower(Trim(text.substr(pos, plus == std::string_view::npos ? plus : plus - pos)));
            if (tok.empty()) return std::nullopt; // "F11+", "+F11", "ctrl++F11"

            if (tok == "ctrl" || tok == "control") {
                if (haveCtrl) return std::nullopt;
                haveCtrl = hk.ctrl = true;
            } else if (tok == "shift") {
                if (haveShift) return std::nullopt;
                haveShift = hk.shift = true;
            } else if (tok == "alt") {
                if (haveAlt) return std::nullopt;
                haveAlt = hk.alt = true;
            } else {
                const unsigned vk = KeyFromName(tok);
                if (vk == 0 || haveKey) return std::nullopt;
                hk.vk = vk;
                haveKey = true;
            }

            if (plus == std::string_view::npos) break;
            pos = plus + 1;
        }
        if (!haveKey) return std::nullopt; // modifier-only
        return hk;
    }
    catch (...) {
        return std::nullopt;
    }
}

std::string FormatHotkey(const Hotkey& hk)
{
    try {
        if (!hk.Valid()) return "none";
        std::string out;
        if (hk.ctrl) out += "Ctrl+";
        if (hk.shift) out += "Shift+";
        if (hk.alt) out += "Alt+";
        const std::string name = NameFromKey(hk.vk);
        out += name.empty() ? "vk" + std::to_string(hk.vk) : name; // unnamed codes only come from hand-built Hotkeys
        return out;
    }
    catch (...) {
        return "none";
    }
}

ConfigResult ParseConfig(std::string_view tomlText)
{
    ConfigResult result;
    try {
        // Skip a UTF-8 byte order mark (Notepad adds one).
        if (tomlText.size() >= 3 && tomlText.substr(0, 3) == "\xEF\xBB\xBF") tomlText.remove_prefix(3);

#if TOML_EXCEPTIONS
        toml::table root;
        try {
            root = toml::parse(tomlText);
        }
        catch (const toml::parse_error& err) {
            result.warnings.push_back(SyntaxWarning(err));
            return result;
        }
        ReadTables(root, result.config, result.warnings);
#else
        toml::parse_result parsed = toml::parse(tomlText);
        if (!parsed) {
            result.warnings.push_back(SyntaxWarning(parsed.error()));
            return result;
        }
        ReadTables(parsed.table(), result.config, result.warnings);
#endif
    }
    catch (const std::exception& e) {
        result = ConfigResult{};
        result.warnings.push_back(std::string("config could not be read: ") + e.what() + "; using defaults");
    }
    catch (...) {
        result = ConfigResult{};
        result.warnings.push_back("config could not be read (unknown error); using defaults");
    }
    return result;
}

ConfigResult LoadConfig(const std::filesystem::path& file)
{
    std::string shown;
    try {
        shown = file.string();
    }
    catch (...) {
        shown = "<unprintable path>";
    }

    try {
        std::error_code ec;
        if (!std::filesystem::exists(file, ec)) {
            ConfigResult r;
            r.warnings.push_back("config file not found: " + shown);
            return r;
        }
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            ConfigResult r;
            r.warnings.push_back("config file could not be opened: " + shown + "; using defaults");
            return r;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        if (in.bad()) {
            ConfigResult r;
            r.warnings.push_back("config file could not be read: " + shown + "; using defaults");
            return r;
        }
        return ParseConfig(ss.str());
    }
    catch (...) {
        ConfigResult r;
        r.warnings.push_back("config file could not be read: " + shown + "; using defaults");
        return r;
    }
}

namespace {
std::vector<std::string> g_configWarnings;
}

void SetConfigWarnings(std::vector<std::string> warnings)
{
    g_configWarnings = std::move(warnings);
}

const std::vector<std::string>& ConfigWarnings()
{
    return g_configWarnings;
}

const Config& GetConfig()
{
    return *g_config.load(std::memory_order_acquire);
}

void SetConfig(const Config& config)
{
    try {
        g_config.store(new Config(config), std::memory_order_release);
    }
    catch (...) {
        // Out of memory: keep the previous config.
    }
}

} // namespace core
