// stereo/math: projection-from-FOV builder and XR -> engine conversion, against hand-computed values.
#include <doctest/doctest.h>

#include "stereo/math/projection.hpp"
#include "stereo/math/xr_to_engine.hpp"

#include <cmath>
#include <numbers>

using namespace stereo;

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

struct Clip {
    float x, y, z, w;
};

// clip = P * (p, 1), column vectors.
Clip Project(const Mat44& p, const Vec3& v)
{
    Clip c{};
    float* out = &c.x;
    for (int r = 0; r < 4; ++r) {
        out[r] = p.m[r][0] * v.x + p.m[r][1] * v.y + p.m[r][2] * v.z + p.m[r][3];
    }
    return c;
}

void CheckVec(const Vec3& a, const Vec3& b, double eps = 1e-5)
{
    CHECK(a.x == doctest::Approx(b.x).epsilon(eps));
    CHECK(a.y == doctest::Approx(b.y).epsilon(eps));
    CHECK(a.z == doctest::Approx(b.z).epsilon(eps));
}

void CheckNear(float a, float b, float eps = 1e-5f)
{
    CHECK(std::fabs(a - b) <= eps);
}

void CheckMat(const Mat34& a, const Mat34& b, float eps = 1e-5f)
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            CheckNear(a.m[r][c], b.m[r][c], eps);
        }
    }
}

} // namespace

TEST_CASE("PerspectiveFovRH matches the engine formula (hand-computed values)")
{
    // hfov 90 deg -> 1 / tan(45 deg) = 1; aspect 16:9; helmet camera n = 1, f = 25000 (conventions 3.1).
    const Mat44 p = PerspectiveFovRH(kPi / 2, 1.0f, 25000.0f, 16.0f / 9.0f);
    CHECK(p.m[0][0] == doctest::Approx(1.0));
    CHECK(p.m[1][1] == doctest::Approx(16.0 / 9.0));
    CHECK(p.m[2][2] == doctest::Approx(1.0 / 24999.0).epsilon(1e-4)); // n / (f - n)
    CHECK(p.m[2][3] == doctest::Approx(25000.0 / 24999.0)); // f * n / (f - n)
    CHECK(p.m[3][2] == -1.0f);
    CHECK(p.m[3][3] == 0.0f);
    CHECK(p.m[0][2] == 0.0f);
    CHECK(p.m[1][2] == 0.0f);

    SUBCASE("runtime values of capture f8323: hfov 84.4 deg at 1920x1080")
    {
        const Mat44 q = PerspectiveFovRH(2.0f * std::atan(1.0f / 1.1030f), 1.0f, 25000.0f, 1920.0f / 1080.0f);
        CHECK(q.m[0][0] == doctest::Approx(1.1030));
        CHECK(q.m[1][1] == doctest::Approx(1.9609).epsilon(1e-3));
    }
    SUBCASE("reversed-Z: near plane -> ndc z 1, far plane -> 0")
    {
        const Clip nearPt = Project(p, {0, 0, -1.0f});
        const Clip farPt = Project(p, {0, 0, -25000.0f});
        CHECK(nearPt.z / nearPt.w == doctest::Approx(1.0));
        CHECK(std::fabs(farPt.z / farPt.w) < 1e-6);
    }
}

TEST_CASE("Asymmetric projection maps the frustum edges to NDC -1 / +1")
{
    const FovTangents t{-0.8f, 1.2f, 0.9f, -0.7f};
    const Mat44 p = PerspectiveFromTangents(t, 0.5f, 1000.0f);
    CHECK(p.m[0][0] == doctest::Approx(1.0)); // 2 / (1.2 + 0.8)
    CHECK(p.m[1][1] == doctest::Approx(1.25)); // 2 / (0.9 + 0.7)
    CHECK(p.m[0][2] == doctest::Approx(0.2)); // (1.2 - 0.8) / 2
    CHECK(p.m[1][2] == doctest::Approx(0.125)); // (0.9 - 0.7) / 1.6
    const float d = 5.0f; // distance in front of the eye (z = -5)
    const Clip right = Project(p, {t.right * d, 0, -d});
    const Clip left = Project(p, {t.left * d, 0, -d});
    const Clip up = Project(p, {0, t.up * d, -d});
    const Clip down = Project(p, {0, t.down * d, -d});
    CHECK(right.x / right.w == doctest::Approx(1.0));
    CHECK(left.x / left.w == doctest::Approx(-1.0));
    CHECK(up.y / up.w == doctest::Approx(1.0));
    CHECK(down.y / down.w == doctest::Approx(-1.0));
    // Depth rows are those of the engine formula with the same n / f.
    const Mat44 sym = PerspectiveFovRH(1.0f, 0.5f, 1000.0f, 1.0f);
    CHECK(p.m[2][2] == sym.m[2][2]);
    CHECK(p.m[2][3] == sym.m[2][3]);
    CHECK(p.m[3][2] == -1.0f);
}

TEST_CASE("Tangents round-trip through an engine matrix")
{
    const Mat44 sym = PerspectiveFovRH(1.4731f, 1.0f, 25000.0f, 1.7778f);
    const FovTangents t = TangentsFromProjection(sym);
    CHECK(t.right == doctest::Approx(std::tan(1.4731f / 2)));
    CHECK(t.left == doctest::Approx(-t.right));
    CHECK(t.up == doctest::Approx(t.right / 1.7778f));
    CHECK(t.down == doctest::Approx(-t.up));
    Mat44 rebuilt = sym;
    ApplyTangents(rebuilt, t);
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            CheckNear(rebuilt.m[r][c], sym.m[r][c]);
        }
    }
    const FovTangents a{-0.9f, 1.1f, 0.8f, -1.0f};
    Mat44 asym = sym;
    ApplyTangents(asym, a);
    const FovTangents back = TangentsFromProjection(asym);
    CheckNear(back.left, a.left);
    CheckNear(back.right, a.right);
    CheckNear(back.up, a.up);
    CheckNear(back.down, a.down);
}

TEST_CASE("OffsetTangents shifts the window without resizing it")
{
    const FovTangents t{-1.0f, 1.0f, 0.5625f, -0.5625f};
    const FovTangents s = OffsetTangents(t, 0.2f, 0.0f);
    CHECK(s.left == doctest::Approx(-0.6));
    CHECK(s.right == doctest::Approx(1.4));
    CHECK(s.right - s.left == doctest::Approx(t.right - t.left));
    Mat44 p = PerspectiveFovRH(kPi / 2, 1.0f, 100.0f, 16.0f / 9.0f);
    ApplyTangents(p, s);
    CHECK(p.m[0][2] == doctest::Approx(0.4)); // grows by 2 * fx
    CHECK(p.m[0][0] == doctest::Approx(1.0)); // scale unchanged
    // A point on the optical axis now lands left of centre (window moved right).
    const Clip axis = Project(p, {0, 0, -10.0f});
    CHECK(axis.x / axis.w == doctest::Approx(-0.4));
}

TEST_CASE("UnionTangents contains both mirrored eye frusta")
{
    const FovTangents left{-1.38f, 0.84f, 0.97f, -1.43f};
    const FovTangents right{-0.84f, 1.38f, 0.97f, -1.43f};
    const FovTangents u = UnionTangents(left, right);
    CHECK(u.left == doctest::Approx(-1.38f));
    CHECK(u.right == doctest::Approx(1.38f));
    CHECK(u.up == doctest::Approx(0.97f));
    CHECK(u.down == doctest::Approx(-1.43f));
}

TEST_CASE("TangentsFromXrFov (Quest 3-like left eye)")
{
    const FovTangents t = TangentsFromXrFov(-0.9076f, 0.7505f, 0.8203f, -0.8901f);
    CHECK(t.left == doctest::Approx(std::tan(-0.9076)));
    CHECK(t.right == doctest::Approx(std::tan(0.7505)));
    CHECK(t.up == doctest::Approx(std::tan(0.8203)));
    CHECK(t.down == doctest::Approx(std::tan(-0.8901)));
    const Mat44 p = PerspectiveFromTangents(t, 1.0f, 25000.0f);
    CHECK(p.m[0][2] < 0.0f); // left eye: more FOV to the left -> negative x asymmetry
}

TEST_CASE("CameraLocalTransform: yaw / pitch / roll signs and offset")
{
    const Vec3 forward{0, 0, -1};
    const Vec3 up{0, 1, 0};
    CheckVec(TransformVector(CameraLocalTransform(kPi / 2, 0, 0, {}), forward), {-1, 0, 0}); // yaw left
    CheckVec(TransformVector(CameraLocalTransform(0, kPi / 2, 0, {}), forward), {0, 1, 0}); // pitch up
    CheckVec(TransformVector(CameraLocalTransform(0, 0, kPi / 2, {}), up), {-1, 0, 0}); // roll left
    CheckVec(TransformVector(CameraLocalTransform(kPi, 0, 0, {}), forward), {0, 0, 1}); // look behind
    // Offset is applied after the rotation: the local origin lands exactly on the offset.
    CheckVec(TransformPoint(CameraLocalTransform(0.3f, -0.2f, 0.1f, {0.1f, 0.2f, -0.3f}), {0, 0, 0}), {0.1f, 0.2f, -0.3f});
}

TEST_CASE("XR pose matrix equals the camera-local transform for the same rotation")
{
    const float h = kPi / 4; // half of a 90 deg yaw about +Y
    const Pose pose{{0, std::sin(h), 0, std::cos(h)}, {0.03f, 0.0f, 0.0f}};
    CheckMat(PoseToMatrix(pose), CameraLocalTransform(kPi / 2, 0, 0, {0.03f, 0.0f, 0.0f}));
    const Pose pitch{{std::sin(0.25f), 0, 0, std::cos(0.25f)}, {}};
    CheckMat(PoseToMatrix(pitch), CameraLocalTransform(0, 0.5f, 0, {}));
}

TEST_CASE("ComposeCameraLocal on a camera heading +Y on a level road (Z-up world)")
{
    // Camera world: columns (right, up, back) = (1,0,0), (0,0,1), (0,-1,0), position (10, 20, 3).
    const Mat34 cam{{{1, 0, 0, 10}, {0, 0, -1, 20}, {0, 1, 0, 3}}};
    CheckVec(TransformVector(cam, {0, 0, -1}), {0, 1, 0}); // camera forward = world +Y
    // 1 m forward in camera space -> 1 m along world +Y.
    CheckVec(Translation(ComposeCameraLocal(cam, CameraLocalTransform(0, 0, 0, {0, 0, -1}))), {10, 21, 3});
    // Yaw left 90 deg -> forward becomes world -X; up stays world +Z.
    const Mat34 yawed = ComposeCameraLocal(cam, CameraLocalTransform(kPi / 2, 0, 0, {}));
    CheckVec(TransformVector(yawed, {0, 0, -1}), {-1, 0, 0});
    CheckVec(TransformVector(yawed, {0, 1, 0}), {0, 0, 1});
    CheckVec(Translation(yawed), {10, 20, 3});
}

TEST_CASE("XrToEngineWorld basis change B")
{
    CheckVec(XrToEngineWorld(Vec3{0, 0, -1}), {0, 1, 0}); // XR forward -> engine forward (+Y)
    CheckVec(XrToEngineWorld(Vec3{0, 1, 0}), {0, 0, 1}); // XR up -> engine up (+Z)
    CheckVec(XrToEngineWorld(Vec3{1, 0, 0}), {1, 0, 0}); // right stays right
    // Identity XR pose -> camera looking along engine +Y with Z up; proper rotation (det +1).
    const Mat34 m = XrToEngineWorld(Identity34());
    CheckVec(TransformVector(m, {0, 0, -1}), {0, 1, 0});
    const float det = m.m[0][0] * (m.m[1][1] * m.m[2][2] - m.m[1][2] * m.m[2][1]) -
                      m.m[0][1] * (m.m[1][0] * m.m[2][2] - m.m[1][2] * m.m[2][0]) +
                      m.m[0][2] * (m.m[1][0] * m.m[2][1] - m.m[1][1] * m.m[2][0]);
    CHECK(det == doctest::Approx(1.0));
    // Positions go through the same B.
    const Mat34 p = XrToEngineWorld(PoseToMatrix({{0, 0, 0, 1}, {0.1f, 1.2f, -0.5f}}));
    CheckVec(Translation(p), {0.1f, 0.5f, 1.2f});
}

TEST_CASE("ViewFromWorld inverts the camera world matrix")
{
    const Mat34 world = ComposeCameraLocal(Mat34{{{1, 0, 0, -211.2f}, {0, 0, -1, -14.25f}, {0, 1, 0, 50.7f}}},
        CameraLocalTransform(0.7f, -0.28f, 0.05f, {0.2f, 0.1f, 0}));
    const Mat44 view = ViewFromWorld(world);
    const Mat44 product = Mul(view, ToMat44(world));
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            CheckNear(product.m[r][c], r == c ? 1.0f : 0.0f, 1e-4f);
        }
    }
    // The camera position maps to the view-space origin.
    const Vec3 pos = Translation(world);
    const Vec3 inView = TransformPoint(ToMat34(view), pos);
    CheckNear(inView.x, 0, 1e-3f);
    CheckNear(inView.y, 0, 1e-3f);
    CheckNear(inView.z, 0, 1e-3f);
}

TEST_CASE("RigidInverse undoes a rigid transform")
{
    const Mat34 a = CameraLocalTransform(0.7f, -0.3f, 0.2f, {1.0f, -2.0f, 0.5f});
    CheckMat(Mul(RigidInverse(a), a), Identity34());
    CheckMat(Mul(a, RigidInverse(a)), Identity34());
}

TEST_CASE("RecenterFromHead keeps position and yaw, drops pitch and roll")
{
    // Head yawed 30 deg left, pitched 20 deg down, rolled 10 deg, at (0.1, 1.2, -0.3).
    const Mat34 head = CameraLocalTransform(0.5236f, -0.349f, 0.1745f, {0.1f, 1.2f, -0.3f});
    // Same rotation as a pose: q = q_yaw * q_pitch * q_roll.
    const auto quat = [](float ax, float ay, float az, float angle) {
        const float s = std::sin(angle / 2);
        return Quat{ax * s, ay * s, az * s, std::cos(angle / 2)};
    };
    const auto mul = [](const Quat& a, const Quat& b) {
        return Quat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    };
    const Quat q = mul(quat(0, 1, 0, 0.5236f), mul(quat(1, 0, 0, -0.349f), quat(0, 0, 1, 0.1745f)));
    const Pose pose{q, {0.1f, 1.2f, -0.3f}};
    CheckMat(PoseToMatrix(pose), head, 1e-4f);
    CheckMat(RecenterFromHead(pose), CameraLocalTransform(0.5236f, 0, 0, {0.1f, 1.2f, -0.3f}), 1e-4f);

    SUBCASE("looking straight down or up still finds the yaw")
    {
        const Pose down{mul(quat(0, 1, 0, -1.0f), quat(1, 0, 0, -kPi / 2)), {}};
        CheckMat(RecenterFromHead(down), CameraLocalTransform(-1.0f, 0, 0, {}), 1e-4f);
        const Pose up{mul(quat(0, 1, 0, 2.0f), quat(1, 0, 0, kPi / 2)), {}};
        CheckMat(RecenterFromHead(up), CameraLocalTransform(2.0f, 0, 0, {}), 1e-4f);
    }
}

TEST_CASE("EyeLocal is the eye relative to the recentered head")
{
    // Recentered at a head 1.2 m up, yawed 90 deg left. The left eye is 32 mm to the head's left.
    const float h = kPi / 4;
    const Quat yawLeft{0, std::sin(h), 0, std::cos(h)};
    const Pose head{yawLeft, {0.0f, 1.2f, 0.0f}};
    const Mat34 recenter = RecenterFromHead(head);
    // In the reference space the head's left (-X local) is +Z after the 90 deg left yaw.
    const Pose leftEye{yawLeft, {0.0f, 1.2f, 0.032f}};
    const Mat34 local = EyeLocal(recenter, leftEye);
    CheckMat(local, CameraLocalTransform(0, 0, 0, {-0.032f, 0, 0}), 1e-5f);
    // Leaning 10 cm forward along the recentered forward moves the eye -Z in camera-local axes.
    const Pose leaned{yawLeft, {-0.1f, 1.2f, 0.032f}};
    CheckVec(Translation(EyeLocal(recenter, leaned)), {-0.032f, 0.0f, -0.1f});
}

TEST_CASE("ScreenPose is straight ahead of the recentered head, facing it")
{
    const float h = kPi / 4; // head yawed 90 deg left at (0.2, 1.1, -0.5)
    const Mat34 recenter = RecenterFromHead({{0, std::sin(h), 0, std::cos(h)}, {0.2f, 1.1f, -0.5f}});
    const Pose screen = ScreenPose(recenter, 2.0f);
    CheckVec(screen.position, {0.2f - 2.0f, 1.1f, -0.5f}); // forward is -X after a 90 deg left yaw
    CheckMat(PoseToMatrix(screen), CameraLocalTransform(kPi / 2, 0, 0, screen.position), 1e-5f);
}
