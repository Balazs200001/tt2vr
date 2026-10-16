// Engine projection matrices (docs/conventions.md sections 3 and 8.2).
//
// The engine's perspective is right-handed, reversed-Z, finite far, D3D clip z in [0, 1], w = -z_view
// (Math::MatrixPerspectiveFovRH 0x140813390). Only four elements depend on the field of view:
//   m[0][0] = 2 / (tR - tL)            m[0][2] = (tR + tL) / (tR - tL)
//   m[1][1] = 2 / (tU - tD)            m[1][2] = (tU + tD) / (tU - tD)
// where tX are the tangents of the frustum half-angles (left and down negative for a centred frustum).
// NDC x = m[0][0] * (x / -z) - m[0][2], so a positive m[0][2] moves the image left (window shifted right).
// The depth rows m[2][2] = n / (f - n), m[2][3] = f*n / (f - n), m[3][2] = -1 never change with the FOV.
// TAA jitter is added on top of m[0][2] / m[1][2] by the engine after the matrix is built (section 3.5).
#pragma once

#include "stereo/math/matrix.hpp"

namespace stereo {

struct FovTangents {
    float left, right, up, down; // tan of the half-angles; left/down < 0 for a frustum containing the axis
};

// Replica of the engine's Math::MatrixPerspectiveFovRH(out, hfov, near, far, aspectScale): hfov is the
// HORIZONTAL fov in radians, m[1][1] = m[0][0] * aspect.
Mat44 PerspectiveFovRH(float hfovRad, float nearZ, float farZ, float aspect);

// Asymmetric engine-convention perspective from tangents (conventions 8.2).
Mat44 PerspectiveFromTangents(const FovTangents& t, float nearZ, float farZ);

// Reads the four FOV elements of an engine perspective (inverse of ApplyTangents). Jitter, if present in
// m[0][2] / m[1][2], is read as part of the asymmetry.
FovTangents TangentsFromProjection(const Mat44& p);

// Overwrites the four FOV elements; leaves the depth rows untouched.
void ApplyTangents(Mat44& p, const FovTangents& t);

// XrFovf angles (radians; angleLeft and angleDown are negative for a normal eye) to tangents.
FovTangents TangentsFromXrFov(float angleLeft, float angleRight, float angleUp, float angleDown);

// Shifts the view window by `fx` of its width to the right and `fy` of its height up, keeping its size.
// Example: fx = 0.2 shows 20% more on the right; objects move left in the image. m[0][2] grows by 2 * fx.
FovTangents OffsetTangents(const FovTangents& t, float fx, float fy);

} // namespace stereo
