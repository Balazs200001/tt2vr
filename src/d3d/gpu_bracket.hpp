// d3d::GpuBracket: GPU time of work we record directly on the immediate context between Begin and End (our own
// Present-time work: the eye copy and HUD panel, PLAN 9d / 9e). The game's frame timer (game::GpuTimers) ends at its
// OnFlushRender and does not include it. Ring of 4 frames, read back without waiting; a frame whose slot is still
// in flight is not timed.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

namespace d3d {

// Thread: game main thread (Present hook). Not thread-safe.
class GpuBracket {
public:
    void Begin(ID3D11Device* device, ID3D11DeviceContext* context);
    void End(ID3D11DeviceContext* context);
    // Average ms of the brackets read back since the previous call (count = how many); -1 if none.
    double TakeAverageMs(int* count = nullptr);
    void Release();

private:
    void ReadBack(ID3D11DeviceContext* context);

    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint, begin, end;
        bool pending = false;
    };
    static constexpr int kSlots = 4;
    Slot m_slots[kSlots];
    int m_next = 0;
    int m_open = -1; // slot between Begin and End
    bool m_failed = false; // CreateQuery failed: stay off
    double m_sumMs = 0;
    int m_count = 0;
};

} // namespace d3d
