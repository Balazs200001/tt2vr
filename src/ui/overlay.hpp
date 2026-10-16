#pragma once

#include <d3d11.h>

namespace ui {

// Desktop-window ImGui overlay: FPS, hook and version status, capture buttons, toggles.
// Thread: game main thread (Present hook and the game window's message loop).
void Render(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context);
void ReleaseTargets(); // before ResizeBuffers
void ToggleVisible();
bool Visible();
// Software mouse cursor drawn by ImGui on top of the frame. Purely visual: the game's own cursor state
// (ShowCursor / SetCursor / clipping) is never touched.
void ToggleCursor();

} // namespace ui
