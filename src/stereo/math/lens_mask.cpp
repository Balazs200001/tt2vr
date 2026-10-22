#include "stereo/math/lens_mask.hpp"

#include <algorithm>
#include <cmath>

namespace stereo {

std::vector<float> MaskToNdc(const std::vector<float>& tangents, const FovTangents& fov)
{
    std::vector<float> out(tangents.size());
    const float w = fov.right - fov.left, h = fov.up - fov.down;
    if (w <= 0.0f || h <= 0.0f) {
        return {};
    }
    for (std::size_t i = 0; i + 1 < tangents.size(); i += 2) {
        out[i] = (2.0f * tangents[i] - (fov.right + fov.left)) / w;
        out[i + 1] = (2.0f * tangents[i + 1] - (fov.up + fov.down)) / h;
    }
    return out;
}

std::vector<float> SyntheticHiddenMesh(const FovTangents& fov, float inset, int segments)
{
    segments = std::max(segments, 4);
    const float cx = 0.5f * (fov.left + fov.right), cy = 0.5f * (fov.up + fov.down);
    const float rx = 0.5f * (fov.right - fov.left) * inset, ry = 0.5f * (fov.up - fov.down) * inset;
    // Each ellipse segment is joined to the point on the frustum rectangle in the same direction (a ring between
    // the ellipse and the rectangle, which covers the corners).
    const auto onRect = [&](float a, float& x, float& y) {
        const float dx = std::cos(a), dy = std::sin(a);
        const float hx = 0.5f * (fov.right - fov.left), hy = 0.5f * (fov.up - fov.down);
        const float t = std::min(std::fabs(dx) > 1e-6f ? hx / std::fabs(dx) : 1e9f,
            std::fabs(dy) > 1e-6f ? hy / std::fabs(dy) : 1e9f);
        x = cx + dx * t;
        y = cy + dy * t;
    };
    std::vector<float> tris;
    tris.reserve(static_cast<std::size_t>(segments) * 12);
    constexpr float kTwoPi = 6.28318530718f;
    for (int i = 0; i < segments; ++i) {
        const float a0 = kTwoPi * static_cast<float>(i) / static_cast<float>(segments);
        const float a1 = kTwoPi * static_cast<float>(i + 1) / static_cast<float>(segments);
        const float e0x = cx + rx * std::cos(a0), e0y = cy + ry * std::sin(a0);
        const float e1x = cx + rx * std::cos(a1), e1y = cy + ry * std::sin(a1);
        float r0x, r0y, r1x, r1y;
        onRect(a0, r0x, r0y);
        onRect(a1, r1x, r1y);
        tris.insert(tris.end(), {e0x, e0y, r0x, r0y, r1x, r1y, e0x, e0y, r1x, r1y, e1x, e1y});
    }
    return tris;
}

} // namespace stereo
