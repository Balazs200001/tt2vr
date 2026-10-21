// Comfort transforms for the riding camera (PLAN Phase 8). Engine world is Z-up; camera-local axes are X right, Y up,
// Z back (docs/conventions.md). Pure code, unit-tested in tests/test_comfort.cpp.
#pragma once

#include "stereo/math/matrix.hpp"

namespace stereo {

// Roll of a camera against the horizon (world +Z up), radians: atan2(X.z, Y.z) of its axes. Positive = tilted to the
// left (its right side raised), the sign of CameraLocalTransform's roll.
float HorizonRoll(const Mat34& camera);

// Camera-local roll about the view axis that removes `share` (0..1) of `camera`'s roll against the horizon:
// camera * HorizonLock(camera, share) keeps its position and forward axis and has (1 - share) of the roll. The lock
// fades out between 60 and 80 deg of pitch, where the roll is undefined (looking straight up or down).
Mat34 HorizonLock(const Mat34& camera, float share);

// World scale, IPD scale and seat offset (PLAN 8d / 8e) on camera-local eye transforms (stereo::EyeLocal, relative to
// the recentered head): translations become t_i' = (t_head + (t_i - t_head) * ipdScale) / worldScale + seat, the head
// (eye midpoint) t_head / worldScale + seat; rotations are unchanged. worldScale 2 = the world looks twice as large.
void ScaleEyes(Mat34 eyes[2], Mat34& head, float worldScale, float ipdScale, const Vec3& seat);

// Rotation angle of a rigid transform's rotation part, radians (0..pi).
float RotationAngle(const Mat34& r);
// The rotation part of `r` raised to the power t: the same axis, t times the angle (t = 0 identity, 1 the rotation,
// -1 its inverse). Translation is dropped. For camera shakes (PLAN 8c), which are small rotations.
Mat34 ScaleRotation(const Mat34& r, float t);

} // namespace stereo
