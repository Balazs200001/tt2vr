// d3d::flash ([debug] flash_detect, 2026-10-22): looks for single-frame glitches in the engine's two-view frame
// (Balazs: "flashes of distant landscape through nearby scenery", a frame or two, both eyes, anywhere). Each riding
// frame the back buffer (before the eye copy) is drawn into a 1024x512 texture, both eyes side by side; its mip 1
// (512x256, box-filtered by GenerateMips) is read back through a ring of staging copies, never waited for. Frame N is
// a glitch when, in some tiles of an eye, it is unlike both N-1 and N+1 while those agree (core::ScoreGlitch). Each
// one is logged and saved as captures\flashes\flash_<n>.png (N-1 | N | N+1). Glitches it never sees happen after the
// mod hands the frame over (runtime, compositor, reprojection).
#pragma once

#include <d3d11.h>
#include <dxgi.h>

#include <cstdint>

namespace d3d::flash {

// Thread: game main thread, Present pre-hook, before the eye copy. `stereo` = this frame has two views; `resets` =
// TAA history resets so far (game::TaaResetCount): frames around a reset (camera cut, crash, respawn) are skipped.
void OnPresent(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context, bool stereo,
    std::uint64_t resets);

} // namespace d3d::flash
