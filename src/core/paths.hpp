#pragma once

#include <filesystem>

namespace core {

// Folder containing TT2.exe.
std::filesystem::path GameDir();
// <game>\tt2vr\ (mod files, logs, captures).
std::filesystem::path ModDir();
// [capture] dir, resolved against ModDir() when relative; created on first use.
std::filesystem::path CaptureDir();
// [capture] renderdoc_dir (CaptureDir() when empty); created on first use.
std::filesystem::path RenderDocDir();
// <game>\tt2vr\commands\ (remote requests); created on first use.
std::filesystem::path CommandsDir();

} // namespace core
