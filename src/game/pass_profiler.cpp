#include "game/pass_profiler.hpp"

#include "core/hooks.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "game/engine_layout.hpp"
#include "game/gpu_timers.hpp"
#include "game/signatures.hpp"

#include <spdlog/fmt/fmt.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <vector>

#include <windows.h>

namespace game {
namespace {

using namespace layout;
using Microsoft::WRL::ComPtr;

// As in gpu_timers.cpp: PlayAll::Graphics::SimpleGPUPerformanceQuery {+0 query, +0xc queue}.
struct EngineQuery {
    ID3D11Query* query = nullptr;
    std::uint32_t unknown8 = 0;
    std::int32_t queue = 0;
    std::uint64_t unknown10[2] = {};
};
static_assert(sizeof(EngineQuery) == 0x20);

using AddProfilingEventFn = void (*)(void* renderLayer, EngineQuery* query, const char* name);
using PassFn = void (*)(void* renderLayer, void* pass);

AddProfilingEventFn g_addEvent = nullptr;
PassFn g_draw = nullptr, g_dispatch = nullptr, g_copy = nullptr;
bool g_installed = false;

// Pass layouts (renderer.md 9): DrawPass desc (name first) at +0x18, DispatchCommand name at +0x3b8; copies are unnamed.
constexpr std::ptrdiff_t kDrawPassName = 0x18;
constexpr std::ptrdiff_t kDispatchName = 0x3b8;
constexpr std::ptrdiff_t kRenderLayerImmediate = 0x268; // byte: passes execute at once (inside execution)

enum class Kind : std::uint8_t { Draw, Compute, Copy };
constexpr const char* kKindNames[] = {"draw", "compute", "copy"};
constexpr int kSlots = 4;
constexpr int kMaxPasses = 768; // timed passes per frame (2 events each); the queue holds 18112 entries

struct PassRecord {
    const char* name; // static engine literal, or null
    Kind kind;
    std::int8_t view; // 0, 1, or -1 = outside the views
};

struct Slot {
    std::unique_ptr<EngineQuery[]> events; // 2 per pass, queued by address: must stay put until executed
    std::vector<ComPtr<ID3D11Query>> stamps;
    ComPtr<ID3D11Query> disjoint;
    PassRecord passes[kMaxPasses];
    int count = 0;
    int dropped = 0; // passes beyond kMaxPasses
    bool open = false; // recording or waiting for its disjoint End
    bool pending = false; // ended, not read back yet
    int age = 0;
};

Slot g_slots[kSlots];
bool g_ready = false;
std::uint64_t g_frame = 0;
Slot* g_recording = nullptr;
int g_framesLeft = 0; // frames still to record for the request
int g_framesPending = 0; // recorded frames not read back yet
std::function<void(const std::string&)> g_done;

struct Sum {
    double ms[3] = {}; // view 0, view 1, frame
    int count[3] = {};
    Kind kind = Kind::Draw;
};
std::map<std::string, Sum> g_sums;
int g_framesRead = 0, g_framesLost = 0, g_droppedTotal = 0;

bool Immediate(void* layer)
{
    return Read<std::uint8_t>(layer, kRenderLayerImmediate) != 0;
}

// Brackets one recorded pass. Thread: the recording thread (game main thread).
template <Kind K>
void Timed(PassFn original, void* layer, void* pass, std::ptrdiff_t nameAt)
{
    Slot* slot = g_recording;
    if (!slot || !layer || !pass || Immediate(layer) || slot->count >= kMaxPasses) {
        if (slot && slot->count >= kMaxPasses) {
            ++slot->dropped;
        }
        original(layer, pass);
        return;
    }
    const int i = slot->count++;
    const char* name = nameAt >= 0 ? Read<const char*>(pass, nameAt) : nullptr;
    slot->passes[i] = {name, K, static_cast<std::int8_t>(GpuTimersOpenView())};
    g_addEvent(layer, &slot->events[2 * i], name ? name : kKindNames[static_cast<int>(K)]);
    original(layer, pass);
    g_addEvent(layer, &slot->events[2 * i + 1], nullptr);
}

void HookedDraw(void* layer, void* pass)
{
    Timed<Kind::Draw>(g_draw, layer, pass, kDrawPassName);
}
void HookedDispatch(void* layer, void* pass)
{
    Timed<Kind::Compute>(g_dispatch, layer, pass, kDispatchName);
}
void HookedCopy(void* layer, void* pass)
{
    Timed<Kind::Copy>(g_copy, layer, pass, -1);
}

bool CreateQueries(ID3D11Device* device)
{
    D3D11_QUERY_DESC stamp{D3D11_QUERY_TIMESTAMP, 0};
    D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    for (Slot& s : g_slots) {
        s.events = std::make_unique<EngineQuery[]>(2 * kMaxPasses);
        s.stamps.resize(2 * kMaxPasses);
        for (int i = 0; i < 2 * kMaxPasses; ++i) {
            if (FAILED(device->CreateQuery(&stamp, &s.stamps[i]))) {
                return false;
            }
            s.events[i].query = s.stamps[i].Get();
        }
        if (FAILED(device->CreateQuery(&disjoint, &s.disjoint))) {
            return false;
        }
    }
    return true;
}

// Reads a slot whose frame has executed. False = not ready yet.
bool ReadBack(Slot& s, ID3D11DeviceContext* immediate)
{
    constexpr UINT kNoFlush = D3D11_ASYNC_GETDATA_DONOTFLUSH;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    if (immediate->GetData(s.disjoint.Get(), &dj, sizeof(dj), kNoFlush) != S_OK) {
        return false;
    }
    std::vector<UINT64> t(2 * s.count);
    for (int i = 0; i < 2 * s.count; ++i) {
        if (immediate->GetData(s.stamps[i].Get(), &t[i], sizeof(UINT64), kNoFlush) != S_OK) {
            return false;
        }
    }
    if (dj.Disjoint || dj.Frequency == 0) {
        ++g_framesLost;
        return true;
    }
    const double toMs = 1000.0 / static_cast<double>(dj.Frequency);
    for (int i = 0; i < s.count; ++i) {
        const PassRecord& p = s.passes[i];
        if (t[2 * i + 1] < t[2 * i]) {
            continue; // an event that never executed keeps an older value
        }
        Sum& sum = g_sums[p.name ? p.name : kKindNames[static_cast<int>(p.kind)]];
        const int bucket = p.view == 0 || p.view == 1 ? p.view : 2;
        sum.ms[bucket] += static_cast<double>(t[2 * i + 1] - t[2 * i]) * toMs;
        ++sum.count[bucket];
        sum.kind = p.kind;
    }
    g_droppedTotal += s.dropped;
    ++g_framesRead;
    return true;
}

std::string Report()
{
    const double n = std::max(g_framesRead, 1);
    struct Row {
        std::string name;
        Sum sum;
        double total;
    };
    std::vector<Row> rows;
    double total[3] = {};
    for (const auto& [name, sum] : g_sums) {
        rows.push_back({name, sum, (sum.ms[0] + sum.ms[1] + sum.ms[2]) / n});
        for (int b = 0; b < 3; ++b) {
            total[b] += sum.ms[b] / n;
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.total > b.total; });
    const auto path = core::CaptureDir() / fmt::format("gpuprofile_{}.txt", g_frame);
    std::ofstream out(path, std::ios::trunc);
    out << fmt::format("# tt2vr GPU profile: {} frames read ({} disjoint, {} passes over the {} per-frame limit)\n",
        g_framesRead, g_framesLost, g_droppedTotal, kMaxPasses);
    out << "# ms per frame, summed over every pass of that name (count = passes per frame); 'frame' = recorded outside\n"
           "# the two views (shadow cascades, probes, HUD, ...). Pass times can overlap on the GPU, so the sum can\n"
           "# differ from the view and frame totals of game::GpuTimers.\n";
    out << fmt::format("{:<40} {:>8} {:>9} {:>9} {:>9}   {:>5} {:>5} {:>5}  kind\n", "pass", "total", "view 0", "view 1",
        "frame", "n0", "n1", "nf");
    for (const Row& r : rows) {
        out << fmt::format("{:<40} {:>8.3f} {:>9.3f} {:>9.3f} {:>9.3f}   {:>5.1f} {:>5.1f} {:>5.1f}  {}\n",
            r.name.substr(0, 40), r.total, r.sum.ms[0] / n, r.sum.ms[1] / n, r.sum.ms[2] / n, r.sum.count[0] / n,
            r.sum.count[1] / n, r.sum.count[2] / n, kKindNames[static_cast<int>(r.sum.kind)]);
    }
    out << fmt::format("{:<40} {:>8.3f} {:>9.3f} {:>9.3f} {:>9.3f}\n", "SUM", total[0] + total[1] + total[2], total[0],
        total[1], total[2]);
    const GpuTimes gpu = LastGpuTimes();
    if (gpu.valid) {
        out << fmt::format("# game::GpuTimers (last frame): view 0 {:.3f}, view 1 {:.3f}, engine frame bracket {:.3f} ms\n",
            gpu.viewMs[0], gpu.viewMs[1], gpu.frameMs);
    }
    std::string top;
    for (std::size_t i = 0; i < rows.size() && i < 6; ++i) {
        top += fmt::format("{}{} {:.2f}", i ? ", " : "", rows[i].name, rows[i].total);
    }
    return fmt::format("gpuprofile ok: {} frames, passes sum {:.2f} ms (view 0 {:.2f}, view 1 {:.2f}, frame {:.2f}); "
                       "top: {}; {}",
        g_framesRead, total[0] + total[1] + total[2], total[0], total[1], total[2], top, path.string());
}

} // namespace

void InstallPassProfiler()
{
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;
    g_addEvent = reinterpret_cast<AddProfilingEventFn>(SignatureAddress(Sig::RenderLayerAddProfilingEvent));
    void* draw = SignatureAddress(Sig::RenderLayerDrawCommandBuffer);
    void* dispatch = SignatureAddress(Sig::RenderLayerDispatch);
    void* copy = SignatureAddress(Sig::RenderLayerCopyTextureSlice);
    if (!g_addEvent || !draw || !dispatch || !copy) {
        TT2VR_LOG_WARN("pass profiler: signatures missing; no per-pass GPU profile");
        return;
    }
    using core::hooks::Result;
    g_installed =
        core::hooks::Create("RenderLayer::DrawCommandBuffer", draw, &HookedDraw, &g_draw) == Result::Ok &&
        core::hooks::Create("RenderLayer::Dispatch", dispatch, &HookedDispatch, &g_dispatch) == Result::Ok &&
        core::hooks::Create("RenderLayer::CopyTextureSlice", copy, &HookedCopy, &g_copy) == Result::Ok;
    if (g_installed) {
        TT2VR_LOG_INFO("pass profiler: hooks installed (idle until a gpuprofile request)");
    }
    else {
        TT2VR_LOG_ERROR("pass profiler: hook install failed");
    }
}

void RequestPassProfile(int frames, std::function<void(const std::string&)> done)
{
    if (!g_installed) {
        done("gpuprofile FAILED: profiler hooks not installed (see log)");
        return;
    }
    if (g_done) {
        done("gpuprofile FAILED: a profile is already running");
        return;
    }
    g_sums.clear();
    g_framesRead = g_framesLost = g_droppedTotal = 0;
    g_framesLeft = std::clamp(frames, 1, 600);
    g_done = std::move(done);
}

void PassProfilerPresent(ID3D11Device* device, ID3D11DeviceContext* immediate)
{
    if (!g_installed || !device || !immediate) {
        return;
    }
    ++g_frame;
    // The frame that ends here: close its disjoint bracket.
    if (g_recording) {
        immediate->End(g_recording->disjoint.Get());
        g_recording->open = false;
        g_recording->pending = true;
        g_recording->age = 0;
        g_recording = nullptr;
        ++g_framesPending;
    }
    for (Slot& s : g_slots) {
        if (!s.pending) {
            continue;
        }
        if (ReadBack(s, immediate)) {
            s.pending = false;
            --g_framesPending;
        }
        else if (++s.age > 8) {
            s.pending = false; // never completed (device reset?): give up on this frame
            --g_framesPending;
            ++g_framesLost;
        }
    }
    if (g_done && g_framesLeft == 0 && g_framesPending == 0) {
        const std::string result = Report();
        TT2VR_LOG_INFO("{}", result);
        auto done = std::move(g_done);
        g_done = nullptr;
        done(result);
    }
    if (g_framesLeft <= 0) {
        return;
    }
    if (!g_ready) {
        if (!CreateQueries(device)) {
            TT2VR_LOG_ERROR("pass profiler: CreateQuery failed");
            g_installed = false;
            g_framesLeft = 0;
            if (g_done) {
                auto done = std::move(g_done);
                g_done = nullptr;
                done("gpuprofile FAILED: CreateQuery failed");
            }
            return;
        }
        g_ready = true;
    }
    Slot& next = g_slots[g_frame % kSlots];
    if (next.pending || next.open) {
        return; // all four slots busy: skip recording this frame
    }
    next.count = 0;
    next.dropped = 0;
    next.open = true;
    immediate->Begin(next.disjoint.Get());
    g_recording = &next;
    --g_framesLeft;
}

} // namespace game
