// stereo::CutDetector: decides from the game camera's position and forward axis, once per frame, whether the frame is a
// camera cut (the engine has none, docs/re/temporal.md N6 and 11.3). Pure code, unit-tested in
// tests/test_cut_detector.cpp; game::TaaControl resets both eyes' history on a cut.
//
// Rules, per frame (step = distance the camera moved since the previous frame):
//   - World Shift: if a shift vector is given and subtracting it from the previous position shortens the step, the step
//     is measured after that rebase (the 2048 m floating-origin jump is no cut).
//   - Cut if the step is longer than kMinDistance + kStepFactor x the longest step of the last kWindow frames, or the
//     forward axis turns by more than kMaxTurnDeg.
//   - The limit follows the recent maximum, not the last step: in the headset the steps alternate between short and
//     about 2.7 m at speed (frame pacing; 2026-10-21 log, "2.7 m (limit 2.5), 0 deg" about 30 times in 13 s with a
//     last-step limit). Real in-ride cuts turn by 11-26 deg (crash camera, respawn) or jump from a standstill.
//   - A cut's step is not remembered (one cut does not raise the limit for the next frames), unless the next frame
//     repeats it (kRepeatMin..kRepeatMax of it, no turn): that is the camera's speed, e.g. resuming at speed after a
//     pause emptied the window, so it is no cut and is remembered.
#pragma once

#include "stereo/math/matrix.hpp"

namespace stereo {

class CutDetector {
public:
    static constexpr float kMinDistance = 2.0f; // m
    static constexpr float kStepFactor = 2.5f;
    static constexpr float kMaxTurnDeg = 10.0f;
    static constexpr int kWindow = 32; // frames
    static constexpr float kMaxRememberedStep = 10.0f; // m; longer non-cut steps are clamped
    static constexpr float kRepeatMin = 0.5f, kRepeatMax = 2.0f; // x the previous frame's cut step

    struct Result {
        bool cut = false;
        bool first = false; // no previous frame: nothing measured
        bool rebased = false; // the World Shift was applied to the step
        float distance = 0.0f; // m, after the rebase
        float unrebasedDistance = 0.0f; // m, before it
        float limit = 0.0f; // m
        float turnDeg = 0.0f;
    };

    // `fwd` = the camera's forward axis (unit length). `shift` = this frame's World Shift vector, or nullptr.
    Result Update(const Vec3& pos, const Vec3& fwd, const Vec3* shift = nullptr);
    // Forgets the previous frame and the recent steps.
    void Reset();

private:
    bool m_have = false;
    Vec3 m_pos{};
    Vec3 m_fwd{};
    float m_steps[kWindow] = {};
    int m_next = 0;
    float m_lastCutStep = 0.0f; // the previous frame's step if it was a cut, else 0
};

} // namespace stereo
