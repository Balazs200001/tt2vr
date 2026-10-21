#include "game/slot_mirror.hpp"

#include "core/config.hpp"
#include "core/hooks.hpp"
#include "core/log.hpp"
#include "core/test_mode.hpp"
#include "game/engine_layout.hpp"
#include "game/signatures.hpp"
#include "game/split_view.hpp"

#include <spdlog/fmt/fmt.h>

#include <chrono>
#include <cstdint>
#include <cstdio>

#include <windows.h>

namespace game {
namespace {

using namespace layout;

using WaterDropRenderFn = void (*)(void* wd, float dt, void* lastFrame, unsigned short slot);
using IsEnabledFn = bool (*)(const void* ppm, unsigned index, unsigned short slot);
using EnableFn = void (*)(void* ppm, unsigned index, short slot);
using GetFloatFn = bool (*)(const void* material, std::uint64_t id, float* out);
using GetVectorFn = bool (*)(const void* material, std::uint64_t id, float* out4);
using SetFloatFn = bool (*)(void* material, std::uint64_t id, float value);
using SetVectorFn = bool (*)(void* material, std::uint64_t id, const float* value4);
using StringIdFn = std::uint64_t* (*)(std::uint64_t* out, const char* text, int type);

WaterDropRenderFn g_waterDropRender = nullptr;
IsEnabledFn g_isEnabled = nullptr;
EnableFn g_enable = nullptr, g_disable = nullptr;
GetFloatFn g_getFloat = nullptr;
GetVectorFn g_getVector = nullptr;
SetFloatFn g_setFloat = nullptr;
SetVectorFn g_setVector = nullptr;
void* const* g_engine = nullptr; // &g_pGameEngine
bool g_greyReady = false;

bool g_twoViews = false; // this frame's render runs the two views (set in the render pre-hook, main thread)
bool g_loggedDrops = false;
// Crash grey: slot 1 enabled by the mod (to switch it off again when slot 0 goes off or two views stop).
bool g_greyMirrored = false;
std::uint64_t g_idBlend = 0, g_idGrey = 0, g_idLevels = 0, g_idColor = 0; // material parameter StringIds
std::chrono::steady_clock::time_point g_rainUntil{};
void* g_lastRenderLoop = nullptr; // the main render loop of the last frame (for the drops readout)

constexpr unsigned kNoPostProcess = 0xffffffffu;

char* Slot(void* wd, int slot)
{
    return static_cast<char*>(wd) + kWaterDropSlots + slot * kWaterDropSlotSize;
}

// Thread: game main thread (DeferredRenderLoop::Render, once per view while recording).
void HookedWaterDropRender(void* wd, float dt, void* lastFrame, unsigned short slot)
{
    if (slot != 1 || !g_twoViews || !wd || !core::GetConfig().xr.visor_drops_shared) {
        g_waterDropRender(wd, dt, lastFrame, slot);
        return;
    }
    // View 1 draws slot 0's drops over its own last-frame colour; dt 0 = no second simulation step.
    g_waterDropRender(wd, 0.0f, lastFrame, 0);
    __try {
        Write<std::uint8_t>(Slot(wd, 1), kWaterDropSlotAlive, Read<std::uint8_t>(Slot(wd, 0), kWaterDropSlotAlive));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (!g_loggedDrops) {
        g_loggedDrops = true;
        TT2VR_LOG_INFO("slot mirror: visor drops shared (the right eye shows the left eye's drops)");
    }
}

// The collision effect's FadeToColor state, or false. SEH: game objects, read only.
bool ReadGrey(void*& ppm, unsigned& index, void*& svm)
{
    __try {
        void* engine = g_engine ? *g_engine : nullptr;
        void* effect = engine ? Read<void*>(engine, kGameEngineCollisionEffect) : nullptr;
        if (!effect) {
            return false;
        }
        ppm = Read<void*>(effect, kCollisionEffectPostProcess);
        index = Read<unsigned>(effect, kCollisionEffectPpIndex);
        svm = Read<void*>(effect, kCollisionEffectMaterial);
        return ppm && svm && index != kNoPostProcess;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* SlotMaterial(void* svm, int slot)
{
    __try {
        char* holder = Read<char*>(Read<char*>(svm, slot * 8), kSplitViewSlotHolder);
        return holder ? holder + kHolderMaterialDefinition : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void MirrorGrey(bool twoViews)
{
    void* ppm = nullptr;
    void* svm = nullptr;
    unsigned index = 0;
    if (!g_greyReady || !ReadGrey(ppm, index, svm)) {
        return;
    }
    const bool on0 = g_isEnabled(ppm, index, 0);
    const bool on1 = g_isEnabled(ppm, index, 1);
    const bool want = twoViews && on0;
    if (want && !on1) {
        g_enable(ppm, index, 1);
        g_greyMirrored = true;
        TT2VR_LOG_INFO("slot mirror: crash grey (FadeToColor) on in the right eye too");
    }
    else if (!want && on1 && g_greyMirrored) {
        g_disable(ppm, index, 1);
        g_greyMirrored = false;
        TT2VR_LOG_INFO("slot mirror: crash grey off in the right eye");
    }
    if (!want) {
        return;
    }
    void* m0 = SlotMaterial(svm, 0);
    void* m1 = SlotMaterial(svm, 1);
    if (!m0 || !m1 || m0 == m1) {
        return;
    }
    float f = 0.0f;
    if (g_getFloat(m0, g_idBlend, &f)) {
        g_setFloat(m1, g_idBlend, f);
    }
    if (g_getFloat(m0, g_idGrey, &f)) {
        g_setFloat(m1, g_idGrey, f);
    }
    alignas(16) float v[4] = {};
    if (g_getVector(m0, g_idLevels, v)) {
        g_setVector(m1, g_idLevels, v);
    }
    if (g_getVector(m0, g_idColor, v)) {
        g_setVector(m1, g_idColor, v);
    }
}

void ForceRain(void* renderLoop)
{
    if (std::chrono::steady_clock::now() >= g_rainUntil) {
        return;
    }
    __try {
        if (void* wd = Read<void*>(renderLoop, kRenderLoopWaterDrop)) {
            Write<float>(Slot(wd, 0), kWaterDropSlotRain, 1.0f);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Both slots' rain, drop counts (rain / line / splash arrays), alive and enable bytes. SEH: engine object, read only.
void DropState(char* out, std::size_t size)
{
    __try {
        void* wd = g_lastRenderLoop ? Read<void*>(g_lastRenderLoop, kRenderLoopWaterDrop) : nullptr;
        if (!wd) {
            std::snprintf(out, size, "no WaterDrop yet");
            return;
        }
        const char* s0 = Slot(wd, 0);
        const char* s1 = Slot(wd, 1);
        std::snprintf(out, size,
            "slot 0: rain %.2f drops %d/%d/%d alive %u enable %u; slot 1: rain %.2f drops %d/%d/%d alive %u enable %u",
            Read<float>(s0, kWaterDropSlotRain), Read<int>(s0, 0x04), Read<int>(s0, 0x14), Read<int>(s0, 0x24),
            Read<std::uint8_t>(s0, kWaterDropSlotAlive), Read<std::uint8_t>(s0, kWaterDropSlotAlive + 1),
            Read<float>(s1, kWaterDropSlotRain), Read<int>(s1, 0x04), Read<int>(s1, 0x14), Read<int>(s1, 0x24),
            Read<std::uint8_t>(s1, kWaterDropSlotAlive), Read<std::uint8_t>(s1, kWaterDropSlotAlive + 1));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        std::snprintf(out, size, "WaterDrop unreadable");
    }
}

} // namespace

void InstallSlotMirror()
{
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;
    void* render = SignatureAddress(Sig::WaterDropRender);
    if (render && core::hooks::Create("WaterDrop::Render", render, &HookedWaterDropRender, &g_waterDropRender) ==
                      core::hooks::Result::Ok) {
        TT2VR_LOG_INFO("slot mirror: WaterDrop::Render hooked (shared visor drops: {})",
            core::GetConfig().xr.visor_drops_shared);
    }
    else {
        TT2VR_LOG_WARN("slot mirror: WaterDrop::Render not hooked; each eye keeps its own visor drops");
    }
    g_isEnabled = reinterpret_cast<IsEnabledFn>(SignatureAddress(Sig::PostProcessIsEnabled));
    g_enable = reinterpret_cast<EnableFn>(SignatureAddress(Sig::PostProcessEnable));
    g_disable = reinterpret_cast<EnableFn>(SignatureAddress(Sig::PostProcessDisable));
    g_getFloat = reinterpret_cast<GetFloatFn>(SignatureAddress(Sig::MaterialGetFloat));
    g_getVector = reinterpret_cast<GetVectorFn>(SignatureAddress(Sig::MaterialGetVector));
    g_setFloat = reinterpret_cast<SetFloatFn>(SignatureAddress(Sig::MaterialSetFloat));
    g_setVector = reinterpret_cast<SetVectorFn>(SignatureAddress(Sig::MaterialSetVector));
    const auto stringId = reinterpret_cast<StringIdFn>(SignatureAddress(Sig::StringIdFromChars));
    g_engine = static_cast<void* const*>(SignatureAddress(Sig::GameEngineInstance));
    if (g_isEnabled && g_enable && g_disable && g_getFloat && g_getVector && g_setFloat && g_setVector && stringId &&
        g_engine) {
        stringId(&g_idBlend, "fBlend", 1); // as TTCollisionEffect's Manage builds them
        stringId(&g_idGrey, "fGrey", 1);
        stringId(&g_idLevels, "vLevels", 1);
        stringId(&g_idColor, "vColor", 1);
        g_greyReady = true;
        TT2VR_LOG_INFO("slot mirror: crash grey mirror ready");
    }
    else {
        TT2VR_LOG_WARN("slot mirror: post-process signatures missing; the crash grey stays in the left eye only");
    }
}

void OnSlotMirrorLoopRender(void* renderLoop, bool twoViews)
{
    g_twoViews = twoViews;
    g_lastRenderLoop = renderLoop;
    if (renderLoop && core::TestMode()) {
        ForceRain(renderLoop);
    }
    MirrorGrey(twoViews);
}

std::string ForceVisorRain(float seconds)
{
    if (!core::TestMode()) {
        return "drops FAILED: test mode only (launch with -tt2vr-test)";
    }
    g_rainUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000.0f));
    char state[256];
    DropState(state, sizeof(state));
    return fmt::format("drops ok: rain on the visor for {:.0f} s; {}", seconds, state);
}

} // namespace game
