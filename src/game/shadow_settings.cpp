#include "game/shadow_settings.hpp"

#include "core/config.hpp"
#include "core/hooks.hpp"
#include "core/log.hpp"
#include "game/engine_layout.hpp"
#include "game/signatures.hpp"
#include "game/version.hpp"

#include <algorithm>
#include <cstdint>

namespace game {
namespace {

using namespace layout;

using ConfigureRendererFn = int (*)(const void* engine, void* rendererDesc, const void* graphicsDesc);
using MemberIntFn = int (*)(void* self);

ConfigureRendererFn g_configureRenderer = nullptr;
MemberIntFn g_updateShadowQuality = nullptr;
MemberIntFn g_createStaticShadowMap = nullptr;
void* g_cascadeSize = nullptr; // CVar<int>
void* g_staticSize = nullptr; // CVar<uint>
int g_scale = 1;

constexpr int kMaxCascadeSize = 4096; // 3 cascades side by side: 3 * 4096 <= 16384 (D3D11 texture width limit)

// Multiplies the CVar's slot-0 value for the lifetime of the scope (main thread; the engine reads slot 0 there).
class ScaledSlot {
public:
    ScaledSlot(void* cvar, int cap, const char* name) : m_cvar(cvar)
    {
        if (!m_cvar || g_scale <= 1) {
            m_cvar = nullptr;
            return;
        }
        m_saved = Read<int>(m_cvar, kCVarIntSlots);
        const int scaled = std::min(m_saved * g_scale, cap);
        Write(m_cvar, kCVarIntSlots, scaled);
        TT2VR_LOG_INFO("shadows: {} {} x{} -> {} for map creation", name, m_saved, g_scale, scaled);
    }
    ~ScaledSlot()
    {
        if (m_cvar) {
            Write(m_cvar, kCVarIntSlots, m_saved);
        }
    }
    ScaledSlot(const ScaledSlot&) = delete;
    ScaledSlot& operator=(const ScaledSlot&) = delete;

private:
    void* m_cvar;
    int m_saved = 0;
};

// Thread: game main thread (GameEngine::CreateApplication, before "Creating renderer").
int HookedConfigureRenderer(const void* engine, void* rendererDesc, const void* graphicsDesc)
{
    ScaledSlot scale(g_cascadeSize, kMaxCascadeSize, "CascadeShadowMapSize");
    return g_configureRenderer(engine, rendererDesc, graphicsDesc);
}

// Thread: game main thread (shadow quality option applied).
int HookedUpdateShadowQuality(void* renderer)
{
    ScaledSlot scale(g_cascadeSize, kMaxCascadeSize, "CascadeShadowMapSize");
    return g_updateShadowQuality(renderer);
}

// Thread: game main thread (Renderer::OnRender resize block, on every split change).
int HookedCreateStaticShadowMap(void* csm)
{
    ScaledSlot scale(g_staticSize, 16384, "StaticShadow.Size");
    return g_createStaticShadowMap(csm);
}

} // namespace

void InstallShadowScaleHooks()
{
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;
    const auto& xc = core::GetConfig().xr;
    g_scale = xc.shadow_scale;
    if (!xc.enabled || g_scale <= 1 || !EngineHooksAllowed()) {
        return;
    }
    ResolveSignatures();
    g_cascadeSize = SignatureAddress(Sig::CascadeShadowMapSize);
    g_staticSize = SignatureAddress(Sig::StaticShadowMapSize);
    using core::hooks::Result;
    int installed = 0;
    if (g_cascadeSize && SignatureAddress(Sig::GameEngineConfigureRenderer) &&
        core::hooks::Create("GameEngine::ConfigureRenderer", SignatureAddress(Sig::GameEngineConfigureRenderer),
            &HookedConfigureRenderer, &g_configureRenderer) == Result::Ok) {
        ++installed;
    }
    if (g_cascadeSize && SignatureAddress(Sig::RendererUpdateShadowQuality) &&
        core::hooks::Create("Renderer::UpdateShadowQuality", SignatureAddress(Sig::RendererUpdateShadowQuality),
            &HookedUpdateShadowQuality, &g_updateShadowQuality) == Result::Ok) {
        ++installed;
    }
    if (g_staticSize && SignatureAddress(Sig::CsmCreateStaticShadowMap) &&
        core::hooks::Create("CascadedShadowMapRenderer::CreateStaticShadowMap",
            SignatureAddress(Sig::CsmCreateStaticShadowMap), &HookedCreateStaticShadowMap,
            &g_createStaticShadowMap) == Result::Ok) {
        ++installed;
    }
    TT2VR_LOG_INFO("shadows: x{} shadow map scale for VR, {} of 3 hooks installed", g_scale, installed);
}

} // namespace game
