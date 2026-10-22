// stereo::MaskToNdc / SyntheticHiddenMesh: the hidden-area mesh lands where the eye's projection puts it.
#include <doctest/doctest.h>

#include "stereo/math/lens_mask.hpp"

#include <cmath>

using stereo::FovTangents;

TEST_CASE("MaskToNdc: frustum corners and the axis")
{
    const FovTangents fov{-1.2f, 0.8f, 0.9f, -1.1f};
    const auto ndc = stereo::MaskToNdc({-1.2f, -1.1f, 0.8f, 0.9f, 0.0f, 0.0f}, fov);
    REQUIRE(ndc.size() == 6);
    CHECK(ndc[0] == doctest::Approx(-1.0f)); // left, bottom corner
    CHECK(ndc[1] == doctest::Approx(-1.0f));
    CHECK(ndc[2] == doctest::Approx(1.0f)); // right, top corner
    CHECK(ndc[3] == doctest::Approx(1.0f));
    CHECK(ndc[4] == doctest::Approx(0.2f)); // the view axis is right of centre for this asymmetric frustum
    CHECK(ndc[5] == doctest::Approx(0.1f));
    CHECK(stereo::MaskToNdc({1.0f, 1.0f}, FovTangents{0.5f, 0.5f, 1.0f, -1.0f}).empty()); // degenerate frustum
}

TEST_CASE("SyntheticHiddenMesh: covers the corners, never the centre")
{
    const FovTangents fov{-1.0f, 1.0f, 1.0f, -1.0f};
    const auto tris = stereo::SyntheticHiddenMesh(fov, 0.95f, 32);
    REQUIRE(tris.size() == 32u * 12u);
    // Point-in-triangle over the mesh.
    const auto covered = [&](float px, float py) {
        for (std::size_t i = 0; i + 5 < tris.size(); i += 6) {
            const float ax = tris[i], ay = tris[i + 1], bx = tris[i + 2], by = tris[i + 3], cx = tris[i + 4],
                        cy = tris[i + 5];
            const float d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
            const float d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
            const float d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
            const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
            if (!(neg && pos)) {
                return true;
            }
        }
        return false;
    };
    CHECK(covered(0.97f, 0.97f)); // corner
    CHECK(covered(-0.97f, -0.97f));
    CHECK_FALSE(covered(0.0f, 0.0f)); // centre
    CHECK_FALSE(covered(0.5f, 0.3f));
}
