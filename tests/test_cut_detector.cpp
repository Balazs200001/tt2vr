// stereo::CutDetector: camera-cut decisions on step patterns seen in test mode and in the headset (2026-10-21).
#include <doctest/doctest.h>

#include "stereo/math/cut_detector.hpp"

#include <cmath>
#include <vector>

using stereo::CutDetector;
using stereo::Vec3;

namespace {

constexpr Vec3 kForward{0.0f, 1.0f, 0.0f};

Vec3 Turned(float deg)
{
    const float r = deg * 3.14159265f / 180.0f;
    return {std::sin(r), std::cos(r), 0.0f};
}

// Moves along +y by each step; returns the number of frames reported as cuts.
int Cuts(CutDetector& d, float& y, const std::vector<float>& steps)
{
    int cuts = 0;
    for (float s : steps) {
        y += s;
        cuts += d.Update({0.0f, y, 0.0f}, kForward).cut ? 1 : 0;
    }
    return cuts;
}

} // namespace

TEST_CASE("CutDetector: first frame measures nothing")
{
    CutDetector d;
    const auto r = d.Update({1.0f, 2.0f, 3.0f}, kForward);
    CHECK(r.first);
    CHECK_FALSE(r.cut);
}

TEST_CASE("CutDetector: steady riding and the headset's uneven steps are no cuts")
{
    CutDetector d;
    float y = 0.0f;
    d.Update({0.0f, y, 0.0f}, kForward);
    CHECK(Cuts(d, y, std::vector<float>(60, 1.4f)) == 0); // 300 km/h at 60 fps
    // Headset pacing: steps alternate between short and about 2.7 m (log 13:14:03..13:14:16, all were false cuts).
    std::vector<float> uneven;
    for (int i = 0; i < 40; ++i) {
        uneven.push_back(i % 2 ? 2.9f : 0.3f);
    }
    CHECK(Cuts(d, y, uneven) == 0);
}

TEST_CASE("CutDetector: accelerating from a standstill with uneven steps is no cut")
{
    CutDetector d;
    float y = 0.0f;
    d.Update({0.0f, y, 0.0f}, kForward);
    std::vector<float> steps;
    for (int i = 0; i < 30; ++i) {
        steps.push_back(i % 2 ? 0.1f * static_cast<float>(i) : 0.0f); // 0, 0.1, 0, 0.3, ... up to 2.9 m
    }
    CHECK(Cuts(d, y, steps) == 0);
}

TEST_CASE("CutDetector: crash camera and respawn are cuts")
{
    CutDetector d;
    float y = 0.0f;
    d.Update({0.0f, y, 0.0f}, kForward);
    Cuts(d, y, std::vector<float>(20, 0.5f)); // about 100 km/h
    // Crash camera: 4.3 m and 16 deg (test mode).
    auto r = d.Update({0.0f, y + 4.3f, 0.0f}, Turned(16.0f));
    CHECK(r.cut);
    // Respawn from a stopped crash camera: 6.8 m, back to the road heading.
    CutDetector s;
    s.Update({0.0f, 0.0f, 0.0f}, Turned(16.0f));
    r = s.Update({0.0f, 6.8f, 0.0f}, kForward);
    CHECK(r.cut);
    // A teleport from a standstill without turning (start-line reset: 3.8 m, 3 deg) is a cut too.
    CutDetector t;
    t.Update({0.0f, 0.0f, 0.0f}, kForward);
    CHECK(t.Update({0.0f, 3.8f, 0.0f}, Turned(3.0f)).cut);
}

TEST_CASE("CutDetector: turns")
{
    CutDetector d;
    d.Update({0.0f, 0.0f, 0.0f}, kForward);
    CHECK_FALSE(d.Update({0.0f, 0.1f, 0.0f}, Turned(5.0f)).cut);
    CHECK(d.Update({0.0f, 0.2f, 0.0f}, Turned(16.0f)).cut);
}

TEST_CASE("CutDetector: a cut does not raise the limit")
{
    CutDetector d;
    d.Update({0.0f, 0.0f, 0.0f}, kForward);
    CHECK(d.Update({0.0f, 100.0f, 0.0f}, kForward).cut); // teleport
    const auto r = d.Update({0.0f, 103.0f, 0.0f}, kForward); // 3 m after it, still from a standstill
    CHECK(r.limit == doctest::Approx(CutDetector::kMinDistance));
    CHECK(r.cut);
}

TEST_CASE("CutDetector: World Shift is rebased, not a cut")
{
    CutDetector d;
    d.Update({100.0f, -2047.5f, 50.0f}, kForward);
    const Vec3 shift{0.0f, -2048.0f, 0.0f};
    const auto r = d.Update({100.0f, -2047.8f + 2048.0f, 50.0f}, kForward, &shift);
    CHECK(r.rebased);
    CHECK(r.distance == doctest::Approx(0.3f).epsilon(0.01)); // float positions near 2048 m: ~1e-4 m resolution
    CHECK(r.unrebasedDistance > 2000.0f);
    CHECK_FALSE(r.cut);
    // A shift flag that does not match the camera's jump is ignored.
    const Vec3 wrong{2048.0f, 0.0f, 0.0f};
    const auto w = d.Update({100.0f, 0.5f, 50.0f}, kForward, &wrong);
    CHECK_FALSE(w.rebased);
}
