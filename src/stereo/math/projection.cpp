#include "stereo/math/projection.hpp"

#include <cmath>

namespace stereo {

Mat44 PerspectiveFovRH(float hfovRad, float nearZ, float farZ, float aspect)
{
    // Same operation order as the engine (0x140813390) so the result is bit-comparable.
    const float xScale = 1.0f / std::tan(hfovRad * 0.5f);
    const float k = farZ / (nearZ - farZ);
    Mat44 p{};
    p.m[0][0] = xScale;
    p.m[1][1] = xScale * aspect;
    p.m[2][2] = -k - 1.0f;
    p.m[2][3] = -(k * nearZ);
    p.m[3][2] = -1.0f;
    return p;
}

Mat44 PerspectiveFromTangents(const FovTangents& t, float nearZ, float farZ)
{
    Mat44 p = PerspectiveFovRH(1.0f, nearZ, farZ, 1.0f); // depth rows only; the FOV terms are replaced below
    ApplyTangents(p, t);
    return p;
}

FovTangents TangentsFromProjection(const Mat44& p)
{
    const float width = 2.0f / p.m[0][0]; // tR - tL
    const float height = 2.0f / p.m[1][1]; // tU - tD
    const float sumX = p.m[0][2] * width; // tR + tL
    const float sumY = p.m[1][2] * height; // tU + tD
    return {(sumX - width) * 0.5f, (sumX + width) * 0.5f, (sumY + height) * 0.5f, (sumY - height) * 0.5f};
}

void ApplyTangents(Mat44& p, const FovTangents& t)
{
    const float width = t.right - t.left;
    const float height = t.up - t.down;
    p.m[0][0] = 2.0f / width;
    p.m[1][1] = 2.0f / height;
    p.m[0][2] = (t.right + t.left) / width;
    p.m[1][2] = (t.up + t.down) / height;
}

FovTangents TangentsFromXrFov(float angleLeft, float angleRight, float angleUp, float angleDown)
{
    return {std::tan(angleLeft), std::tan(angleRight), std::tan(angleUp), std::tan(angleDown)};
}

FovTangents OffsetTangents(const FovTangents& t, float fx, float fy)
{
    const float dx = fx * (t.right - t.left);
    const float dy = fy * (t.up - t.down);
    return {t.left + dx, t.right + dx, t.up + dy, t.down + dy};
}

} // namespace stereo
