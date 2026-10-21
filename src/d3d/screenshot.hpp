// d3d::Screenshot: remote "screenshot [name]". The engine's back buffer of the next frame (before the eye copy, the
// desktop mirror and the overlay), box-downscaled to at most 1920 px wide, saved as <capture dir>\screens\<name>.png.
// Lets Claude see the menus while driving the game in test mode (core::TestMode); works in every mode.
#pragma once

#include <d3d11.h>
#include <dxgi.h>

#include <functional>
#include <string>

namespace d3d::screenshot {

// Thread: present thread. `name` already sanitised ([A-Za-z0-9_-]); empty = a numbered name.
void Request(std::string name, std::function<void(const std::string&)> done);
// Present pre-hook, before anything draws over the engine's frame.
void OnPresent(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context);

} // namespace d3d::screenshot
