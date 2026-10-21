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

} // namespace stereo
