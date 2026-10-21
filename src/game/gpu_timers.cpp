#include "game/gpu_timers.hpp"

#include "core/log.hpp"
#include "game/engine_layout.hpp"
#include "game/signatures.hpp"

#include <spdlog/fmt/fmt.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace game {
namespace {

using namespace layout;
using Microsoft::WRL::ComPtr;

// PlayAll::Graphics::SimpleGPUPerformanceQuery as AddProfilingEvent and its executor use it: +0 the query the
// executor ends, +0xc the GPU queue (0 = the main queue that is split into jobs). The engine never creates one.
struct EngineQuery {
    ID3D11Query* query = nullptr;
    std::uint32_t unknown8 = 0;
    std::int32_t queue = 0;
    std::uint64_t unknown10[2] = {};
};
static_assert(offsetof(EngineQuery, queue) == 0xc && sizeof(EngineQuery) == 0x20);

// (renderLayer, query, name): name != nullptr -> End(query) + BeginEvent(name), nullptr -> End(query) + EndEvent().
using AddProfilingEventFn = void (*)(void* renderLayer, EngineQuery* query, const char* name);

constexpr int kSlots = 4; // frames in flight; read back one or two frames later
constexpr int kEvents = 4; // view 0 begin / end, view 1 begin / end
constexpr const char* kViewNames[2] = {"tt2vr view 0", "tt2vr view 1"};

struct Slot {
    ComPtr<ID3D11Query> stamp[kEvents];
    EngineQuery event[kEvents]; // queued by address: must stay put until the frame's passes have executed
    int views = 0; // views whose begin and end were queued
    bool pending = false; // queued, not read back yet
    // The engine's GPU timer slot of this frame (Renderer+kRendererGpuTimerSlots): start, end, disjoint.
    const void* renderer = nullptr;
    std::ptrdiff_t engineSlot = 0;
    ID3D11Query* engine[3] = {};
};

AddProfilingEventFn g_addEvent = nullptr;
void** g_renderLayer = nullptr; // RenderLayer::s_pInstance (address of the pointer)
bool g_ready = false; // queries created
Slot g_slots[kSlots];
std::uint64_t g_frame = 0; // Present count, selects the recording slot
Slot* g_recording = nullptr; // inside the main DeferredRenderLoop::Render
int g_openIndex = -1; // camera index of the view whose begin was queued and not ended yet

GpuTimes g_last;
double g_sumView[2] = {};
int g_countView[2] = {}; // frames that measured view k (one-view frames have no view 1)
double g_sumFrame = 0;
int g_sumCount = 0;

void Queue(EngineQuery& event, const char* name)
{
    if (void* layer = g_renderLayer ? *g_renderLayer : nullptr) {
        g_addEvent(layer, &event, name);
    }
}

void EndOpenView()
{
    if (g_recording && g_openIndex >= 0) {
        Queue(g_recording->event[2 * (g_recording->views - 1) + 1], nullptr);
        g_openIndex = -1;
    }
}

// Thread: game main thread, Present post-hook (inside the engine's immediate-context lock).
bool ReadBack(Slot& slot, ID3D11DeviceContext* immediate, GpuTimes& out)
{
    // The engine reuses its slot four frames later and recreates the queries on a device reset: read only while
    // the Renderer still holds the same three.
    for (int i = 0; i < 3; ++i) {
        if (!slot.engine[i] || Read<ID3D11Query*>(slot.renderer, slot.engineSlot + i * 8) != slot.engine[i]) {
            return false;
        }
    }
    constexpr UINT kNoFlush = D3D11_ASYNC_GETDATA_DONOTFLUSH;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    if (immediate->GetData(slot.engine[2], &disjoint, sizeof(disjoint), kNoFlush) != S_OK || disjoint.Disjoint ||
        disjoint.Frequency == 0) {
        return false;
    }
    UINT64 t[kEvents] = {};
    UINT64 frame[2] = {};
    for (int i = 0; i < 2 * slot.views; ++i) {
        if (immediate->GetData(slot.stamp[i].Get(), &t[i], sizeof(UINT64), kNoFlush) != S_OK) {
            return false;
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (immediate->GetData(slot.engine[i], &frame[i], sizeof(UINT64), kNoFlush) != S_OK) {
            return false;
        }
    }
    for (int i = 1; i < 2 * slot.views; ++i) {
        if (t[i] < t[i - 1]) {
            return false; // not this frame's stamps (an event that never executed keeps an older value)
        }
    }
    const double toMs = 1000.0 / static_cast<double>(disjoint.Frequency);
    out = {};
    out.valid = true;
    out.views = slot.views;
    for (int v = 0; v < slot.views; ++v) {
        out.viewMs[v] = static_cast<double>(t[2 * v + 1] - t[2 * v]) * toMs;
    }
    out.frameMs = static_cast<double>(frame[1] - frame[0]) * toMs;
    return true;
}

} // namespace

void InstallGpuTimers()
{
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;
    g_addEvent = reinterpret_cast<AddProfilingEventFn>(SignatureAddress(Sig::RenderLayerAddProfilingEvent));
    g_renderLayer = static_cast<void**>(SignatureAddress(Sig::RenderLayerInstance));
    if (!g_addEvent || !g_renderLayer) {
        g_addEvent = nullptr;
        TT2VR_LOG_WARN("gpu timers: RenderLayer::AddProfilingEvent / s_pInstance missing; no per-view GPU times");
        return;
    }
    TT2VR_LOG_INFO("gpu timers: using RenderLayer::AddProfilingEvent for per-view timestamps");
}

void GpuTimersLoopBegin(void* renderer)
{
    g_recording = nullptr;
    g_openIndex = -1;
    if (!g_ready || !g_addEvent || !renderer) {
        return;
    }
    Slot& slot = g_slots[g_frame % kSlots];
    slot.views = 0;
    slot.pending = false;
    // OnRender began this frame's engine timer (Begin(disjoint), End(start)) before RenderLoop::Render.
    const auto engineFrame = Read<std::uint32_t>(renderer, kRendererGpuTimerFrame);
    slot.renderer = renderer;
    slot.engineSlot = kRendererGpuTimerSlots + static_cast<std::ptrdiff_t>(engineFrame & 3) * kRendererGpuTimerSlotSize;
    for (int i = 0; i < 3; ++i) {
        slot.engine[i] = Read<ID3D11Query*>(renderer, slot.engineSlot + i * 8);
    }
    g_recording = &slot;
}

void GpuTimersView(const void* viewContext)
{
    if (!g_recording || !viewContext) {
        return;
    }
    const int index = Read<std::uint16_t>(viewContext, kViewCameraIndex);
    // The main context can be prepared several times for one view; a third camera index does not exist.
    if (index == g_openIndex || g_recording->views >= 2) {
        return;
    }
    EndOpenView();
    Slot& slot = *g_recording;
    Queue(slot.event[2 * slot.views], kViewNames[slot.views]);
    ++slot.views;
    g_openIndex = index;
}

void GpuTimersLoopEnd()
{
    if (!g_recording) {
        return;
    }
    EndOpenView();
    g_recording->pending = g_recording->views > 0;
    g_recording = nullptr;
}

void GpuTimersPresent(ID3D11Device* device, ID3D11DeviceContext* immediate)
{
    if (!g_addEvent || !device || !immediate) {
        return;
    }
    if (!g_ready) {
        D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP, 0};
        for (Slot& slot : g_slots) {
            for (int i = 0; i < kEvents; ++i) {
                if (FAILED(device->CreateQuery(&desc, &slot.stamp[i]))) {
                    TT2VR_LOG_WARN("gpu timers: CreateQuery(TIMESTAMP) failed; no per-view GPU times");
                    g_addEvent = nullptr;
                    return;
                }
                slot.event[i].query = slot.stamp[i].Get();
            }
        }
        g_ready = true;
    }
    // Oldest first; a frame whose results are not there yet is retried on the next Present (never waited for).
    for (int age = kSlots - 1; age >= 1; --age) {
        Slot& slot = g_slots[(g_frame + kSlots - age) % kSlots];
        if (!slot.pending) {
            continue;
        }
        GpuTimes t;
        if (!ReadBack(slot, immediate, t)) {
            if (age == kSlots - 1) {
                slot.pending = false; // its queries are reissued next frame
            }
            continue;
        }
        slot.pending = false;
        g_last = t;
        for (int v = 0; v < t.views; ++v) {
            g_sumView[v] += t.viewMs[v];
            ++g_countView[v];
        }
        g_sumFrame += t.frameMs;
        ++g_sumCount;
    }
    ++g_frame;
}

int GpuTimersOpenView()
{
    return g_recording ? g_openIndex : -1;
}

GpuTimes LastGpuTimes()
{
    return g_last;
}

std::string GpuTimersPerfSummary()
{
    if (g_sumCount == 0) {
        return {};
    }
    std::string s = fmt::format("gpu view 0 {:.2f} ms", g_sumView[0] / std::max(g_countView[0], 1));
    if (g_countView[1] > 0) {
        s += fmt::format(", view 1 {:.2f} ms ({} two-view frames)", g_sumView[1] / g_countView[1], g_countView[1]);
    }
    s += fmt::format(", frame {:.2f} ms ({} frames)", g_sumFrame / g_sumCount, g_sumCount);
    g_sumView[0] = g_sumView[1] = g_sumFrame = 0;
    g_countView[0] = g_countView[1] = 0;
    g_sumCount = 0;
    return s;
}

} // namespace game
