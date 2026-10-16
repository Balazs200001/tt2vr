// One OpenXR color swapchain (an eye, or the menu screen), with a render target view per image.
//
// Format rule (CLAUDE.md sRGB gotcha): prefer an _SRGB format of the game back buffer's family, so the raw
// gamma-encoded bits can be copied unchanged. `decode` says whether a shader write must convert sRGB -> linear
// first: true when the image only has a typed _SRGB view (the hardware re-encodes) or when the swapchain format
// is linear (the runtime reads the values as linear).
#pragma once

#include "xr/openxr.hpp"

#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace xr {

// Picks the swapchain format from the runtime's list, in order: R8G8B8A8_UNORM_SRGB, B8G8R8A8_UNORM_SRGB,
// R8G8B8A8_UNORM, B8G8R8A8_UNORM; else the runtime's first. Returns DXGI_FORMAT_UNKNOWN for an empty list.
DXGI_FORMAT ChooseSwapchainFormat(const std::vector<std::int64_t>& offered);

// Thread: game main thread.
class EyeSwapchain {
public:
    bool Create(XrInstance instance, XrSession session, ID3D11Device* device, DXGI_FORMAT format, std::uint32_t width,
        std::uint32_t height, const char* name);
    void Destroy();
    bool Valid() const { return m_swapchain != XR_NULL_HANDLE; }

    // Acquires and waits for the next image. Every successful Acquire must be followed by Release.
    bool Acquire(XrInstance instance);
    void Release(XrInstance instance);
    ID3D11Texture2D* Texture() const { return m_textures[m_index]; }
    ID3D11RenderTargetView* Rtv() const { return m_rtvs[m_index].Get(); }

    XrSwapchain Handle() const { return m_swapchain; }
    std::uint32_t Width() const { return m_width; }
    std::uint32_t Height() const { return m_height; }
    DXGI_FORMAT Format() const { return m_format; } // what the swapchain was created with
    DXGI_FORMAT TextureFormat() const { return m_textureFormat; } // what the runtime's textures actually are
    bool Decode() const { return m_decode; }

private:
    XrSwapchain m_swapchain = XR_NULL_HANDLE;
    std::uint32_t m_width = 0, m_height = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT m_textureFormat = DXGI_FORMAT_UNKNOWN;
    bool m_decode = false;
    std::vector<ID3D11Texture2D*> m_textures; // owned by the runtime
    std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> m_rtvs;
    std::uint32_t m_index = 0;
};

} // namespace xr
