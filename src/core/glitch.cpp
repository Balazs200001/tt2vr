#include "core/glitch.hpp"

#include <cstdlib>

namespace core {
namespace {

int Luma(const std::uint8_t* bgra)
{
    return (bgra[2] * 77 + bgra[1] * 150 + bgra[0] * 29) >> 8;
}

} // namespace

GlitchScore ScoreGlitch(const std::uint8_t* prev, const std::uint8_t* cur, const std::uint8_t* next, std::size_t stride,
    int x0, int x1, int height)
{
    GlitchScore r;
    for (int ty = 0; ty + kGlitchTile <= height; ty += kGlitchTile) {
        for (int tx = x0; tx + kGlitchTile <= x1; tx += kGlitchTile) {
            long sum = 0, sum2 = 0;
            for (int y = ty; y < ty + kGlitchTile; ++y) {
                const std::size_t row = static_cast<std::size_t>(y) * stride;
                for (int x = tx; x < tx + kGlitchTile; ++x) {
                    const std::size_t at = row + static_cast<std::size_t>(x) * 4;
                    const int p = Luma(prev + at), c = Luma(cur + at), n = Luma(next + at);
                    sum += std::abs(2 * c - p - n); // 2 x |c - (p + n) / 2|
                    sum2 += std::abs(n - p);
                }
            }
            constexpr float kPixels = static_cast<float>(kGlitchTile * kGlitchTile);
            const float s = static_cast<float>(sum) / (2.0f * kPixels);
            const float d2 = static_cast<float>(sum2) / kPixels;
            ++r.total;
            if (s > kGlitchMinScore && s > kGlitchRatio * d2 + kGlitchSlack) {
                ++r.tiles;
                if (s > r.maxScore) {
                    r.maxScore = s;
                    r.maxScoreD2 = d2;
                }
            }
        }
    }
    return r;
}

} // namespace core
