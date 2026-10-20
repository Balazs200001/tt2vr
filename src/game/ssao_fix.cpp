#include "game/ssao_fix.hpp"

#include "core/config.hpp"
#include "core/hooks.hpp"
#include "core/log.hpp"
#include "game/engine_layout.hpp"
#include "game/signatures.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>

namespace game {
namespace {

using namespace layout;

using UpdateConstantsFn = void (*)(void* ssao, int pass, const float* proj);
// RenderLayer vtable slot +0x70: (layer, flags, constant buffer, -1, 0) -> CPU memory for that buffer's data.
using AllocateCbFn = void* (*)(void* layer, std::uintptr_t flags, void* buffer, std::uintptr_t a, std::uintptr_t b);

UpdateConstantsFn g_updateConstants = nullptr;
AllocateCbFn g_allocateCb = nullptr;
bool g_active = false;
std::atomic<bool> g_logged{false};

thread_local bool t_inUpdate = false;
thread_local float* t_constants = nullptr;

constexpr int kNdcToViewAdd = 10; // float index in g_ASSAOConsts (after pixel sizes, depth unpack, tan, mul)

// Thread: whoever records (main thread or render jobs); cheap outside UpdateConstants.
void* HookedAllocateCb(void* layer, std::uintptr_t flags, void* buffer, std::uintptr_t a, std::uintptr_t b)
{
    void* data = g_allocateCb(layer, flags, buffer, a, b);
    if (t_inUpdate && !t_constants) {
        t_constants = static_cast<float*>(data);
    }
    return data;
}

// Thread: the thread recording the AO pass (IntelSSAO::Render / GenerateSSAO).
void HookedUpdateConstants(void* ssao, int pass, const float* proj)
{
    t_inUpdate = true;
    t_constants = nullptr;
    g_updateConstants(ssao, pass, proj);
    t_inUpdate = false;
    float* c = t_constants;
    t_constants = nullptr;
    if (!c || !proj) {
        return;
    }
    // proj: engine Matrix44, row-major storage of the column-vector matrix: m00 [0], m02 [2], m11 [5], m12 [6].
    const float m00 = proj[0], m02 = proj[2], m11 = proj[5], m12 = proj[6];
    if (m00 == 0.0f || m11 == 0.0f || (std::fabs(m02) < 1e-6f && std::fabs(m12) < 1e-6f)) {
        return; // symmetric: the game's values are already exact
    }
    const float before[2] = {c[kNdcToViewAdd], c[kNdcToViewAdd + 1]};
    c[kNdcToViewAdd] = (m02 - 1.0f) / m00;
    c[kNdcToViewAdd + 1] = (1.0f + m12) / m11;
    if (!g_logged.exchange(true)) {
        TT2VR_LOG_INFO("ssao: NDCToViewAdd ({:.4f}, {:.4f}) -> ({:.4f}, {:.4f}) for an off-centre projection "
                       "(m02 {:.4f}, m12 {:.4f})",
            before[0], before[1], c[kNdcToViewAdd], c[kNdcToViewAdd + 1], m02, m12);
    }
}

void* g_ssaoEnable = nullptr; // CVar<bool> Engine.Renderer.SSAO.Enable
bool g_ssaoOverridden = false;

using UpdateSlotFn = void (*)(void* cvar, unsigned short index, char notify);

} // namespace

void UpdateSsaoOverride(bool vrActive)
{
    if (!g_ssaoEnable) {
        g_ssaoEnable = SignatureAddress(Sig::SsaoEnable);
        if (!g_ssaoEnable) {
            return;
        }
    }
    const bool off = vrActive && !core::GetConfig().xr.ssao;
    if (off) {
        if (!g_ssaoOverridden) {
            g_ssaoOverridden = true;
            TT2VR_LOG_INFO("ssao: off while the headset session runs ([xr] ssao = false; game value {})",
                Read<std::uint8_t>(g_ssaoEnable, kCVarBoolValue));
        }
        Write<std::uint8_t>(g_ssaoEnable, kCVarBoolValue, 0);
        Write<std::uint8_t>(g_ssaoEnable, kCVarBoolValue + 1, 0);
        return;
    }
    if (g_ssaoOverridden) {
        g_ssaoOverridden = false;
        const auto update = reinterpret_cast<UpdateSlotFn>((*static_cast<void* const* const*>(g_ssaoEnable))[kCVarUpdateSlot]);
        for (unsigned short i = 0; i < 2; ++i) {
            update(g_ssaoEnable, i, 0); // slot = the CVar's value again
        }
        TT2VR_LOG_INFO("ssao: game setting restored ({})", Read<std::uint8_t>(g_ssaoEnable, kCVarBoolValue));
    }
}

void InstallSsaoFix()
{
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;
    void* update = SignatureAddress(Sig::IntelSsaoUpdateConstants);
    auto* const* layerInstance = static_cast<void* const*>(SignatureAddress(Sig::RenderLayerInstance));
    void* layer = layerInstance ? *layerInstance : nullptr;
    if (!update || !layer) {
        TT2VR_LOG_WARN("ssao: signatures or RenderLayer missing; AO keeps the symmetric-frustum constants");
        return;
    }
    void* allocate = (*static_cast<void* const* const*>(layer))[kRenderLayerAllocateCbSlot];
    using core::hooks::Result;
    g_active =
        core::hooks::Create("RenderLayer vtbl+0x70 (constant buffer data)", allocate, &HookedAllocateCb,
            &g_allocateCb) == Result::Ok &&
        core::hooks::Create("IntelSSAO::UpdateConstants", update, &HookedUpdateConstants, &g_updateConstants) ==
            Result::Ok;
    if (g_active) {
        TT2VR_LOG_INFO("ssao: off-centre projection fix installed");
    }
    else {
        TT2VR_LOG_WARN("ssao: hook install failed; AO keeps the symmetric-frustum constants");
    }
}

} // namespace game
