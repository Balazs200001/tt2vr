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
DXGI_FORMAT SrgbViewOf(DXGI_FORMAT format); // R8G8B8A8_UNORM / _TYPELESS -> R8G8B8A8_UNORM_SRGB; UNKNOWN if none
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
    // Draws `source` (premultiplied alpha, e.g. an overlay rendered on transparent black) over the rectangle `dst` of
    // the target, linear filtering: colour = src + dst * (1 - src.a); the target's alpha is untouched. No copy.
    bool Composite(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target,
        const D3D11_VIEWPORT& dst);
    // HUD matte (PLAN Phase 7): `black` / `white` hold the same image drawn over black and over white, read through
    // _SRGB views (linear values). Writes premultiplied colour = black and alpha = 1 - (white - black) over the whole
    // target (width x height), no blending.
    bool Matte(ID3D11DeviceContext* context, ID3D11ShaderResourceView* black, ID3D11ShaderResourceView* white,
        ID3D11RenderTargetView* target, UINT width, UINT height);
    // Sets alpha to 1 in the whole target, colour untouched (a raw copy carries the game's back-buffer alpha,
    // which a compositor may blend with).
    bool FillAlpha(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, UINT width, UINT height);
    // Debug views (PLAN 6f): draws `source` over the whole target (width x height), opaque. Raw: rgb as stored;
    // Motion: rg motion vectors (UV units) as 0.5 + 25 x mv in red / green; Depth: reversed-Z depth on a log scale
    // (1 = white, 1e-6 and the far plane 0 = black).
    enum class View { Raw, Motion, Depth };
    bool Visualize(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target,
        UINT width, UINT height, View view);
    // FSR1 (PLAN 9e, AMD FidelityFX FSR 1.0): upscales the source columns [srcX, srcX + srcW) x full height to the
    // whole target (dstW x dstH) with EASU, then sharpens with RCAS (sharpness in stops, 0 = strongest). Decode and
    // copySource as in Blit; output alpha 1. False when the FSR shaders are unavailable (the caller falls back to Blit).
    bool Fsr(ID3D11DeviceContext* context, ID3D11Texture2D* source, UINT srcX, UINT srcW,
        ID3D11RenderTargetView* target, UINT dstW, UINT dstH, bool decode, float sharpness, bool copySource = true);
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
    ComPtr<ID3D11PixelShader> m_psOpaque;
    ComPtr<ID3D11PixelShader> m_psOver;
    ComPtr<ID3D11PixelShader> m_psMotion;
    ComPtr<ID3D11PixelShader> m_psDepth;
    ComPtr<ID3D11PixelShader> m_psMatte;
    ComPtr<ID3D11PixelShader> m_psEasu;
    ComPtr<ID3D11PixelShader> m_psRcas;
    ComPtr<ID3D11Buffer> m_fsrCb; // four uint4 FSR constants, then uint4 {decode, 0, 0, 0}
    // EASU output = RCAS input (dstW x dstH, gamma-encoded values like the back buffer).
    ComPtr<ID3D11Texture2D> m_fsrMid;
    ComPtr<ID3D11ShaderResourceView> m_fsrMidSrv;
    ComPtr<ID3D11RenderTargetView> m_fsrMidRtv;
    UINT m_fsrMidW = 0, m_fsrMidH = 0;
    ComPtr<ID3D11BlendState> m_blendAlpha;
    ComPtr<ID3D11BlendState> m_blendOver;
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
