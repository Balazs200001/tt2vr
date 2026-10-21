#include "game/vr_effects.hpp"

#include "core/config.hpp"
#include "core/log.hpp"
#include "game/engine_layout.hpp"
#include "game/signatures.hpp"

#include <cstdint>

namespace game {
namespace {

using namespace layout;

using UpdateSlotFn = void (*)(void* cvar, unsigned short index, char notify);

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

void UpdateVrEffects(bool vrActive)
{
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
