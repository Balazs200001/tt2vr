#include "stereo/math/comfort.hpp"

#include "stereo/math/xr_to_engine.hpp"

#include <algorithm>
#include <cmath>

namespace stereo {

float HorizonRoll(const Mat34& camera)
{
    return std::atan2(camera.m[2][0], camera.m[2][1]);
}

Mat34 HorizonLock(const Mat34& camera, float share)
{
    // X.z^2 + Y.z^2 = 1 - Z.z^2 = cos^2(pitch): the roll's lever. cos 80 deg = 0.1736, cos 60 deg = 0.5.
    const float lever = std::sqrt(camera.m[2][0] * camera.m[2][0] + camera.m[2][1] * camera.m[2][1]);
    const float fade = std::clamp((lever - 0.1736f) / (0.5f - 0.1736f), 0.0f, 1.0f);
    const float roll = HorizonRoll(camera) * std::clamp(share, 0.0f, 1.0f) * fade;
    // camera * Rz(a): X' = X cos a + Y sin a, so X'.z = 0 at a = -roll.
    return CameraLocalTransform(0.0f, 0.0f, -roll, {0.0f, 0.0f, 0.0f});
}

} // namespace stereo
