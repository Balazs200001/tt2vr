// game::RenderSize (PLAN Phase 3): make the game render at the headset's eye size, independent of its window.
//
// The engine sizes its swap chain and every window render target from one function that reads the window's client
// size, FUN_140959f80 (HWND, int* w, int* h) (docs/re/frame-anatomy.md section 8.5):
//   VideoDeviceDX11Windows::DeviceReset(WindowWin32*)  -> ResizeBuffers(w, h), Window+0x30/+0x34 (viewport),
//                                                         +0x418/+0x41c and +0x500/+0x504, then RTs rescaled from
//                                                         the back buffer (RenderTargetManager::ChangeRenderTargetsSize)
//   VideoDeviceDX11Windows::UpdateWindowResolution     -> WM_SIZE in windowed mode
// Hook (post, game main thread): for the game's main window only, report the forced size instead of the client rect.
// A size change asks for a device reset with the engine's own SetDeviceResetNeeded(nullptr); the next PostFlip
// runs DeviceReset. The window itself keeps its size; the desktop shows the back buffer stretched.
#pragma once

#include <windows.h>

#include <cstdint>

namespace game {

// Resolves the signatures and installs the hook (main thread, after ResolveSignatures). Fail safe: false = untouched.
bool InstallRenderSizeHook();
bool RenderSizeHookActive();

// Main thread. Forces the main window's render size to width x height (rounded down to even, as the engine does);
// 0 x 0 = report the real client size again. A change triggers one device reset.
void RequestRenderSize(HWND mainWindow, std::uint32_t width, std::uint32_t height);

} // namespace game
