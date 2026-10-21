#include "game/shared_shadows.hpp"

#include "core/config.hpp"
#include "core/hooks.hpp"
#include "core/log.hpp"
#include "game/camera.hpp"
#include "game/engine_layout.hpp"
#include "game/signatures.hpp"
#include "game/split_view.hpp"

#include <cstdint>
#include <cstring>

namespace game {
namespace {

using namespace layout;

using SetupRenderContextFn = void (*)(void* csm, const void* views, void* lights, const void* shift);
using CsmRenderFn = void (*)(void* csm, void* renderContext, const void* desc);
using SetContextFn = void (*)(void* renderContext, const void* context);

SetupRenderContextFn g_setup = nullptr;
CsmRenderFn g_render = nullptr;
SetContextFn g_setContext = nullptr;
void** g_renderLayer = nullptr; // RenderLayer::s_pInstance (address of the pointer)
bool g_installed = false;

// View 0's context of this frame (render id g_keptFrame); captured only inside view 0's CSM::Render, because
// SetCascadedShadowContext has other callers (SimpleForwardRenderer, the no-cascade default context).
alignas(16) unsigned char g_kept[kCascadedShadowContextSize];
std::uint32_t g_keptFrame = 0;
bool g_haveKept = false;
bool g_capturing = false;
std::uint64_t g_shared = 0; // frames in which view 1 used view 0's shadows
bool g_loggedHeadFit = false;

// All three hooks in place (a partial install shares nothing) and two views this frame.
bool Sharing()
{
    return g_installed && SplitViewActive();
}

std::uint32_t RenderId()
{
    void* layer = g_renderLayer ? *g_renderLayer : nullptr;
    return layer ? Read<std::uint32_t>(layer, kRenderLayerRenderId) : 0;
}

// Thread: game main thread (DeferredLighting::SetupLights, scene phase: no current eye, union projection).
void HookedSetupRenderContext(void* csm, const void* views, void* lights, const void* shift)
{
    void* view0 = nullptr;
    if (Sharing() && views) {
        const int count = Read<int>(views, kArrayCount);
        void* const* data = Read<void* const*>(views, kArrayData);
        for (int i = 0; data && i < count && i < 2; ++i) {
            if (data[i] && Read<std::uint16_t>(data[i], kViewCameraIndex) == 0) {
                view0 = data[i];
                break;
            }
        }
    }
    const bool moved = view0 && BeginHeadCenterView(view0);
    g_setup(csm, views, lights, shift);
    if (moved) {
        EndHeadCenterView(view0);
        if (!g_loggedHeadFit) {
            g_loggedHeadFit = true;
            TT2VR_LOG_INFO("shared shadows: cascades fitted from the head centre");
        }
    }
}

// Thread: game main thread (per view, DeferredLighting::RenderDirectionalCascadedShadowMap).
void HookedCsmRender(void* csm, void* renderContext, const void* desc)
{
    const unsigned index = renderContext ? Read<std::uint16_t>(renderContext, kRenderContextCameraIndex) : 2;
    if (!Sharing() || index > 1) {
        g_render(csm, renderContext, desc);
        return;
    }
    if (index == 0) {
        g_capturing = true;
        g_render(csm, renderContext, desc);
        g_capturing = false;
        return;
    }
    if (!g_haveKept || g_keptFrame != RenderId()) {
        g_render(csm, renderContext, desc); // view 0 rendered no cascades this frame
        return;
    }
    g_setContext(renderContext, g_kept);
    if (g_shared++ == 0) {
        TT2VR_LOG_INFO("shared shadows: the right eye uses the left eye's cascaded shadow maps (render id {})",
            g_keptFrame);
    }
}

// Thread: game main thread.
void HookedSetContext(void* renderContext, const void* context)
{
    if (g_capturing && context) {
        std::memcpy(g_kept, context, sizeof(g_kept));
        g_keptFrame = RenderId();
        g_haveKept = true;
    }
    g_setContext(renderContext, context);
}

} // namespace

void InstallSharedShadows()
{
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;
    if (!core::GetConfig().xr.shared_shadows) {
        TT2VR_LOG_INFO("shared shadows: off ([xr] shared_shadows = false); each eye renders its own cascades");
        return;
    }
    void* setup = SignatureAddress(Sig::CsmSetupRenderContext);
    void* render = SignatureAddress(Sig::CsmRender);
    void* setContext = SignatureAddress(Sig::RenderContextSetCascadedShadowContext);
    g_renderLayer = static_cast<void**>(SignatureAddress(Sig::RenderLayerInstance));
    using core::hooks::Result;
    // SetCascadedShadowContext first: CSM::Render's index-1 path calls its trampoline.
    g_installed = setup && render && setContext && g_renderLayer &&
                  core::hooks::Create("RenderContext::SetCascadedShadowContext", setContext, &HookedSetContext,
                      &g_setContext) == Result::Ok &&
                  core::hooks::Create("CascadedShadowMapRenderer::Render", render, &HookedCsmRender, &g_render) ==
                      Result::Ok &&
                  core::hooks::Create("CascadedShadowMapRenderer::SetupRenderContext", setup, &HookedSetupRenderContext,
                      &g_setup) == Result::Ok;
    if (!g_installed) {
        TT2VR_LOG_WARN("shared shadows: hooks not installed; each eye renders its own cascades");
        return;
    }
    TT2VR_LOG_INFO("shared shadows: hooks installed");
}

} // namespace game
