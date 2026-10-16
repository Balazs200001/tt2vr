// SHA-256 through Windows CNG (bcrypt).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace core {

// Lower-case 64 hex chars; "" on failure.
std::string Sha256Hex(std::span<const std::uint8_t> data);
// Streams the file in 1 MiB chunks; nullopt on error.
std::optional<std::string> Sha256FileHex(const std::filesystem::path& file);

} // namespace core
