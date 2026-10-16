#include "core/pattern.hpp"

#include <windows.h>

#include <cstring>

namespace core {

namespace {

int HexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool IsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Index of the first non-wildcard byte (ParsePattern guarantees one exists for parsed patterns).
size_t AnchorIndex(const Pattern& p)
{
    for (size_t i = 0; i < p.bytes.size(); ++i) {
        if (p.mask[i]) return i;
    }
    return p.bytes.size();
}

bool MatchesAt(const std::uint8_t* at, const Pattern& p)
{
    for (size_t i = 0; i < p.bytes.size(); ++i) {
        if (p.mask[i] && at[i] != p.bytes[i]) return false;
    }
    return true;
}

} // namespace

std::optional<Pattern> ParsePattern(std::string_view ida)
{
    Pattern p;
    bool anyFixed = false;
    size_t i = 0;
    while (i < ida.size()) {
        if (IsSpace(ida[i])) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < ida.size() && !IsSpace(ida[j])) ++j;
        const std::string_view tok = ida.substr(i, j - i);
        i = j;

        if (tok == "?" || tok == "??") {
            p.bytes.push_back(0);
            p.mask.push_back(false);
            continue;
        }
        if (tok.size() != 2) return std::nullopt;
        const int hi = HexValue(tok[0]);
        const int lo = HexValue(tok[1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        p.bytes.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
        p.mask.push_back(true);
        anyFixed = true;
    }
    if (p.bytes.empty() || !anyFixed) return std::nullopt;
    return p;
}

std::optional<size_t> FindPattern(std::span<const std::uint8_t> haystack, const Pattern& p, size_t start)
{
    const size_t n = p.size();
    if (n == 0 || haystack.size() < n || start > haystack.size() - n) return std::nullopt;

    const size_t anchor = AnchorIndex(p);
    if (anchor >= n) return std::nullopt; // all wildcards: not a meaningful pattern

    const std::uint8_t anchorByte = p.bytes[anchor];
    const std::uint8_t* base = haystack.data();
    const size_t lastStart = haystack.size() - n; // last valid match offset

    // Candidate match offset o has the anchor byte at o + anchor.
    size_t pos = start;
    while (pos <= lastStart) {
        const std::uint8_t* hit =
            static_cast<const std::uint8_t*>(std::memchr(base + pos + anchor, anchorByte, lastStart - pos + 1));
        if (hit == nullptr) return std::nullopt;
        const size_t offset = static_cast<size_t>(hit - base) - anchor;
        if (MatchesAt(base + offset, p)) return offset;
        pos = offset + 1;
    }
    return std::nullopt;
}

std::vector<size_t> FindAllPatterns(std::span<const std::uint8_t> haystack, const Pattern& p, size_t limit)
{
    std::vector<size_t> out;
    size_t start = 0;
    while (out.size() < limit) {
        const auto hit = FindPattern(haystack, p, start);
        if (!hit) break;
        out.push_back(*hit);
        start = *hit + 1;
    }
    return out;
}

std::span<const std::uint8_t> ModuleSection(void* moduleBase, const char* sectionName)
{
    if (moduleBase == nullptr || sectionName == nullptr) return {};
    const auto* base = static_cast<const std::uint8_t*>(moduleBase);

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return {};
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return {};

    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        // Name is 8 bytes, not necessarily NUL-terminated.
        char name[9] = {};
        std::memcpy(name, sec->Name, 8);
        if (std::strcmp(name, sectionName) == 0) {
            const size_t size = sec->Misc.VirtualSize != 0 ? sec->Misc.VirtualSize : sec->SizeOfRawData;
            return {base + sec->VirtualAddress, size};
        }
    }
    return {};
}

} // namespace core
