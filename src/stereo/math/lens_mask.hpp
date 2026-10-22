// stereo::LensMask (PLAN 9 lens visibility mask): the headset's hidden-area mesh (XR_KHR_visibility_mask, triangle
// list of points on each view's z = -1 plane, i.e. FOV tangents) turned into normalized device coordinates of the
// eye's rendered projection. Pure, unit-tested in tests/test_lens_mask.cpp.
#pragma once

#include "stereo/math/projection.hpp"

#include <vector>

namespace stereo {

// `tangents` = x, y pairs (triangle list, 3 points per triangle). Returns NDC x, y pairs (D3D: y up) for the eye's
// frustum `fov` (the same tangents the eye is rendered with). Points outside the frustum map outside [-1, 1].
std::vector<float> MaskToNdc(const std::vector<float>& tangents, const FovTangents& fov);

// A stand-in hidden-area mesh for the simulated headset when no real one was saved: everything outside an ellipse
// inscribed in the frustum, scaled by `inset` (1 = touching the edges), as `segments` x 2 triangles (tangent space).
std::vector<float> SyntheticHiddenMesh(const FovTangents& fov, float inset, int segments);

} // namespace stereo
