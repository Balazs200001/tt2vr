#pragma once

#include <filesystem>

namespace core {

// Folder containing TT2.exe.
std::filesystem::path GameDir();
// <game>\tt2vr\ (mod files, logs, captures).
std::filesystem::path ModDir();

} // namespace core
