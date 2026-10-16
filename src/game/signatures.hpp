// Engine signatures (CLAUDE.md hard rule 3: no hard-coded addresses; everything is found by pattern scan).
// Each entry records our name, the pattern, how it was found and the exe SHA-256 it was validated on.
#pragma once

#include <array>
#include <cstddef>

namespace game {

struct Signature {
    const char* name; // our name, as in docs/re/symbols.md
    const char* pattern; // IDA style, see core::ParsePattern
    std::ptrdiff_t offset; // added to the match address (e.g. to land on a rel32)
    const char* howFound; // short evidence note
    const char* validatedOn; // exe SHA-256
};

// A zero-length C array is ill-formed, hence std::array<Signature, 0>. Phase 2 adds the first entries
// (switch to `inline constexpr Signature kSignatures[] = {...}` once there is one).
inline constexpr std::array<Signature, 0> kSignatures{};

} // namespace game
