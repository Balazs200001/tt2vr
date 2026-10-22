#include "game/vr_effects.hpp"

#include "core/config.hpp"
#include "core/hooks.hpp"
#include "core/log.hpp"
#include "game/engine_layout.hpp"
#include "game/signatures.hpp"
#include "game/split_view.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cstdint>
#include <sstream>

#include <windows.h>

namespace game {
namespace {

using namespace layout;

using UpdateSlotFn = void (*)(void* cvar, unsigned short index, char notify);
using RenderFxaaFn = void (*)(void* renderLoop, void* renderContext, const void* option);
using GetIndexFn = int (*)(const void* ppm, const std::uint64_t* stringId);
using EnableFn = void (*)(void* ppm, unsigned index, short slot);
using IsEnabledFn = bool (*)(const void* ppm, unsigned index, unsigned short slot);
using StringIdFn = std::uint64_t* (*)(std::uint64_t* out, const char* text, int type);

using IsUsingTaaFn = bool (*)();

RenderFxaaFn g_renderFxaa = nullptr;
IsUsingTaaFn g_isUsingTaa = nullptr;
bool g_vrActive = false; // the headset session runs (DoUpdate pre-hook), read by the FXAA hook
bool g_fxaaLogged = false;
bool g_fxaaKeptLogged = false;

constexpr std::ptrdiff_t kRenderLoopPostProcess = 0x3d0; // PostProcessManager*
constexpr std::ptrdiff_t kRenderLoopFrameDt = 0x20; // float ms; 0 (pause, menus) = RenderTAA skips the resolve

// Thread: game main thread (DeferredRenderLoop::Render, per view).
void HookedRenderFxaa(void* renderLoop, void* renderContext, const void* option)
{
    if (g_vrActive && !core::GetConfig().xr.fxaa) {
        // RenderFXAA is one step of the post chain's countdown (PostProcessManager+0xd0, set per frame by
        // ResetFramePostProcessCount): the last step writes the final target (the back buffer). Skipped, the chain
        // ends one step early in a ping-pong buffer, so a single-view render (menus) never reaches the screen and the
        // menu draws over stale frames (2026-10-22 headset session). Two-view frames are safe: CopySplitRT copies
        // each view's current target into its half. And only when TAA runs after it (else no AA at all).
        if (SplitViewActive() && g_isUsingTaa && g_isUsingTaa() &&
            Read<float>(renderLoop, kRenderLoopFrameDt) != 0.0f) {
            if (!g_fxaaLogged) {
                g_fxaaLogged = true;
                TT2VR_LOG_INFO("vr effects: FXAA off in the two-view render (TAA follows it)");
            }
            return;
        }
        if (!g_fxaaKeptLogged) {
            g_fxaaKeptLogged = true;
            TT2VR_LOG_INFO("vr effects: FXAA kept for a single-view render or one without TAA (menus, pause)");
        }
    }
    g_renderFxaa(renderLoop, renderContext, option);
}

// [xr] occlusion = false: the culler's per-view occluder lists stay empty (ClearOccluder emptied them this frame), so
// nothing is occlusion-culled behind the terrain occlusion boxes; frustum culling is unchanged (renderer.md 10).
// Thread: game main thread (DeferredRenderLoop / ForwardRenderLoop::Render).
using PrepareOccluderFn = void (*)(void* culler, void* pool);
PrepareOccluderFn g_prepareOccluder = nullptr;
bool g_occlusionLoggedState = true;

void HookedPrepareOccluder(void* culler, void* pool)
{
    const bool skip = g_vrActive && !core::GetConfig().xr.occlusion;
    if (skip == g_occlusionLoggedState) {
        g_occlusionLoggedState = !skip;
        TT2VR_LOG_INFO("vr effects: occlusion culling {}", skip ? "off ([xr] occlusion = false)" : "back on");
    }
    if (skip) {
        return;
    }
    g_prepareOccluder(culler, pool);
}

struct Override {
    Sig sig;
    const char* name; // for the log
    bool (*keep)(); // config: true = leave the game's value while in VR
    void* cvar = nullptr;
    bool active = false;
};

Override g_overrides[] = {
    {Sig::SsaoEnable, "SSAO ([xr] ssao)", [] { return core::GetConfig().xr.ssao; }},
    {Sig::MotionBlurEnable, "motion blur ([xr] motion_blur)", [] { return core::GetConfig().xr.motion_blur; }},
};

void Apply(Override& o, bool vrActive)
{
    if (!o.cvar && !(o.cvar = SignatureAddress(o.sig))) {
        return;
    }
    if (vrActive && !o.keep()) {
        if (!o.active) {
            o.active = true;
            TT2VR_LOG_INFO("vr effects: {} off while the headset session runs (game value {})", o.name,
                Read<std::uint8_t>(o.cvar, kCVarBoolValue));
        }
        Write<std::uint8_t>(o.cvar, kCVarBoolValue, 0);
        Write<std::uint8_t>(o.cvar, kCVarBoolValue + 1, 0);
        return;
    }
    if (o.active) {
        o.active = false;
        const auto update = reinterpret_cast<UpdateSlotFn>((*static_cast<void* const* const*>(o.cvar))[kCVarUpdateSlot]);
        for (unsigned short i = 0; i < 2; ++i) {
            update(o.cvar, i, 0); // the slot follows the CVar's own value again
        }
        TT2VR_LOG_INFO("vr effects: {} back to the game setting ({})", o.name, Read<std::uint8_t>(o.cvar, kCVarBoolValue));
    }
}

} // namespace

void InstallVrEffects()
{
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;
    g_isUsingTaa = reinterpret_cast<IsUsingTaaFn>(SignatureAddress(Sig::RenderLoopIsUsingTAA));
    void* fxaa = g_isUsingTaa ? SignatureAddress(Sig::RenderLoopRenderFXAA) : nullptr; // no skip without the gate
    if (!fxaa || core::hooks::Create("RenderLoop::RenderFXAA", fxaa, &HookedRenderFxaa, &g_renderFxaa) !=
                     core::hooks::Result::Ok) {
        TT2VR_LOG_WARN("vr effects: RenderFXAA not hooked; [xr] fxaa has no effect");
    }
    void* occluders = SignatureAddress(Sig::VisibilityCullerPrepareOccluder);
    if (!occluders || core::hooks::Create("VisibilityCuller::PrepareOccluder", occluders, &HookedPrepareOccluder,
                          &g_prepareOccluder) != core::hooks::Result::Ok) {
        TT2VR_LOG_WARN("vr effects: PrepareOccluder not hooked; [xr] occlusion has no effect");
    }
}

void ApplyVrPostEffects(void* renderLoop, bool vrActive)
{
    static std::uint64_t s_vignette = 0;
    static bool s_logged = false;
    if (!vrActive || core::GetConfig().xr.vignette || !renderLoop) {
        return;
    }
    const auto getIndex = reinterpret_cast<GetIndexFn>(SignatureAddress(Sig::PostProcessGetIndex));
    const auto disable = reinterpret_cast<EnableFn>(SignatureAddress(Sig::PostProcessDisable));
    const auto isEnabled = reinterpret_cast<IsEnabledFn>(SignatureAddress(Sig::PostProcessIsEnabled));
    const auto stringId = reinterpret_cast<StringIdFn>(SignatureAddress(Sig::StringIdFromChars));
    if (!getIndex || !disable || !isEnabled || !stringId) {
        return;
    }
    if (!s_vignette) {
        stringId(&s_vignette, "Vignette", 1); // as Vignette::ApplyParams builds it
    }
    void* ppm = nullptr;
    __try {
        ppm = Read<void*>(renderLoop, kRenderLoopPostProcess);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!ppm) {
        return;
    }
    const int index = getIndex(ppm, &s_vignette);
    if (index < 0) {
        return;
    }
    if (isEnabled(ppm, static_cast<unsigned>(index), 0) || isEnabled(ppm, static_cast<unsigned>(index), 1)) {
        disable(ppm, static_cast<unsigned>(index), -1);
        if (!s_logged) {
            s_logged = true;
            TT2VR_LOG_INFO("vr effects: vignette off while the headset session runs");
        }
    }
}

std::string VrEffectCommand(const std::string& arg)
{
    const auto space = arg.find(' ');
    const std::string name = arg.substr(0, space);
    const std::string value = space == std::string::npos ? "" : arg.substr(space + 1);
    core::Config c = core::GetConfig();
    if (name == "render" || name == "sharpness") {
        // PLAN 9e A/B: [xr] render_scale (the next frame requests the new render size) and fsr_sharpness.
        float v = 0.0f;
        std::istringstream in(value);
        if (!(in >> v)) {
            return "vreffect FAILED: use vreffect render <0.5..1> or vreffect sharpness <0..2>";
        }
        float& field = name == "render" ? c.xr.render_scale : c.xr.fsr_sharpness;
        field = name == "render" ? std::clamp(v, 0.5f, 1.0f) : std::clamp(v, 0.0f, 2.0f);
        core::SetConfig(c);
        return fmt::format("vreffect render scale {:.2f} sharpness {:.2f} (saved to user.toml, as from the VR tab)",
            c.xr.render_scale, c.xr.fsr_sharpness);
    }
    if ((name != "fxaa" && name != "vignette" && name != "occlusion" && name != "mask" && name != "depth") ||
        (value != "on" && value != "off")) {
        return "vreffect FAILED: use vreffect <fxaa|vignette|occlusion|mask|depth> <on|off>, render <scale> or "
               "sharpness <stops>";
    }
    (name == "fxaa"       ? c.xr.fxaa
        : name == "vignette" ? c.xr.vignette
        : name == "mask"     ? c.xr.visibility_mask
        : name == "depth"    ? c.xr.depth_submit
                             : c.xr.occlusion) = value == "on";
    core::SetConfig(c);
    g_fxaaLogged = false;
    return "vreffect " + name + " " + value + " (this run only; [xr] " + name + " in tt2vr.toml keeps the default)";
}

void UpdateVrEffects(bool vrActive)
{
    g_vrActive = vrActive;
    for (Override& o : g_overrides) {
        Apply(o, vrActive);
    }
}

void ForceVrResolution(bool vrActive)
{
    static bool s_logged = false;
    static bool s_forcing = false;
    void* scale = SignatureAddress(Sig::ResolutionScaling);
    void* enabled = SignatureAddress(Sig::DynamicResolutionEnabled);
    if (!scale || !enabled) {
        return;
    }
    if (!vrActive) {
        if (s_forcing) {
            s_forcing = false;
            TT2VR_LOG_INFO("vr effects: dynamic resolution back to the game setting");
        }
        return;
    }
    const float sx = Read<float>(scale, 0), sy = Read<float>(scale, 4);
    const bool on = Read<std::uint8_t>(enabled, 0) != 0;
    if (!s_forcing) {
        s_forcing = true;
        TT2VR_LOG_INFO("vr effects: dynamic resolution held at 1.0 while the headset session runs");
    }
    if ((on || sx != 1.0f || sy != 1.0f) && !s_logged) {
        s_logged = true; // once per game run: the player's setting asks for dynamic resolution
        TT2VR_LOG_INFO("vr effects: the game's dynamic resolution was active (scale {:.3f} x {:.3f}, upscale {}); "
                       "forced to 1.0 and off in VR, your setting is unchanged",
            sx, sy, on ? "on" : "off");
    }
    Write(scale, 0, 1.0f);
    Write(scale, 4, 1.0f);
    Write<std::uint8_t>(enabled, 0, 0);
}

} // namespace game
