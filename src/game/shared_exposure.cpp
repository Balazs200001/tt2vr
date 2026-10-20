#include "game/shared_exposure.hpp"

#include "core/hooks.hpp"
#include "core/log.hpp"
#include "game/engine_layout.hpp"
#include "game/signatures.hpp"
#include "game/split_view.hpp"

#include <cstdint>

namespace game {
namespace {

using namespace layout;

// camIdx is a ushort passed in the fifth (stack) argument slot; forwarded as the full slot.
using TonemapFn = void (*)(void* tm, void* sceneHdr, void* dst, void* ctx, std::uintptr_t camIdx, void* vc);

TonemapFn g_tonemap = nullptr;
bool g_view0Done = false; // view 0's tonemap ran this frame (its result is in tm+0x138)
bool g_logged = false;

// Thread: game main thread (recording inside DeferredRenderLoop::Render).
void HookedTonemap(void* tm, void* sceneHdr, void* dst, void* ctx, std::uintptr_t camIdx, void* vc)
{
    const unsigned index = static_cast<unsigned>(camIdx & 0xffff);
    if (!tm || index == 0 || index > 1 || !SplitViewActive() || !g_view0Done) {
        g_tonemap(tm, sceneHdr, dst, ctx, camIdx, vc);
        if (index == 0) {
            g_view0Done = true;
        }
        return;
    }
    void* out = Read<void*>(tm, kTonemapOutput);
    void* prev1 = Read<void*>(tm, kTonemapPrevious + 8);
    const auto gate = Read<std::uint8_t>(tm, kTonemapLuminanceGate);
    Write(tm, kTonemapOutput, Read<void*>(tm, kTonemapPrevious)); // view 0's result this frame
    Write<std::uint8_t>(tm, kTonemapLuminanceGate, 0); // no adaptation step for slot 1
    g_tonemap(tm, sceneHdr, dst, ctx, camIdx, vc);
    Write(tm, kTonemapOutput, out); // undo the engine's slot-1 swap: slot 1's history stays as it was
    Write(tm, kTonemapPrevious + 8, prev1);
    Write<std::uint8_t>(tm, kTonemapLuminanceGate, gate);
    if (!g_logged) {
        g_logged = true;
        TT2VR_LOG_INFO("shared exposure: the right eye composes with the left eye's exposure");
    }
}

} // namespace

void InstallSharedExposure()
{
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;
    void* target = SignatureAddress(Sig::TonemapRender);
    if (!target || core::hooks::Create("Tonemap (FUN_140914680)", target, &HookedTonemap, &g_tonemap) !=
                       core::hooks::Result::Ok) {
        TT2VR_LOG_WARN("shared exposure: tonemap hook not installed; each eye adapts its own exposure");
        return;
    }
    TT2VR_LOG_INFO("shared exposure: hook installed");
}

void OnSharedExposureFrame()
{
    g_view0Done = false;
}

} // namespace game
