#include "game/shadow_settings.hpp"

#include "core/config.hpp"
#include "core/log.hpp"
#include "game/engine_layout.hpp"
#include "game/signatures.hpp"
#include "game/version.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>

namespace game {
namespace {

using namespace layout;

using UpdateSlotFn = void (*)(void* cvar, unsigned short index, char notify);

// Raises an int CVar's base value to `wanted` (clamped to its min/max) and recomputes both camera-index slots.
// Returns the slot-0 value before and after, or false on an access fault.
bool RaiseIntCVar(void* cvar, int wanted, int& before, int& after)
{
    __try {
        before = Read<int>(cvar, kCVarIntSlots);
        int value = std::max(wanted, Read<int>(cvar, kCVarIntBase));
        if (const int* lo = Read<const int*>(cvar, kCVarIntMin)) {
            value = std::max(value, *lo);
        }
        if (const int* hi = Read<const int*>(cvar, kCVarIntMax)) {
            value = std::min(value, *hi);
        }
        Write(cvar, kCVarIntBase, value);
        const auto update = reinterpret_cast<UpdateSlotFn>((*static_cast<void* const* const*>(cvar))[kCVarUpdateSlot]);
        for (unsigned short i = 0; i < 2; ++i) {
            update(cvar, i, 0);
        }
        after = Read<int>(cvar, kCVarIntSlots);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Apply(Sig sig, const char* name, int wanted)
{
    if (wanted <= 0) {
        return;
    }
    void* cvar = SignatureAddress(sig);
    int before = 0, after = 0;
    if (!cvar || !RaiseIntCVar(cvar, wanted, before, after)) {
        TT2VR_LOG_WARN("shadows: {} not applied (CVar missing or unreadable)", name);
        return;
    }
    TT2VR_LOG_INFO("shadows: {} {} -> {} (wanted {})", name, before, after, wanted);
}

} // namespace

void ApplyShadowSettings()
{
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;
    const auto& xc = core::GetConfig().xr;
    if (!xc.enabled || !EngineHooksAllowed()) {
        return;
    }
    ResolveSignatures();
    Apply(Sig::CascadeShadowMapSize, "Engine.Renderer.Shadow.CascadeShadowMapSize", xc.shadow_map_size);
    Apply(Sig::StaticShadowMapSize, "Engine.Renderer.StaticShadow.Size", xc.static_shadow_map_size);
}

} // namespace game
