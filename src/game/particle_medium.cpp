#include "game/particle_medium.hpp"

#include "core/hooks.hpp"
#include "core/log.hpp"
#include "game/signatures.hpp"
#include "game/split_view.hpp"

namespace game {
namespace {

using EnableMediumFn = void (*)(void* manager, int medium, bool enable);

constexpr int kSecondCameraMedium = 2; // the per-camera instances of camera index 1

EnableMediumFn g_enableMedium = nullptr;
bool g_logged = false;

// Thread: game main thread (GameContext::RenderOneFrame, before Renderer::DoUpdate).
void HookedEnableMedium(void* manager, int medium, bool enable)
{
    if (medium == kSecondCameraMedium && !enable && SplitViewActive()) {
        enable = true;
        if (!g_logged) {
            g_logged = true;
            TT2VR_LOG_INFO("particle medium: per-camera effects simulated for the right eye too (medium 2 on)");
        }
    }
    g_enableMedium(manager, medium, enable);
}

} // namespace

void InstallParticleMedium()
{
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;
    void* target = SignatureAddress(Sig::HHParticlesManagerEnableMedium);
    if (!target || core::hooks::Create("HHParticlesManager::EnableMedium", target, &HookedEnableMedium,
                       &g_enableMedium) != core::hooks::Result::Ok) {
        TT2VR_LOG_WARN("particle medium: hook not installed; per-camera particle effects show in the left eye only");
        return;
    }
    TT2VR_LOG_INFO("particle medium: hook installed");
}

} // namespace game
