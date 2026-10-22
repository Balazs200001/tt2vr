#include "core/user_toml.hpp"

#include "core/paths.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <fstream>
#include <system_error>

namespace core {

std::filesystem::path UserTomlPath()
{
    return ModDir() / L"user.toml";
}

std::string FormatUserToml(const std::vector<UserValue>& values)
{
    std::string out = "# tt2vr user settings: written by the mod when you change a setting while the game runs (overlay,\n"
                      "# hotkeys). Loaded after tt2vr.toml; every value here overrides the one there. Delete a line (or the\n"
                      "# file) to go back to tt2vr.toml's value.\n";
    std::vector<std::string> sections;
    for (const UserValue& v : values) {
        if (std::find(sections.begin(), sections.end(), v.section) == sections.end()) {
            sections.push_back(v.section);
        }
    }
    for (const std::string& section : sections) {
        out += fmt::format("\n[{}]\n", section);
        for (const UserValue& v : values) {
            if (v.section == section) {
                out += v.boolean ? fmt::format("{} = {}\n", v.key, v.value != 0.0)
                                 : fmt::format("{} = {:.6g}\n", v.key, v.value);
            }
        }
    }
    return out;
}

bool WriteUserToml(const std::filesystem::path& file, const std::vector<UserValue>& values, std::string& error)
{
    try {
        const std::string text = FormatUserToml(values);
        auto tmp = file;
        tmp += L".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out << text;
            if (!out) {
                error = "cannot write " + tmp.string();
                return false;
            }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, file, ec); // replaces an existing file
        if (ec) {
            error = "cannot replace " + file.string() + ": " + ec.message();
            return false;
        }
        return true;
    }
    catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    catch (...) {
        error = "unknown error";
        return false;
    }
}

} // namespace core
