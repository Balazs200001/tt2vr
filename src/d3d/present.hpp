#pragma once

#include <d3d11.h>

#include <cstdint>
#include <string>
#include <vector>

namespace d3d {

// Called once from the device-creation hook (game main thread) with the game's device objects:
// installs the shader, context and swap-chain hooks.
void OnDeviceCreated(ID3D11Device* device, ID3D11DeviceContext* immediate, IDXGISwapChain* swapChain);

struct FrameStats {
    std::uint64_t frame = 0; // completed Present calls on the game's swap chain
    double fps = 0.0;
    double ms = 0.0;
    std::uint32_t width = 0; // back buffer
    std::uint32_t height = 0;
    bool presentHooked = false;
};
FrameStats Stats();

// Actions (present thread: hotkeys, overlay buttons, remote commands). Results go to the log, the
// overlay's result list and, for remote commands, the .done file.
void RequestFrameTrace(const std::string& origin);
void RequestTextureDump(const std::string& origin);
void RequestRenderDocCapture(const std::string& origin, const std::string& name);
void WriteShaderRegistry(const std::string& origin);
std::vector<std::string> RecentResults(); // newest last

} // namespace d3d
