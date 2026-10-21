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

float RotationAngle(const Mat34& r)
{
    const float c = (r.m[0][0] + r.m[1][1] + r.m[2][2] - 1.0f) * 0.5f;
    return std::acos(std::clamp(c, -1.0f, 1.0f));
}

Mat34 ScaleRotation(const Mat34& r, float t)
{
    // Axis from the skew-symmetric part: (R - R^T) / 2 = sin(angle) [axis]x. Fine for angles well below pi.
    const float angle = RotationAngle(r);
    const float x = r.m[2][1] - r.m[1][2], y = r.m[0][2] - r.m[2][0], z = r.m[1][0] - r.m[0][1];
    const float len = std::sqrt(x * x + y * y + z * z);
    if (angle < 1e-6f || len < 1e-9f) {
        return Identity34();
    }
    const float ax = x / len, ay = y / len, az = z / len;
    const float a = angle * t, c = std::cos(a), s = std::sin(a), k = 1.0f - c;
    return {{
        {c + ax * ax * k, ax * ay * k - az * s, ax * az * k + ay * s, 0.0f},
        {ay * ax * k + az * s, c + ay * ay * k, ay * az * k - ax * s, 0.0f},
        {az * ax * k - ay * s, az * ay * k + ax * s, c + az * az * k, 0.0f},
    }};
}

} // namespace stereo
