// The one place where OpenXR space meets engine space (CLAUDE.md; docs/conventions.md section 8).
//
// Frames:
//   - XR reference/view space: right-handed, +X right, +Y up, +Z back (-Z forward), metres.
//   - Engine camera-local space: +X right, +Y up, +Z back. The SAME basis as XR view space
//     (conventions section 2), so a pose expressed relative to the camera needs no basis change.
//   - Engine world: right-handed, +X right, +Y forward, +Z up, 1 unit = 1 m (likely).
// Rules:
//   1. Camera-relative composition (head tracking, Phase 2 overlay offsets): eyeWorld = camera * eyeLocal,
//      eyeLocal in camera-local (= XR) axes. No B.
//   2. XR reference space to engine world axes: p_e = B * p_xr, R_e = B * R_xr (world side only; the local
//      side keeps the XR basis because it equals the engine camera basis). B maps x->x, -z->y, y->z.
#pragma once

#include "stereo/math/matrix.hpp"

namespace stereo {

struct Quat {
    float x, y, z, w; // XrQuaternionf order
};

struct Pose {
    Quat orientation;
    Vec3 position; // XrPosef layout
};

// Rigid transform of an XR pose (column vectors): columns 0..2 are the pose's local axes, column 3 the position.
Mat34 PoseToMatrix(const Pose& pose);

// Rotation/offset in camera-local axes: T(offset) * Ry(yaw) * Rx(pitch) * Rz(roll), angles in radians.
// Positive yaw turns left (forward -Z towards -X), positive pitch looks up, positive roll tilts the head left
// (counter-clockwise as seen by the viewer). The same convention as an XR orientation in a -Z-forward space.
Mat34 CameraLocalTransform(float yawRad, float pitchRad, float rollRad, const Vec3& offset);

// Rule 1: eye (or offset camera) world matrix from the engine camera world matrix and a camera-local transform.
Mat34 ComposeCameraLocal(const Mat34& cameraWorld, const Mat34& local);

// Rule 2: basis change B from XR reference axes to engine world axes.
Vec3 XrToEngineWorld(const Vec3& v);
Mat34 XrToEngineWorld(const Mat34& xrPose); // B * M

// Engine view matrix (world -> view) for a camera/eye world matrix: the 4x4 inverse.
Mat44 ViewFromWorld(const Mat34& world);

// Inverse of a rigid transform (orthonormal rotation): (R^T, -R^T * t).
Mat34 RigidInverse(const Mat34& a);

// Head tracking (PLAN Phase 3). All in XR reference-space axes (= camera-local axes, rule 1).
// Recenter reference from a head pose: keeps the head position and its yaw about +Y, drops pitch and roll so the
// horizon stays level. Looking straight up or down, the yaw comes from the head's up axis instead of its forward.
Mat34 RecenterFromHead(const Pose& head);
// Eye transform relative to the recenter reference: inverse(recenter) * eye. This is the camera-local transform that
// moves the engine camera to the eye (rule 1: eyeWorld = camera * EyeLocal).
Mat34 EyeLocal(const Mat34& recenter, const Pose& eye);
// Pose (XR reference space) of a virtual screen `distance` metres straight ahead of a recenter reference, facing it:
// same yaw, same height. Used for the menu screen quad layer (Phase 3 interim, PLAN Phase 7).
Pose ScreenPose(const Mat34& recenter, float distance);

} // namespace stereo
