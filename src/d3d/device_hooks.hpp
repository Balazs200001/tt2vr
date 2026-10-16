#pragma once

namespace d3d {

// Redirects TT2.exe's import of D3D11CreateDeviceAndSwapChain to our wrapper and keeps the
// redirect in place until the game has created its device. Returns false if the import was not found.
bool InstallDeviceHooks();

} // namespace d3d
