#include "game/vr_effects.hpp"

#include "core/config.hpp"
#include "core/hooks.hpp"
#include "core/log.hpp"
#include "game/engine_layout.hpp"
#include "game/signatures.hpp"

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

RenderFxaaFn g_renderFxaa = nullptr;
bool g_vrActive = false; // the headset session runs (DoUpdate pre-hook), read by the FXAA hook
bool g_fxaaLogged = false;

constexpr std::ptrdiff_t kRenderLoopPostProcess = 0x3d0; // PostProcessManager*

// Thread: game main thread (DeferredRenderLoop::Render, per view).
void HookedRenderFxaa(void* renderLoop, void* renderContext, const void* option)
{
    if (g_vrActive && !core::GetConfig().xr.fxaa) {
        if (!g_fxaaLogged) {
            g_fxaaLogged = true;
            TT2VR_LOG_INFO("vr effects: FXAA off while the headset session runs (TAA stays on)");
        }
        return;
    }
    g_renderFxaa(renderLoop, renderContext, option);
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
    void* fxaa = SignatureAddress(Sig::RenderLoopRenderFXAA);
    if (!fxaa || core::hooks::Create("RenderLoop::RenderFXAA", fxaa, &HookedRenderFxaa, &g_renderFxaa) !=
                     core::hooks::Result::Ok) {
        TT2VR_LOG_WARN("vr effects: RenderFXAA not hooked; [xr] fxaa has no effect");
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
        return fmt::format("vreffect render scale {:.2f} sharpness {:.2f} (this run only)", c.xr.render_scale,
            c.xr.fsr_sharpness);
    }
    if ((name != "fxaa" && name != "vignette") || (value != "on" && value != "off")) {
        return "vreffect FAILED: use vreffect <fxaa|vignette> <on|off>, render <scale> or sharpness <stops>";
    }
    (name == "fxaa" ? c.xr.fxaa : c.xr.vignette) = value == "on";
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
