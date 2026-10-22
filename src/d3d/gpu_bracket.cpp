#include "d3d/gpu_bracket.hpp"

#include "core/log.hpp"
#include "d3d/internal.hpp"

#include <cstdint>

namespace d3d {

void GpuBracket::Begin(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (m_failed || !device || !context) {
        return;
    }
    InternalScope internal;
    ReadBack(context);
    Slot& slot = m_slots[m_next];
    if (slot.pending) {
        return; // still in flight four frames later: skip this frame
    }
    if (!slot.disjoint) {
        D3D11_QUERY_DESC dq{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        D3D11_QUERY_DESC tq{D3D11_QUERY_TIMESTAMP, 0};
        if (FAILED(device->CreateQuery(&dq, &slot.disjoint)) || FAILED(device->CreateQuery(&tq, &slot.begin)) ||
            FAILED(device->CreateQuery(&tq, &slot.end))) {
            TT2VR_LOG_WARN("gpu bracket: CreateQuery failed; Present-time GPU work is not timed");
            Release();
            m_failed = true;
            return;
        }
    }
    context->Begin(slot.disjoint.Get());
    context->End(slot.begin.Get());
    m_open = m_next;
}

void GpuBracket::End(ID3D11DeviceContext* context)
{
    if (m_open < 0 || !context) {
        return;
    }
    InternalScope internal;
    Slot& slot = m_slots[m_open];
    context->End(slot.end.Get());
    context->End(slot.disjoint.Get());
    slot.pending = true;
    m_open = -1;
    m_next = (m_next + 1) % kSlots;
}

void GpuBracket::ReadBack(ID3D11DeviceContext* context)
{
    constexpr UINT kNoFlush = D3D11_ASYNC_GETDATA_DONOTFLUSH;
    for (Slot& slot : m_slots) {
        if (!slot.pending) {
            continue;
        }
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (context->GetData(slot.disjoint.Get(), &dj, sizeof(dj), kNoFlush) != S_OK) {
            continue;
        }
        UINT64 t0 = 0, t1 = 0;
        if (context->GetData(slot.begin.Get(), &t0, sizeof(t0), kNoFlush) != S_OK ||
            context->GetData(slot.end.Get(), &t1, sizeof(t1), kNoFlush) != S_OK) {
            continue;
        }
        slot.pending = false;
        if (!dj.Disjoint && dj.Frequency != 0 && t1 >= t0) {
            m_sumMs += static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dj.Frequency);
            ++m_count;
        }
    }
}

double GpuBracket::TakeAverageMs(int* count)
{
    const double avg = m_count > 0 ? m_sumMs / m_count : -1.0;
    if (count) {
        *count = m_count;
    }
    m_sumMs = 0;
    m_count = 0;
    return avg;
}

void GpuBracket::Release()
{
    for (Slot& slot : m_slots) {
        slot = {};
    }
    m_next = 0;
    m_open = -1;
}

} // namespace d3d
