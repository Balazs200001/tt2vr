#include "stereo/math/xr_to_engine.hpp"

#include <cmath>

namespace stereo {

Mat34 PoseToMatrix(const Pose& pose)
{
    const auto [x, y, z, w] = pose.orientation;
    return {{
        {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), pose.position.x},
        {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), pose.position.y},
        {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), pose.position.z},
    }};
}

Mat34 CameraLocalTransform(float yawRad, float pitchRad, float rollRad, const Vec3& offset)
{
    const float cy = std::cos(yawRad), sy = std::sin(yawRad);
    const float cp = std::cos(pitchRad), sp = std::sin(pitchRad);
    const float cr = std::cos(rollRad), sr = std::sin(rollRad);
    const Mat34 ry{{{cy, 0, sy, 0}, {0, 1, 0, 0}, {-sy, 0, cy, 0}}};
    const Mat34 rx{{{1, 0, 0, 0}, {0, cp, -sp, 0}, {0, sp, cp, 0}}};
    const Mat34 rz{{{cr, -sr, 0, 0}, {sr, cr, 0, 0}, {0, 0, 1, 0}}};
    Mat34 out = Mul(ry, Mul(rx, rz));
    out.m[0][3] = offset.x;
    out.m[1][3] = offset.y;
    out.m[2][3] = offset.z;
    return out;
}

Mat34 ComposeCameraLocal(const Mat34& cameraWorld, const Mat34& local)
{
    return Mul(cameraWorld, local);
}

Vec3 XrToEngineWorld(const Vec3& v)
{
    return {v.x, -v.z, v.y};
}

Mat34 XrToEngineWorld(const Mat34& xrPose)
{
    constexpr Mat34 kB{{{1, 0, 0, 0}, {0, 0, -1, 0}, {0, 1, 0, 0}}};
    return Mul(kB, xrPose);
}

Mat44 ViewFromWorld(const Mat34& world)
{
    return Inverse(ToMat44(world));
}

Mat34 RigidInverse(const Mat34& a)
{
    Mat34 out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out.m[r][c] = a.m[c][r];
        }
    }
    for (int r = 0; r < 3; ++r) {
        out.m[r][3] = -(out.m[r][0] * a.m[0][3] + out.m[r][1] * a.m[1][3] + out.m[r][2] * a.m[2][3]);
    }
    return out;
}

Mat34 RecenterFromHead(const Pose& head)
{
    const Mat34 m = PoseToMatrix(head);
    // Ry(yaw) * (0, 0, -1) = (-sin yaw, 0, -cos yaw); forward is -column 2.
    float x = -m.m[0][2], z = -m.m[2][2];
    float yaw = 0.0f;
    if (x * x + z * z > 1e-6f) {
        yaw = std::atan2(-x, -z);
    }
    else {
        // Forward is vertical. Ry(yaw) * Rx(+-90 deg) * (0, 1, 0) = +-(sin yaw, 0, cos yaw), sign = -forward.y.
        const float s = m.m[1][2] > 0.0f ? 1.0f : -1.0f; // column 2 is back: +y when looking down
        x = s * -m.m[0][1];
        z = s * -m.m[2][1];
        yaw = std::atan2(x, z);
    }
    return CameraLocalTransform(yaw, 0.0f, 0.0f, head.position);
}

Mat34 EyeLocal(const Mat34& recenter, const Pose& eye)
{
    return Mul(RigidInverse(recenter), PoseToMatrix(eye));
}

Pose ScreenPose(const Mat34& recenter, float distance)
{
    // Recenter is Ry(yaw) plus translation: column 2 (back) = (sin yaw, 0, cos yaw).
    const float yaw = std::atan2(recenter.m[0][2], recenter.m[2][2]);
    const Vec3 center = TransformPoint(recenter, {0.0f, 0.0f, -distance});
    return {{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)}, center};
}

} // namespace stereo
