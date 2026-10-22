// core::Glitch: scoring for d3d::FlashDetector ([debug] flash_detect). Pure, unit-tested in tests/test_glitch.cpp.
//
// Three consecutive frames prev, cur, next (8-bit BGRA, same size). In each kTile x kTile tile of the rectangle
// [x0, x1) x [0, height):
//   s  = mean |luma(cur) - (luma(prev) + luma(next)) / 2|   (how unlike its neighbours cur is)
//   d2 = mean |luma(next) - luma(prev)|                       (how much the scene changes over two frames anyway)
// A tile is a glitch tile when s > kMinScore and s > kRatio x d2 + kSlack: cur is unlike both neighbours while those
// agree, as when scenery is missing for one frame. Steady motion and camera cuts (prev != next) do not qualify.
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {

struct GlitchScore {
    int tiles = 0; // glitch tiles
    int total = 0; // tiles looked at
    float maxScore = 0.0f; // the largest s among the glitch tiles (0..255)
    float maxScoreD2 = 0.0f; // that tile's d2
};

inline constexpr int kGlitchTile = 32;
inline constexpr float kGlitchMinScore = 20.0f;
inline constexpr float kGlitchRatio = 2.0f;
inline constexpr float kGlitchSlack = 4.0f;

// `stride` in bytes. Tiles that do not fit completely are skipped.
GlitchScore ScoreGlitch(const std::uint8_t* prev, const std::uint8_t* cur, const std::uint8_t* next, std::size_t stride,
    int x0, int x1, int height);

} // namespace core
