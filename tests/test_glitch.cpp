// core::ScoreGlitch on synthetic frames: a region missing for one frame is found, motion and cuts are not.
#include <doctest/doctest.h>

#include "core/glitch.hpp"

#include <cstdint>
#include <vector>

namespace {

constexpr int kW = 256, kH = 128;
constexpr std::size_t kStride = kW * 4;

// A diagonal gradient shifted by `shift` pixels (camera motion), grey.
std::vector<std::uint8_t> Scene(int shift)
{
    std::vector<std::uint8_t> img(kStride * kH);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const auto v = static_cast<std::uint8_t>(((x + shift) * 3 + y * 2) % 200 + 20);
            std::uint8_t* p = &img[y * kStride + x * 4];
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    }
    return img;
}

void Fill(std::vector<std::uint8_t>& img, int x0, int y0, int w, int h, std::uint8_t v)
{
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) {
            std::uint8_t* p = &img[y * kStride + x * 4];
            p[0] = p[1] = p[2] = v;
        }
    }
}

} // namespace

TEST_CASE("ScoreGlitch: steady motion is no glitch")
{
    const auto a = Scene(0), b = Scene(2), c = Scene(4);
    const auto r = core::ScoreGlitch(a.data(), b.data(), c.data(), kStride, 0, kW, kH);
    CHECK(r.total == (kW / core::kGlitchTile) * (kH / core::kGlitchTile));
    CHECK(r.tiles == 0);
}

TEST_CASE("ScoreGlitch: a region replaced for one frame is found, only in its half")
{
    const auto a = Scene(0), c = Scene(4);
    auto b = Scene(2);
    Fill(b, 140, 32, 64, 64, 235); // bright "distant sky" where scenery was, right half only
    const auto right = core::ScoreGlitch(a.data(), b.data(), c.data(), kStride, kW / 2, kW, kH);
    CHECK(right.tiles >= 4);
    CHECK(right.maxScore > core::kGlitchMinScore);
    const auto left = core::ScoreGlitch(a.data(), b.data(), c.data(), kStride, 0, kW / 2, kH);
    CHECK(left.tiles == 0);
}

TEST_CASE("ScoreGlitch: a camera cut (prev and next differ) is no glitch")
{
    auto a = Scene(0), b = Scene(50), c = Scene(52);
    Fill(a, 0, 0, kW, kH, 30); // before the cut: another scene
    const auto r = core::ScoreGlitch(a.data(), b.data(), c.data(), kStride, 0, kW, kH);
    CHECK(r.tiles == 0);
}
