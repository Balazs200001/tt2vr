#include "game/cull_probe.hpp"

#include "core/config.hpp"
#include "core/hooks.hpp"
#include "core/log.hpp"
#include "game/engine_layout.hpp"
#include "game/signatures.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <intrin.h>
#include <windows.h>

namespace game {
namespace {

using namespace layout;

// (culler, ViewContext*, Plane*, Plane*, flags, RenderInstanceManager*). The main views pass RenderLoop+0x1e74 | 0xe38;
// cascades | 0x40, the static shadow map | 0x80, local-light shadows | 0x100 (renderer.md 10, DeferredRenderLoop).
using AddCullingDataFn = void (*)(void* culler, const void* viewContext, const void* p0, const void* p1,
    std::uint32_t flags, void* instances);
constexpr std::uint32_t kMainViewFlags = 0xe38;
constexpr std::ptrdiff_t kViewFrustumPlanes = 0x350; // Math::Frustum (BuildFromViewProj), 6 planes x float4 first

AddCullingDataFn g_addCullingData = nullptr;
using StartCullingFn = void (*)(void* spawner, void* scene, const void* desc, const void* views, void* waitGroup);
StartCullingFn g_startCulling = nullptr;

// GenericInstanceSpawner::StartCulling calls since the last summary (one Present): who called and for which views.
struct CullCall {
    std::uintptr_t caller = 0; // return address - exe base
    int views = 0;
    int cameraIndex[4] = {}, viewType[4] = {};
};
constexpr int kMaxCalls = 6;
CullCall g_calls[kMaxCalls];
int g_callCount = 0;
void* const* g_spawner = nullptr; // &AbstractGenericInstanceSpawner::s_pInstance

// GenericInstanceSpawner (static, delegated RE 2026-10-22; trees and bushes): per view slot (camera index x 13 + view
// type) the merged visible group array {+0x118 + slot*0x10: count +4, data +8}; per camera index the billboard array
// count at +0xdc + idx*0x10; the per-chunk entries (count +0x10c, data +0x110, stride 0x18, view mask +8).
constexpr std::ptrdiff_t kViewType = 0x460;
constexpr std::ptrdiff_t kSpawnerSlotCount = 0x11c, kSpawnerBillboardCount = 0xdc;
constexpr std::ptrdiff_t kSpawnerChunkCount = 0x10c, kSpawnerChunks = 0x110;
constexpr int kSlotsPerCamera = 13;

struct SpawnerCounts {
    int groups = -1, billboards = -1, chunks = -1; // -1 = not readable
};

// SEH: engine data from static RE.
SpawnerCounts ReadSpawner(void* spawner, int cameraIndex, int viewType)
{
    SpawnerCounts c;
    __try {
        const int slot = cameraIndex * kSlotsPerCamera + viewType;
        const auto* s = static_cast<const char*>(spawner);
        c.groups = *reinterpret_cast<const int*>(s + slot * 0x10 + kSpawnerSlotCount);
        c.billboards = *reinterpret_cast<const int*>(s + cameraIndex * 0x10 + kSpawnerBillboardCount);
        const int count = *reinterpret_cast<const int*>(s + kSpawnerChunkCount);
        const char* chunks = *reinterpret_cast<const char* const*>(s + kSpawnerChunks);
        int visible = 0;
        for (int i = 0; chunks && i < count && i < 100000; ++i) {
            const auto mask = *reinterpret_cast<const std::uint32_t*>(chunks + i * 0x18 + 8);
            visible += (mask >> (slot & 31)) & 1u;
        }
        c.chunks = visible;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        c = {};
    }
    return c;
}

struct ViewRecord {
    bool culled = false;
    bool rendered = false;
    float view[16] = {}; // at AddCullingData
    float planes[24] = {};
    float renderDiff = 0.0f; // max |rendered view - culled view| element
    float planesMoved = 0.0f; // degrees, largest plane-normal turn since the previous frame
    SpawnerCounts spawner; // at render time
};
ViewRecord g_now[2], g_prev[2];

float NormalAngleDeg(const float* a, const float* b)
{
    const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    const float lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (la < 1e-6f || lb < 1e-6f) {
        return 0.0f;
    }
    const float c = std::clamp((a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb), -1.0f, 1.0f);
    return std::acos(c) * 57.2957795f;
}

// Thread: game main thread (scene phase of DeferredRenderLoop / ForwardRenderLoop::Render).
void HookedAddCullingData(void* culler, const void* viewContext, const void* p0, const void* p1, std::uint32_t flags,
    void* instances)
{
    if (viewContext && (flags & kMainViewFlags) == kMainViewFlags) {
        const auto index = Read<std::uint16_t>(viewContext, kViewCameraIndex);
        if (index < 2) {
            ViewRecord& r = g_now[index];
            r.culled = true;
            std::memcpy(r.view, static_cast<const char*>(viewContext) + kViewTransform + kTmView, sizeof(r.view));
            std::memcpy(r.planes, static_cast<const char*>(viewContext) + kViewFrustumPlanes, sizeof(r.planes));
            if (g_prev[index].culled) {
                float moved = 0.0f;
                for (int p = 0; p < 6; ++p) {
                    moved = std::max(moved, NormalAngleDeg(&r.planes[p * 4], &g_prev[index].planes[p * 4]));
                }
                r.planesMoved = moved;
            }
        }
    }
    g_addCullingData(culler, viewContext, p0, p1, flags, instances);
}

// Thread: game main thread (DeferredRenderLoop / ForwardRenderLoop / other scene renders).
void HookedStartCulling(void* spawner, void* scene, const void* desc, const void* views, void* waitGroup)
{
    if (g_callCount < kMaxCalls) {
        CullCall& c = g_calls[g_callCount];
        c = {};
        c.caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) -
                   reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (views) {
            const int count = Read<int>(views, 4);
            const void* const* data = Read<const void* const*>(views, 8);
            c.views = count;
            for (int i = 0; data && i < count && i < 4; ++i) {
                if (data[i]) {
                    c.cameraIndex[i] = Read<std::uint16_t>(data[i], kViewCameraIndex);
                    c.viewType[i] = Read<std::uint16_t>(data[i], kViewType);
                }
            }
        }
    }
    ++g_callCount;
    g_startCulling(spawner, scene, desc, views, waitGroup);
}

} // namespace

void InstallCullProbe()
{
    static bool s_done = false;
    if (s_done || !core::GetConfig().debug.flash_detect) {
        return;
    }
    s_done = true;
    void* add = SignatureAddress(Sig::VisibilityCullerAddCullingData);
    if (!add || core::hooks::Create("VisibilityCuller::AddCullingData", add, &HookedAddCullingData,
                    &g_addCullingData) != core::hooks::Result::Ok) {
        TT2VR_LOG_WARN("cull probe: AddCullingData not hooked; flash reports carry no culling data");
        return;
    }
    g_spawner = static_cast<void* const*>(SignatureAddress(Sig::GenericInstanceSpawnerInstance));
    void* start = SignatureAddress(Sig::GenericInstanceSpawnerStartCulling);
    if (!start || core::hooks::Create("GenericInstanceSpawner::StartCulling", start, &HookedStartCulling,
                      &g_startCulling) != core::hooks::Result::Ok) {
        TT2VR_LOG_WARN("cull probe: StartCulling not hooked");
    }
    TT2VR_LOG_INFO("cull probe: hooks installed ([debug] flash_detect){}", g_spawner ? "" : "; no spawner counts");
}

void CullProbeLoopBegin()
{
    for (int k = 0; k < 2; ++k) {
        if (g_now[k].culled) {
            g_prev[k] = g_now[k];
        }
        g_now[k] = {};
    }
}

void CullProbeRenderView(const void* viewContext)
{
    if (!g_addCullingData || !viewContext) {
        return;
    }
    const auto index = Read<std::uint16_t>(viewContext, kViewCameraIndex);
    if (index >= 2 || g_now[index].rendered) {
        return;
    }
    ViewRecord& r = g_now[index];
    r.rendered = true;
    if (g_spawner && *g_spawner) {
        r.spawner = ReadSpawner(*g_spawner, index, Read<std::uint16_t>(viewContext, kViewType));
    }
    if (!r.culled) {
        return;
    }
    float view[16];
    std::memcpy(view, static_cast<const char*>(viewContext) + kViewTransform + kTmView, sizeof(view));
    float diff = 0.0f;
    for (int i = 0; i < 16; ++i) {
        diff = std::max(diff, std::fabs(view[i] - r.view[i]));
    }
    r.renderDiff = diff;
}

std::string CullProbeSummary()
{
    if (!g_addCullingData) {
        return {};
    }
    std::string s;
    for (int k = 0; k < 2; ++k) {
        const ViewRecord& r = g_now[k];
        s += fmt::format("{}v{} {}", k ? "; " : "", k, r.culled ? "culled" : "NOT culled");
        if (r.culled) {
            s += fmt::format(", render vs cull view {:.4f}, planes moved {:.1f} deg", r.renderDiff, r.planesMoved);
        }
        if (!r.rendered) {
            s += ", not rendered";
        }
        else if (r.spawner.groups >= 0) {
            s += fmt::format(", spawner groups {} billboards {} chunks {}", r.spawner.groups, r.spawner.billboards,
                r.spawner.chunks);
        }
    }
    if (g_startCulling) {
        s += fmt::format("; spawner culling x{}:", g_callCount);
        for (int i = 0; i < g_callCount && i < kMaxCalls; ++i) {
            const CullCall& c = g_calls[i];
            s += fmt::format(" [TT2+{:#x}, {} view(s)", c.caller, c.views);
            for (int v = 0; v < c.views && v < 4; ++v) {
                s += fmt::format(" c{}/t{}", c.cameraIndex[v], c.viewType[v]);
            }
            s += "]";
        }
        g_callCount = 0; // one summary per Present
    }
    return s;
}

} // namespace game
