#include "core/commands.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

namespace core {
namespace {

std::string Lower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view Trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

std::string SafeName(std::string_view s)
{
    std::string out;
    for (char c : s.substr(0, 64)) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
        out += ok ? c : '_';
    }
    return out;
}

} // namespace

std::vector<Command> ParseCommands(std::string_view text)
{
    std::vector<Command> commands;
    while (!text.empty()) {
        const size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view() : text.substr(eol + 1);
        if (const size_t hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        const size_t space = line.find_first_of(" \t");
        const std::string word = Lower(line.substr(0, space));
        const std::string_view arg = space == std::string_view::npos ? std::string_view() : Trim(line.substr(space));
        Command c;
        c.text = std::string(line);
        if (word == "frametrace" || word == "trace") {
            c.kind = CommandKind::FrameTrace;
            c.arg = SafeName(arg); // optional frame count ("frametrace 2": two consecutive frames)
        }
        else if (word == "dump") {
            c.kind = CommandKind::TextureDump;
        }
        else if (word == "capture" || word == "renderdoc") {
            c.kind = CommandKind::RenderDocCapture;
            c.arg = SafeName(arg);
        }
        else if (word == "shaders") {
            c.kind = CommandKind::ShaderRegistry;
        }
        else if (word == "camera") {
            c.kind = CommandKind::Camera;
        }
        else if (word == "xrprobe") {
            c.kind = CommandKind::XrProbe;
        }
        else if (word == "leaktest") {
            c.kind = CommandKind::LeakTest;
            c.arg = Lower(arg); // row: taa (default), ssr, froxel
        }
        else if (word == "taareset") {
            c.kind = CommandKind::TaaReset;
        }
        else if (word == "convergence") {
            c.kind = CommandKind::Convergence;
        }
        else if (word == "shifttest") {
            c.kind = CommandKind::ShiftTest;
        }
        else if (word == "views") {
            c.kind = CommandKind::Views;
            c.arg = Lower(arg);
        }
        else if (word == "mvsave") {
            c.kind = CommandKind::MvSave;
        }
        else if (word == "hud") {
            c.kind = CommandKind::Hud;
        }
        else if (word == "freeze") {
            c.kind = CommandKind::Freeze;
            c.arg = Lower(arg);
        }
        else if (word == "cuttest") {
            c.kind = CommandKind::CutTest;
            c.arg = Lower(arg);
        }
        else if (word == "screenshot" || word == "screen") {
            c.kind = CommandKind::Screenshot;
            c.arg = SafeName(arg);
        }
        else if (word == "pad") {
            c.kind = CommandKind::Pad;
            c.arg = std::string(arg);
        }
        else if (word == "head") {
            c.kind = CommandKind::Head;
            c.arg = std::string(arg);
        }
        commands.push_back(std::move(c));
    }
    return commands;
}

std::vector<CommandFile> TakeCommandFiles(const std::filesystem::path& dir)
{
    std::vector<CommandFile> files;
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> entries;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".txt") {
            entries.push_back(entry);
        }
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        std::error_code e1, e2;
        return a.last_write_time(e1) < b.last_write_time(e2);
    });
    for (const auto& entry : entries) {
        std::ifstream in(entry.path(), std::ios::binary);
        if (!in) {
            continue; // still being written; next poll
        }
        std::stringstream ss;
        ss << in.rdbuf();
        in.close();
        if (!std::filesystem::remove(entry.path(), ec)) {
            continue;
        }
        files.push_back({entry.path(), ParseCommands(ss.str())});
    }
    return files;
}

void WriteDone(const std::filesystem::path& request, const std::vector<std::string>& results)
{
    auto done = request;
    done.replace_extension(".done");
    std::ofstream out(done, std::ios::trunc);
    for (const auto& r : results) {
        out << r << "\n";
    }
    if (!out) {
        TT2VR_LOG_WARN("commands: cannot write {}", done.string());
    }
}

} // namespace core
