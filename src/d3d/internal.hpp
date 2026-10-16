#pragma once

namespace d3d {

// Our own D3D calls (overlay, dump copies, ScreenGrab) pass through the context hooks too.
// While an InternalScope is alive on a thread, hooks forward without recording.
inline thread_local int t_internalDepth = 0;

struct InternalScope {
    InternalScope() { ++t_internalDepth; }
    ~InternalScope() { --t_internalDepth; }
    InternalScope(const InternalScope&) = delete;
    InternalScope& operator=(const InternalScope&) = delete;
};

inline bool InInternalCall()
{
    return t_internalDepth > 0;
}

} // namespace d3d
