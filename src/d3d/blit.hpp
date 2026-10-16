// d3d::Blit - copies a texture (the game's back buffer) into another of a different size or format family with a
// fullscreen triangle (PLAN Phase 3: back buffer -> OpenXR eye image). Our own HLSL, compiled at runtime.
//
// Color rule (CLAUDE.md, sRGB gotcha): the back buffer holds gamma-encoded values. The source is always sampled
// through a non-sRGB view, so the shader sees those raw values. Then:
//   decode = false: the raw values are written as-is (target view is non-sRGB and the consumer reads them as sRGB).
//   decode = true:  the shader converts sRGB -> linear first (target view is _SRGB and re-encodes, or the consumer
//                   reads the target as linear).
// The context state the blit touches is saved and restored (the engine may cache bindings across frames).
#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace d3d {

// Format helpers for the 8/10-bit color formats a back buffer or swapchain can have; other formats map to
// themselves.
DXGI_FORMAT TypelessOf(DXGI_FORMAT format); // R8G8B8A8_UNORM_SRGB -> R8G8B8A8_TYPELESS
DXGI_FORMAT LinearViewOf(DXGI_FORMAT format); // R8G8B8A8_UNORM_SRGB / _TYPELESS -> R8G8B8A8_UNORM
bool IsSrgb(DXGI_FORMAT format);
bool SameFamily(DXGI_FORMAT a, DXGI_FORMAT b); // CopyResource-compatible

// Thread: game main thread (Present hook). Not thread-safe.
class Blitter {
public:
    // Compiles the shaders and creates the fixed states. Returns false (and logs) if anything fails.
    bool Init(ID3D11Device* device);
    bool Ready() const { return m_ready; }
    // Stretches the source columns [u0, u0 + uw) (fractions of its width; default all of it) over the target view
    // (dstW x dstH, linear filtering). copySource = false reuses the source copy of the previous Blit (same source,
    // same frame).
    bool Blit(ID3D11DeviceContext* context, ID3D11Texture2D* source, ID3D11RenderTargetView* target, UINT dstW,
        UINT dstH, bool decode, float u0 = 0.0f, float uw = 1.0f, bool copySource = true);
    // Same into the rectangle `dst` of the target (in target pixels); the rest of the target is untouched. The
    // target may be the source itself (the source is copied first).
    bool BlitRect(ID3D11DeviceContext* context, ID3D11Texture2D* source, ID3D11RenderTargetView* target,
        const D3D11_VIEWPORT& dst, bool decode, float u0 = 0.0f, float uw = 1.0f, bool copySource = true);
    void Release();

private:
    bool PrepareSource(ID3D11Texture2D* source);

    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    bool m_ready = false;
    ID3D11Device* m_device = nullptr; // the game's; not owned
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_psRaw;
    ComPtr<ID3D11PixelShader> m_psDecode;
    ComPtr<ID3D11SamplerState> m_sampler;
    ComPtr<ID3D11RasterizerState> m_raster;
    ComPtr<ID3D11BlendState> m_blend;
    ComPtr<ID3D11DepthStencilState> m_depth;
    // Shader-readable copy of the source (a swap chain back buffer usually has no SRV bind flag).
    ComPtr<ID3D11Texture2D> m_copy;
    ComPtr<ID3D11ShaderResourceView> m_copySrv;
    D3D11_TEXTURE2D_DESC m_copyDesc{};
};

} // namespace d3d
