// stereo/math comfort transforms (PLAN Phase 8): horizon roll and the horizon lock.
#include <doctest/doctest.h>

#include "stereo/math/comfort.hpp"
#include "stereo/math/xr_to_engine.hpp"

#include <cmath>
#include <initializer_list>
#include <numbers>

using namespace stereo;

namespace {

constexpr float kDeg = std::numbers::pi_v<float> / 180.0f;

// A camera heading +Y on a level road in the Z-up world (right +X, up +Z, back -Y), turned by yaw / pitch / roll in
// its own axes.
Mat34 RoadCamera(float yawDeg, float pitchDeg, float rollDeg)
{
    const Mat34 level{{{1, 0, 0, 5}, {0, 0, -1, 7}, {0, 1, 0, 1.2f}}};
    return Mul(level, CameraLocalTransform(yawDeg * kDeg, pitchDeg * kDeg, rollDeg * kDeg, {0, 0, 0}));
}

// Forward (-Z) in world axes.
Vec3 Forward(const Mat34& m)
{
    return {-m.m[0][2], -m.m[1][2], -m.m[2][2]};
}

} // namespace

TEST_CASE("HorizonRoll: positive = tilted left, as CameraLocalTransform's roll")
{
    CHECK(HorizonRoll(RoadCamera(0, 0, 0)) == doctest::Approx(0.0f));
    CHECK(HorizonRoll(RoadCamera(0, 0, 20)) == doctest::Approx(20.0f * kDeg));
    CHECK(HorizonRoll(RoadCamera(35, 0, -12)) == doctest::Approx(-12.0f * kDeg));
}

TEST_CASE("HorizonLock removes the given share of the roll and keeps position and forward")
{
    const Mat34 camera = RoadCamera(25, 15, 20);
    for (const float share : {0.0f, 0.5f, 1.0f}) {
        const Mat34 locked = Mul(camera, HorizonLock(camera, share));
        CHECK(HorizonRoll(locked) == doctest::Approx(HorizonRoll(camera) * (1.0f - share)).epsilon(1e-4));
        const Vec3 f0 = Forward(camera), f1 = Forward(locked);
        CHECK(f1.x == doctest::Approx(f0.x).epsilon(1e-5));
        CHECK(f1.y == doctest::Approx(f0.y).epsilon(1e-5));
        CHECK(f1.z == doctest::Approx(f0.z).epsilon(1e-5));
        CHECK(locked.m[0][3] == camera.m[0][3]);
        CHECK(locked.m[2][3] == camera.m[2][3]);
    }
    // A bike-like lean of 55 deg is fully removed.
    const Mat34 lean = RoadCamera(0, -5, -55);
    CHECK(std::fabs(HorizonRoll(Mul(lean, HorizonLock(lean, 1.0f)))) < 1e-4f);
}

TEST_CASE("ScaleEyes: world scale divides head motion and IPD, ipd scale only the separation, seat adds")
{
    // Head 10 cm right and 5 cm up of the recentered point, eyes 64 mm apart, turned 30 deg.
    const Mat34 turn = CameraLocalTransform(30.0f * kDeg, 0, 0, {0, 0, 0});
    Mat34 head = turn;
    head.m[0][3] = 0.10f;
    head.m[1][3] = 0.05f;
    Mat34 eyes[2] = {head, head};
    eyes[0].m[0][3] -= 0.032f;
    eyes[1].m[0][3] += 0.032f;
    Mat34 h = head, e[2] = {eyes[0], eyes[1]};
    ScaleEyes(e, h, 2.0f, 1.0f, {0, 0, 0});
    CHECK(h.m[0][3] == doctest::Approx(0.05f));
    CHECK(h.m[1][3] == doctest::Approx(0.025f));
    CHECK(e[1].m[0][3] - e[0].m[0][3] == doctest::Approx(0.032f)); // IPD halved with the world
    CHECK(e[0].m[0][0] == doctest::Approx(turn.m[0][0])); // rotation kept
    h = head;
    e[0] = eyes[0];
    e[1] = eyes[1];
    ScaleEyes(e, h, 1.0f, 1.5f, {0.0f, -0.02f, -0.01f});
    CHECK(h.m[0][3] == doctest::Approx(0.10f));
    CHECK(h.m[1][3] == doctest::Approx(0.03f));
    CHECK(h.m[2][3] == doctest::Approx(-0.01f));
    CHECK(e[1].m[0][3] - e[0].m[0][3] == doctest::Approx(0.096f));
    CHECK((e[0].m[0][3] + e[1].m[0][3]) * 0.5f == doctest::Approx(0.10f)); // still centred on the head
}

TEST_CASE("ScaleRotation scales the angle about the same axis")
{
    const Mat34 shake = CameraLocalTransform(0.0f, 1.5f * kDeg, -2.0f * kDeg, {0.3f, 0, 0}); // pitch + roll, like a shake
    const float angle = RotationAngle(shake);
    CHECK(angle > 0.0f);
    CHECK(RotationAngle(ScaleRotation(shake, 0.5f)) == doctest::Approx(angle * 0.5f).epsilon(1e-3));
    const Mat34 none = ScaleRotation(shake, 0.0f);
    CHECK(none.m[0][0] == doctest::Approx(1.0f));
    CHECK(none.m[1][2] == doctest::Approx(0.0f));
    // r^-1 * r = identity; r^0.5 * r^0.5 = r; translation dropped.
    const Mat34 back = Mul(ScaleRotation(shake, -1.0f), ScaleRotation(shake, 1.0f));
    CHECK(RotationAngle(back) == doctest::Approx(0.0f).epsilon(1e-3));
    const Mat34 half = ScaleRotation(shake, 0.5f);
    const Mat34 whole = Mul(half, half);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            CHECK(whole.m[r][c] == doctest::Approx(shake.m[r][c]).epsilon(1e-4));
        }
        CHECK(whole.m[r][3] == 0.0f);
    }
    CHECK(RotationAngle(ScaleRotation(Identity34(), 0.5f)) == doctest::Approx(0.0f));
}

TEST_CASE("HorizonLock fades out when looking nearly straight up or down")
{
    const Mat34 steep = RoadCamera(0, 85, 30);
    const Mat34 fix = HorizonLock(steep, 1.0f);
    CHECK(fix.m[0][0] == doctest::Approx(1.0f));
    CHECK(fix.m[1][1] == doctest::Approx(1.0f));
    const Mat34 mid = RoadCamera(0, 70, 30); // half way through the fade
    const float left = HorizonRoll(Mul(mid, HorizonLock(mid, 1.0f)));
    CHECK(std::fabs(left) > 0.0f);
    CHECK(std::fabs(left) < std::fabs(HorizonRoll(mid)));
}
