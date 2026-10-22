// Headset profile: the last real headset's per-eye FOV, IPD and recommended eye size. Saved when a real session
// first locates its views (<game>\tt2vr\headset_profile.txt); the simulated headset (test mode) renders with it, so
// test frames have the real eyes' projections.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xr {

struct HeadsetProfile {
    std::string system = "default (no real session saved yet)";
    std::uint32_t recommendedW = 2528, recommendedH = 2704;
    float ipd = 0.063f; // m, distance between the eye positions
    // XrFovf per eye in radians: left, right, up, down (left/down negative).
    float fov[2][4] = {{-0.9076f, 0.7505f, 0.7679f, -0.8727f}, {-0.7505f, 0.9076f, 0.7679f, -0.8727f}};
    // XR_KHR_visibility_mask hidden-area triangles per eye (x, y on the view's z = -1 plane), empty if unknown.
    std::vector<float> mask[2];
};

// Thread: game main thread. Defaults when the file is missing or unreadable.
HeadsetProfile LoadHeadsetProfile();
void SaveHeadsetProfile(const HeadsetProfile& profile);

} // namespace xr
