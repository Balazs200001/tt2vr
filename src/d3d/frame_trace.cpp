// frame_trace.cpp - one frame of D3D11 calls, written to captures/frametrace_<frame>.txt.
// A request for N frames traces N consecutive frames, one file each (history audits compare frame N+1's reads with
// frame N's writes by object pointer).
//
// Window: from the return of Present(N) to the call of Present(N+1), so the file holds everything the
// game submits for frame N+1. Calls on deferred contexts are buffered per context, attached to their
// command list at FinishCommandList and spliced in at ExecuteCommandList, so the file is in the order
// the immediate context submits work (the GPU order), with the recording context named on each line.
#include "d3d/frame_trace.hpp"

#include "core/config.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "d3d/describe.hpp"
#include "d3d/shader_registry.hpp"
#include "game/version.hpp"

#include <spdlog/fmt/fmt.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace d3d::trace {
namespace {

constexpr std::size_t kMaxLines = 400000; // safety cap (a gameplay frame is ~40k calls)

struct Stream {
    std::string label;
    DWORD thread = 0;
    std::vector<std::string> lines;
};

struct ListRecord {
    std::string label;
    DWORD thread = 0;
    std::vector<std::string> lines;
};

struct Object {
    std::string id;
    std::string description;
};

struct PendingMap {
    void* data = nullptr;
    std::size_t bytes = 0;
};

struct State {
    std::mutex mutex;
    std::uint64_t frame = 0;
    ID3D11DeviceContext* immediate = nullptr;
    std::unordered_map<ID3D11DeviceContext*, Stream> streams;
    std::unordered_map<ID3D11CommandList*, ListRecord> lists;
    std::unordered_map<const void*, Object> objects;
    std::vector<const void*> objectOrder;
    std::map<std::pair<ID3D11DeviceContext*, ID3D11Resource*>, PendingMap> maps;
    int deferredCount = 0;
    int listCount = 0;
    int objectCounter = 0;
    std::size_t lineCount = 0;
    bool truncated = false;
    std::chrono::steady_clock::time_point start;
};

State g_state;
bool g_requested = false; // present thread only
int g_framesLeft = 0; // present thread only: frames still to trace for the pending requests
std::vector<std::string> g_results; // present thread only: one result line per traced frame
std::vector<DoneFn> g_done; // present thread only

// Caller holds the mutex.
Stream& StreamOf(ID3D11DeviceContext* ctx)
{
    auto [it, inserted] = g_state.streams.try_emplace(ctx);
    if (inserted) {
        it->second.label = ctx == g_state.immediate ? "imm" : fmt::format("def{}", ++g_state.deferredCount);
        it->second.thread = GetCurrentThreadId();
    }
    return it->second;
}

template <typename Describe>
std::string ObjectId(const void* object, const char* kind, Describe&& describe)
{
    if (!object) {
        return "null";
    }
    std::lock_guard lock(g_state.mutex);
    auto it = g_state.objects.find(object);
    if (it == g_state.objects.end()) {
        Object o{fmt::format("{}{}", kind, ++g_state.objectCounter), {}};
        it = g_state.objects.emplace(object, std::move(o)).first;
        g_state.objectOrder.push_back(object);
        // Describe outside of the id assignment so nested ids (a view's resource) can take the lock.
        g_state.mutex.unlock();
        std::string description = describe();
        g_state.mutex.lock();
        it = g_state.objects.find(object);
        it->second.description = std::move(description);
    }
    return it->second.id;
}

const char* ViewKind(ID3D11View* view)
{
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&rtv)))) {
        return "rtv";
    }
    if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&dsv)))) {
        return "dsv";
    }
    if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&srv)))) {
        return "srv";
    }
    return "uav";
}

std::string WriteFile(std::uint64_t frame, double ms)
{
    const auto& cfg = core::GetConfig();
    const auto dir = core::CaptureDir();
    const auto path = dir / fmt::format("frametrace_{}.txt", frame);
    std::string result;
    {
        std::ofstream out(path, std::ios::trunc);
        if (!out) {
            result = fmt::format("frametrace FAILED: cannot write {}", path.string());
        }
        else {
            const auto& version = game::ExeVersion();
            out << fmt::format("# tt2vr frame trace, frame {} ({:.2f} ms between Present calls)\n", frame, ms);
            out << fmt::format("# exe sha256 {} ({})\n", version.sha256,
                version.status == game::VersionStatus::Known ? "build " + version.steamBuild : "UNKNOWN build");
            out << "# order: immediate-context submission order; deferred lists are spliced in at ExecuteCommandList.\n";
            out << "# line: <index> <context> <call>; objects are listed at the end (first-sight descriptions).\n";
            out << fmt::format("# cb_floats = {}\n", cfg.capture.cb_floats);
            out << "# contexts:";
            for (const auto& [ctx, s] : g_state.streams) {
                out << fmt::format(" {}(thread {})", s.label, s.thread);
            }
            out << "\n\n";
            std::size_t index = 0;
            if (auto it = g_state.streams.find(g_state.immediate); it != g_state.streams.end()) {
                for (const auto& line : it->second.lines) {
                    out << fmt::format("{:06} {}\n", index++, line);
                }
            }
            for (const auto& [ctx, s] : g_state.streams) {
                if (ctx == g_state.immediate || s.lines.empty()) {
                    continue;
                }
                out << fmt::format("\n# unfinished recording on {} (thread {}), {} calls\n", s.label, s.thread,
                    s.lines.size());
                for (const auto& line : s.lines) {
                    out << fmt::format("{:06} {} {}\n", index++, s.label, line);
                }
            }
            for (const auto& [list, rec] : g_state.lists) {
                out << fmt::format("\n# command list {} finished on {} but not executed in this frame, {} calls\n",
                    rec.label, static_cast<const void*>(list), rec.lines.size());
                for (const auto& line : rec.lines) {
                    out << fmt::format("{:06} {}\n", index++, line);
                }
            }
            if (g_state.truncated) {
                out << fmt::format("\n# TRUNCATED at {} lines\n", kMaxLines);
            }
            out << "\n# objects\n";
            for (const void* o : g_state.objectOrder) {
                const auto& obj = g_state.objects[o];
                out << fmt::format("{} @{} = {}\n", obj.id, o, obj.description);
            }
            result = fmt::format("frametrace ok {} ({} calls, {} contexts, {} objects)", path.string(), index,
                g_state.streams.size(), g_state.objectOrder.size());
        }
    }
    shaders::WriteRegistry(dir / "shader_registry.txt");
    TT2VR_LOG_INFO("{}", result);
    return result;
}

} // namespace

void Request(DoneFn onDone, int frames)
{
    g_requested = true;
    g_framesLeft = std::max(g_framesLeft, std::clamp(frames, 1, kMaxFrames));
    g_done.push_back(std::move(onDone));
}

bool Pending()
{
    return g_requested || Active();
}

void Begin(std::uint64_t frame, ID3D11DeviceContext* immediate)
{
    if (!g_requested || Active()) {
        return;
    }
    g_requested = false;
    {
        std::lock_guard lock(g_state.mutex);
        g_state.frame = frame;
        g_state.immediate = immediate;
        g_state.streams.clear();
        g_state.lists.clear();
        g_state.objects.clear();
        g_state.objectOrder.clear();
        g_state.maps.clear();
        g_state.deferredCount = 0;
        g_state.listCount = 0;
        g_state.objectCounter = 0;
        g_state.lineCount = 0;
        g_state.truncated = false;
        g_state.start = std::chrono::steady_clock::now();
        StreamOf(immediate);
    }
    detail::g_active.store(true);
    TT2VR_LOG_INFO("frametrace: recording frame {}", frame);
}

void End(std::uint64_t frame)
{
    if (!Active()) {
        return;
    }
    detail::g_active.store(false);
    std::lock_guard lock(g_state.mutex);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_state.start).count();
    try {
        g_results.push_back(WriteFile(frame, ms));
    }
    catch (...) {
        TT2VR_LOG_ERROR("frametrace: exception while writing frame {}", frame);
        g_results.push_back(fmt::format("frametrace FAILED: exception while writing frame {}", frame));
    }
    g_state.streams.clear();
    g_state.lists.clear();
    g_state.objects.clear();
    g_state.objectOrder.clear();
    if (--g_framesLeft > 0) {
        g_requested = true; // the next frame (Begin runs after this Present returns)
        return;
    }
    g_framesLeft = 0;
    std::string result;
    for (const auto& r : g_results) {
        result += (result.empty() ? "" : "; ") + r;
    }
    g_results.clear();
    for (auto& done : g_done) {
        if (done) {
            done(result);
        }
    }
    g_done.clear();
}

void Line(ID3D11DeviceContext* ctx, std::string text)
{
    std::lock_guard lock(g_state.mutex);
    if (g_state.lineCount >= kMaxLines) {
        g_state.truncated = true;
        return;
    }
    ++g_state.lineCount;
    Stream& s = StreamOf(ctx);
    if (ctx == g_state.immediate) {
        s.lines.push_back("imm " + text);
    }
    else {
        s.lines.push_back(std::move(text));
    }
}

void FinishList(ID3D11DeviceContext* ctx, ID3D11CommandList* list)
{
    std::lock_guard lock(g_state.mutex);
    Stream& s = StreamOf(ctx);
    ListRecord rec{fmt::format("cl{}", ++g_state.listCount), s.thread, {}};
    rec.lines.reserve(s.lines.size() + 1);
    for (auto& line : s.lines) {
        rec.lines.push_back(fmt::format("{}   {}", s.label, line));
    }
    rec.lines.push_back(fmt::format("{}   FinishCommandList -> {}", s.label, rec.label));
    s.lines.clear();
    g_state.lists[list] = std::move(rec);
}

void ExecuteList(ID3D11DeviceContext* immediate, ID3D11CommandList* list, BOOL restore)
{
    std::lock_guard lock(g_state.mutex);
    Stream& s = StreamOf(immediate);
    auto it = g_state.lists.find(list);
    if (it == g_state.lists.end()) {
        s.lines.push_back(fmt::format("imm ExecuteCommandList {} restore={} (recorded before the trace window)",
            static_cast<const void*>(list), restore));
        return;
    }
    s.lines.push_back(fmt::format("imm ExecuteCommandList {} restore={} ({} calls recorded on thread {}) {{",
        it->second.label, restore, it->second.lines.size(), it->second.thread));
    for (auto& line : it->second.lines) {
        s.lines.push_back(std::move(line));
    }
    s.lines.push_back(fmt::format("imm }} end {}", it->second.label));
    g_state.lists.erase(it);
}

void MapBuffer(ID3D11DeviceContext* ctx, ID3D11Resource* resource, D3D11_MAP type, const D3D11_MAPPED_SUBRESOURCE& mapped)
{
    D3D11_RESOURCE_DIMENSION dim{};
    resource->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER || type != D3D11_MAP_WRITE_DISCARD) {
        return;
    }
    D3D11_BUFFER_DESC desc{};
    static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
    if (!(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) {
        return;
    }
    std::lock_guard lock(g_state.mutex);
    g_state.maps[{ctx, resource}] = PendingMap{mapped.pData, desc.ByteWidth};
}

void UnmapBuffer(ID3D11DeviceContext* ctx, ID3D11Resource* resource)
{
    PendingMap pending;
    {
        std::lock_guard lock(g_state.mutex);
        auto it = g_state.maps.find({ctx, resource});
        if (it == g_state.maps.end()) {
            return;
        }
        pending = it->second;
        g_state.maps.erase(it);
    }
    if (pending.data) {
        Line(ctx, fmt::format("  cb data {} B: {}", pending.bytes, Floats(pending.data, pending.bytes)));
    }
}

std::string Res(ID3D11Resource* resource)
{
    return ObjectId(resource, "res", [&] { return DescribeResource(resource); });
}

std::string View(ID3D11View* view)
{
    if (!view) {
        return "null";
    }
    return ObjectId(view, ViewKind(view), [&] {
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        view->GetResource(&resource);
        return fmt::format("{} of {}", DescribeView(view), Res(resource.Get()));
    });
}

std::string DepthState(ID3D11DepthStencilState* state)
{
    if (!state) {
        return "dss:default";
    }
    return ObjectId(state, "dss", [&] { return DescribeDepthStencilState(state); });
}

std::string RasterState(ID3D11RasterizerState* state)
{
    if (!state) {
        return "rs:default";
    }
    return ObjectId(state, "rs", [&] { return DescribeRasterizerState(state); });
}

std::string Floats(const void* data, std::size_t bytes)
{
    const int maxFloats = core::GetConfig().capture.cb_floats;
    if (!data || maxFloats <= 0) {
        return "-";
    }
    const std::size_t count = std::min<std::size_t>(bytes / sizeof(float), static_cast<std::size_t>(maxFloats));
    const float* f = static_cast<const float*>(data);
    std::string s;
    for (std::size_t i = 0; i < count; ++i) {
        s += (i % 4 == 0) ? "[" : " ";
        s += fmt::format("{:.6g}", f[i]);
        if (i % 4 == 3 || i + 1 == count) {
            s += "]";
        }
    }
    return s;
}

} // namespace d3d::trace
