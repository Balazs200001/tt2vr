#include "stereo/math/cut_detector.hpp"

#include <algorithm>
#include <cmath>

namespace stereo {
namespace {

float Distance(const Vec3& a, const Vec3& b)
{
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

CutDetector::Result CutDetector::Update(const Vec3& pos, const Vec3& fwd, const Vec3* shift)
{
    Result r;
    if (!m_have) {
        m_have = true;
        m_pos = pos;
        m_fwd = fwd;
        r.first = true;
        return r;
    }
    r.unrebasedDistance = r.distance = Distance(pos, m_pos);
    if (shift) {
        const Vec3 rebased{m_pos.x - shift->x, m_pos.y - shift->y, m_pos.z - shift->z};
        const float step = Distance(pos, rebased);
        if (step < r.distance) {
            r.distance = step;
            r.rebased = true;
        }
    }
    const float recent = *std::max_element(std::begin(m_steps), std::end(m_steps));
    r.limit = kMinDistance + kStepFactor * recent;
    const float cosTurn = std::clamp(fwd.x * m_fwd.x + fwd.y * m_fwd.y + fwd.z * m_fwd.z, -1.0f, 1.0f);
    r.turnDeg = std::acos(cosTurn) * 57.2957795f;
    // A step that repeats the previous frame's cut step without turning is the camera's speed: a window starved by a
    // standstill (pause) otherwise made every step at speed a cut (2026-10-22: 13 resets in a row after a pause).
    const bool repeat = m_lastCutStep > 0.0f && r.distance >= kRepeatMin * m_lastCutStep &&
                        r.distance <= kRepeatMax * m_lastCutStep && r.turnDeg <= kMaxTurnDeg;
    r.cut = !repeat && (r.distance > r.limit || r.turnDeg > kMaxTurnDeg);
    m_lastCutStep = r.cut ? r.distance : 0.0f;
    if (!r.cut) {
        m_steps[m_next] = std::min(r.distance, kMaxRememberedStep);
        m_next = (m_next + 1) % kWindow;
    }
    m_pos = pos;
    m_fwd = fwd;
    return r;
}

void CutDetector::Reset()
{
    m_have = false;
    std::fill(std::begin(m_steps), std::end(m_steps), 0.0f);
    m_next = 0;
    m_lastCutStep = 0.0f;
}

} // namespace stereo
