#include "stereo/math/matrix.hpp"

namespace stereo {

using namespace DirectX;

Mat44 Identity44()
{
    Mat44 out;
    XMStoreFloat4x4(&out, XMMatrixIdentity());
    return out;
}

Mat34 Identity34()
{
    return {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
}

Mat44 ToMat44(const Mat34& a)
{
    Mat44 out = Identity44();
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            out.m[r][c] = a.m[r][c];
        }
    }
    return out;
}

Mat34 ToMat34(const Mat44& a)
{
    Mat34 out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            out.m[r][c] = a.m[r][c];
        }
    }
    return out;
}

Mat44 Mul(const Mat44& a, const Mat44& b)
{
    Mat44 out;
    XMStoreFloat4x4(&out, XMMatrixMultiply(XMLoadFloat4x4(&a), XMLoadFloat4x4(&b)));
    return out;
}

Mat34 Mul(const Mat34& a, const Mat34& b)
{
    return ToMat34(Mul(ToMat44(a), ToMat44(b)));
}

Mat44 Inverse(const Mat44& a)
{
    XMVECTOR det;
    const XMMATRIX inv = XMMatrixInverse(&det, XMLoadFloat4x4(&a));
    if (XMVectorGetX(det) == 0.0f) {
        return Identity44();
    }
    Mat44 out;
    XMStoreFloat4x4(&out, inv);
    return out;
}

Vec3 TransformPoint(const Mat34& a, const Vec3& p)
{
    const Vec3 v = TransformVector(a, p);
    return {v.x + a.m[0][3], v.y + a.m[1][3], v.z + a.m[2][3]};
}

Vec3 TransformVector(const Mat34& a, const Vec3& v)
{
    return {a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z, a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
        a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z};
}

Vec3 Translation(const Mat34& a)
{
    return {a.m[0][3], a.m[1][3], a.m[2][3]};
}

} // namespace stereo
