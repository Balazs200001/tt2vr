// game::EngineTextures: the D3D11 resources behind engine texture objects (read only, SEH-guarded).
//   RenderTarget* (TAA history pair, last-frame buffers, tonemap luminance, motion vectors): vtable +0x40 (index 0) ->
//     BaseTexture* = Texture2DDX11, whose ID3D11Texture2D* is at +0x68 (GetDeviceTexture).
//   Texture3D* (froxel volumes 'LightedFroxel'): concrete Texture3DDX11, ID3D11Texture3D* at +0x68 (symbols.md 6c).
#pragma once

#include <d3d11.h>

#include <cstddef>

namespace game {

// Thread: any (game objects are only read). nullptr if the object or its texture is missing.
ID3D11Texture2D* RenderTargetTexture(void* renderTarget);
// The RenderTarget* stored at base+offset (e.g. RenderLoop+0xce8+idx*0x10).
ID3D11Texture2D* RenderTargetTextureAt(void* base, std::ptrdiff_t offset);
ID3D11Resource* Texture3DResource(void* texture3d);

} // namespace game
