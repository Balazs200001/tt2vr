// IDA-style byte pattern scanner (CLAUDE.md rule 3: engine addresses come from pattern scans).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace core {

struct Pattern {
    std::vector<std::uint8_t> bytes;
    std::vector<bool> mask; // true = byte must match; false = wildcard
    size_t size() const { return bytes.size(); }
};

// "48 8B 05 ?? ?? ?? ?? E8 ?": whitespace-separated tokens, each a 2-digit hex byte (either case) or "?"/"??".
// nullopt for: empty, bad token (e.g. "4", "GG", "???", "4?"), or all wildcards.
std::optional<Pattern> ParsePattern(std::string_view ida);

// Offset of the first match at or after `start`, or nullopt.
std::optional<size_t> FindPattern(std::span<const std::uint8_t> haystack, const Pattern& p, size_t start = 0);

// Every match offset (overlapping matches included), up to `limit`.
std::vector<size_t> FindAllPatterns(std::span<const std::uint8_t> haystack, const Pattern& p, size_t limit = 16);

// Bytes of a named PE section (e.g. ".text") of a loaded module; empty span if not found.
// Reads the PE headers in memory. Windows only.
std::span<const std::uint8_t> ModuleSection(void* moduleBase, const char* sectionName);

} // namespace core
