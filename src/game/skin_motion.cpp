#include "game/skin_motion.hpp"

#include "core/hooks.hpp"
#include "core/log.hpp"
#include "game/engine_layout.hpp"
#include "game/signatures.hpp"

#include <atomic>
#include <cstdint>

namespace game {
namespace {

using namespace layout;

using UpdateRenderDataFn = bool (*)(void* mesh, const void* viewContext, unsigned frameId, int dbIdx);

UpdateRenderDataFn g_updateRenderData = nullptr;
std::atomic<bool> g_logged{false};

// Thread: render-phase enki fill job (RenderLoop::FillRenderInstancesJob, FUN_1408d3370); one job handles all views
// of an entity in sequence, so the two camera indices of one mesh never race.
bool HookedUpdateRenderData(void* mesh, const void* viewContext, unsigned frameId, int dbIdx)
{
    if (mesh && viewContext && Read<int>(viewContext, kViewCullingPass) == 0) {
        const unsigned index = Read<std::uint16_t>(viewContext, kViewCameraIndex);
        if (index < 2) {
            const unsigned other = 1 - index;
            if (Read<std::uint32_t>(mesh, kSkinMeshLastFrame + other * 4) == frameId &&
                Read<std::uint32_t>(mesh, kSkinMeshLastFrame + index * 4) != frameId) {
                Write<std::uint32_t>(mesh, kSkinMeshLastFrame + index * 4, frameId);
                Write(mesh, kSkinMeshLastLod + index * 2, Read<std::uint16_t>(mesh, kSkinMeshLastLod + other * 2));
                if (!g_logged.exchange(true, std::memory_order_relaxed)) {
                    TT2VR_LOG_INFO("skin motion: second view's palette latch skipped (first at frame id {}, camera "
                                   "index {})",
                        frameId, index);
                }
            }
        }
    }
    return g_updateRenderData(mesh, viewContext, frameId, dbIdx);
}

} // namespace

void InstallSkinMotion()
{
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;
    void* target = SignatureAddress(Sig::SkinMeshUpdateRenderData);
    if (!target || core::hooks::Create("SkinMesh::UpdateRenderData", target, &HookedUpdateRenderData,
                       &g_updateRenderData) != core::hooks::Result::Ok) {
        TT2VR_LOG_WARN("skin motion: hook not installed; skinned meshes lose object motion in two-view frames");
        return;
    }
    TT2VR_LOG_INFO("skin motion: hook installed");
}

} // namespace game
