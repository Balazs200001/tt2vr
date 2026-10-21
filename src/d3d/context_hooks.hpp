#pragma once

#include <d3d11.h>

namespace d3d {

// Hooks the ID3D11DeviceContext methods the frame trace and texture dump need, on the immediate
// context's vtable and on a deferred context's vtable (they can differ inside d3d11.dll).
// While neither a trace nor a dump is active every hook is a single atomic load and a forward.
void InstallContextHooks(ID3D11Device* device, ID3D11DeviceContext* immediate);

// Called before every ID3D11DeviceContext::Draw on any context (immediate or deferred, any thread) while set; must
// be thread-safe and cheap. nullptr = none (one atomic load per slot and draw). Slot 0: the TAA leak test; slot 1:
// the temporal debug views (game::TemporalDebug).
using PreDrawCallback = void (*)(ID3D11DeviceContext* context);
inline constexpr int kPreDrawSlots = 2;
void SetPreDrawCallback(PreDrawCallback callback, int slot = 0);

} // namespace d3d
