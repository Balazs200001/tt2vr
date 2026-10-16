// Engine-layout matrix helpers (docs/conventions.md section 5.1).
//
// Rules for everything in stereo/math:
//   - Column vectors: clip = M * v. Composition A * B applies B first.
//   - Stored row-major: element [r][c] at m[r][c] (= float index r*4+c); translation at [0][3], [1][3], [2][3].
//   - Mat44 is DirectX::XMFLOAT4X4, whose m[r][c] storage is exactly the engine's Matrix44 layout. DirectXMath's
//     XMMatrixMultiply(A, B) is the plain array product, i.e. the engine's A * B (Math::Matrix44_Mul 0x14046baa0).
//     Do not use DirectXMath's XMVector*Transform or XMMatrixRotation*: those assume row vectors.
//   - Mat34 is the engine's Matrix34 (12 floats, 3 rows x 4, implicit last row 0 0 0 1).
// Pure code: no game access, unit-tested in tests/test_math.cpp.
#pragma once

#include <DirectXMath.h>

namespace stereo {

using Mat44 = DirectX::XMFLOAT4X4;

struct Mat34 {
    float m[3][4];
};

struct Vec3 {
    float x, y, z;
};

Mat44 Identity44();
Mat34 Identity34();

Mat44 ToMat44(const Mat34& a); // appends the row (0, 0, 0, 1)
Mat34 ToMat34(const Mat44& a); // drops row 3

Mat44 Mul(const Mat44& a, const Mat44& b); // a * b
Mat34 Mul(const Mat34& a, const Mat34& b); // a * b (affine)
Mat44 Inverse(const Mat44& a); // general inverse; identity if singular

Vec3 TransformPoint(const Mat34& a, const Vec3& p); // R * p + t
Vec3 TransformVector(const Mat34& a, const Vec3& v); // R * v
Vec3 Translation(const Mat34& a);

} // namespace stereo
