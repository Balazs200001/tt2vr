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
#include <string>

namespace d3d::flash {

// The last few frames are also kept at full size on the GPU (colour and each eye's depth), so a glitch frame is
// saved as captures\screens\flash_<n>_full_{prev,glitch,next}.png and _depth_{l,r}[_prev].png (first few only).
//
// Thread: game main thread, Present pre-hook, before the eye copy. `stereo` = this frame has two views; `resets` =
// TAA history resets so far (game::TaaResetCount): frames around a reset (camera cut, crash, respawn) are skipped.
// `depth` = each eye's depth of this frame (R32_FLOAT, game::EyeDepthTexture), may be null. `note` (may be null) gives
// a line about this frame (game::CullProbeSummary), logged for N-1, N and N+1 with each glitch.
void OnPresent(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context, bool stereo,
    std::uint64_t resets, ID3D11Texture2D* depthLeft, ID3D11Texture2D* depthRight, std::string (*note)());

} // namespace d3d::flash
