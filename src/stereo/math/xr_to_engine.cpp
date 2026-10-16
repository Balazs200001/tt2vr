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

} // namespace stereo
