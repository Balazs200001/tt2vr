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

} // namespace game
