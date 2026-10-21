// tt2vr\user.toml (PLAN Decisions log 2026-10-21): the settings changed at runtime (overlay, hotkeys, later the VR
// Options tab), loaded after tt2vr.toml and overriding it (core::LoadConfig with a base). The mod rewrites the whole
// file; deploys and updates never touch it, so hand edits of tt2vr.toml stay as they are.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace core {

struct UserValue {
    std::string section, key;
    double value = 0.0;
};

// <game>\tt2vr\user.toml
std::filesystem::path UserTomlPath();
// Header comment, then one [section] per distinct section in first-seen order, `key = value` lines. Pure.
std::string FormatUserToml(const std::vector<UserValue>& values);
// Writes the file through a temporary and a rename. False + `error` on failure; never throws.
bool WriteUserToml(const std::filesystem::path& file, const std::vector<UserValue>& values, std::string& error);

} // namespace core
