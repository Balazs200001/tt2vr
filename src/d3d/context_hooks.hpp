#pragma once

#include <d3d11.h>

namespace d3d {

// Hooks the ID3D11DeviceContext methods the frame trace and texture dump need, on the immediate
// context's vtable and on a deferred context's vtable (they can differ inside d3d11.dll).
// While neither a trace nor a dump is active every hook is a single atomic load and a forward.
void InstallContextHooks(ID3D11Device* device, ID3D11DeviceContext* immediate);

} // namespace d3d
