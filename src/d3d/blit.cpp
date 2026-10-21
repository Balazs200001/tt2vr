#include "d3d/blit.hpp"

#include "core/log.hpp"
#include "d3d/internal.hpp"

#include <d3dcompiler.h>

#include <cstring>
#include <iterator>

namespace d3d {
namespace {

using Microsoft::WRL::ComPtr;

struct FormatFamily {
    DXGI_FORMAT typeless, linear, srgb; // srgb = DXGI_FORMAT_UNKNOWN when the family has none
};

constexpr FormatFamily kFamilies[] = {
    {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB},
    {DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB},
    {DXGI_FORMAT_B8G8R8X8_TYPELESS, DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_B8G8R8X8_UNORM_SRGB},
    {DXGI_FORMAT_R10G10B10A2_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_UNKNOWN},
    {DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_UNKNOWN},
};

const FormatFamily* FamilyOf(DXGI_FORMAT f)
{
    for (const auto& fam : kFamilies) {
        if (f == fam.typeless || f == fam.linear || (fam.srgb != DXGI_FORMAT_UNKNOWN && f == fam.srgb)) {
            return &fam;
        }
    }
    return nullptr;
}

// Fullscreen triangle; the source is read through a non-sRGB view (raw stored values).
constexpr char kShaderSource[] = R"(
Texture2D src : register(t0);
SamplerState smp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V VS(uint id : SV_VertexID)
{
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 PSRaw(V i) : SV_Target
{
    return float4(src.Sample(smp, i.uv).rgb, 1);
}
float3 ToLinear(float3 c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
float4 PSDecode(V i) : SV_Target
{
    return float4(ToLinear(saturate(src.Sample(smp, i.uv).rgb)), 1);
}
float4 PSOver(V i) : SV_Target
{
    return src.Sample(smp, i.uv); // premultiplied colour and alpha, for Composite
}
float4 PSOpaque(V i) : SV_Target
{
    return float4(0, 0, 0, 1); // written through an alpha-only write mask
}
)";

ComPtr<ID3DBlob> Compile(const char* entry, const char* target)
{
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "tt2vr_blit", nullptr, nullptr, entry,
        target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        TT2VR_LOG_ERROR("blit: compiling {} failed (hr=0x{:08x}): {}", entry, static_cast<unsigned>(hr),
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
        return nullptr;
    }
    return code;
}

// Every piece of context state Blit changes, saved on construction and restored on destruction.
class StateBackup {
public:
    explicit StateBackup(ID3D11DeviceContext* c) : m_c(c)
    {
        c->IAGetInputLayout(&m_layout);
        c->IAGetPrimitiveTopology(&m_topology);
        m_vsCount = m_psCount = m_gsCount = m_hsCount = m_dsCount = kMaxInstances;
        c->VSGetShader(&m_vs, m_vsInst, &m_vsCount);
        c->PSGetShader(&m_ps, m_psInst, &m_psCount);
        c->GSGetShader(&m_gs, m_gsInst, &m_gsCount);
        c->HSGetShader(&m_hs, m_hsInst, &m_hsCount);
        c->DSGetShader(&m_ds, m_dsInst, &m_dsCount);
        c->PSGetShaderResources(0, 1, &m_srv);
        c->PSGetSamplers(0, 1, &m_sampler);
        c->RSGetState(&m_raster);
        m_viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        c->RSGetViewports(&m_viewportCount, m_viewports);
        m_scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        c->RSGetScissorRects(&m_scissorCount, m_scissors);
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, m_rtvs, &m_dsv);
        c->OMGetBlendState(&m_blend, m_blendFactor, &m_sampleMask);
        c->OMGetDepthStencilState(&m_depth, &m_stencilRef);
    }

    ~StateBackup()
    {
        m_c->IASetInputLayout(m_layout.Get());
        m_c->IASetPrimitiveTopology(m_topology);
        m_c->VSSetShader(m_vs.Get(), m_vsInst, m_vsCount);
        m_c->PSSetShader(m_ps.Get(), m_psInst, m_psCount);
        m_c->GSSetShader(m_gs.Get(), m_gsInst, m_gsCount);
        m_c->HSSetShader(m_hs.Get(), m_hsInst, m_hsCount);
        m_c->DSSetShader(m_ds.Get(), m_dsInst, m_dsCount);
        ID3D11ShaderResourceView* srv = m_srv.Get();
        m_c->PSSetShaderResources(0, 1, &srv);
        ID3D11SamplerState* sampler = m_sampler.Get();
        m_c->PSSetSamplers(0, 1, &sampler);
        m_c->RSSetState(m_raster.Get());
        m_c->RSSetViewports(m_viewportCount, m_viewports);
        m_c->RSSetScissorRects(m_scissorCount, m_scissors);
        m_c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, m_rtvs, m_dsv.Get());
        m_c->OMSetBlendState(m_blend.Get(), m_blendFactor, m_sampleMask);
        m_c->OMSetDepthStencilState(m_depth.Get(), m_stencilRef);
        // The Get* calls above added references; drop them now that the state is back.
        for (auto* p : m_rtvs) {
            if (p) p->Release();
        }
        ReleaseInstances(m_vsInst, m_vsCount);
        ReleaseInstances(m_psInst, m_psCount);
        ReleaseInstances(m_gsInst, m_gsCount);
        ReleaseInstances(m_hsInst, m_hsCount);
        ReleaseInstances(m_dsInst, m_dsCount);
    }

    StateBackup(const StateBackup&) = delete;
    StateBackup& operator=(const StateBackup&) = delete;

private:
    static constexpr UINT kMaxInstances = 256;

    static void ReleaseInstances(ID3D11ClassInstance** inst, UINT count)
    {
        for (UINT i = 0; i < count; ++i) {
            if (inst[i]) inst[i]->Release();
        }
    }

    ID3D11DeviceContext* m_c;
    ComPtr<ID3D11InputLayout> m_layout;
    D3D11_PRIMITIVE_TOPOLOGY m_topology{};
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_ps;
    ComPtr<ID3D11GeometryShader> m_gs;
    ComPtr<ID3D11HullShader> m_hs;
    ComPtr<ID3D11DomainShader> m_ds;
    // Class-instance arrays are filled by the Get* calls as raw references (the API has no ComPtr form).
    ID3D11ClassInstance* m_vsInst[kMaxInstances] = {};
    ID3D11ClassInstance* m_psInst[kMaxInstances] = {};
    ID3D11ClassInstance* m_gsInst[kMaxInstances] = {};
    ID3D11ClassInstance* m_hsInst[kMaxInstances] = {};
    ID3D11ClassInstance* m_dsInst[kMaxInstances] = {};
    UINT m_vsCount = 0, m_psCount = 0, m_gsCount = 0, m_hsCount = 0, m_dsCount = 0;
    ComPtr<ID3D11ShaderResourceView> m_srv;
    ComPtr<ID3D11SamplerState> m_sampler;
    ComPtr<ID3D11RasterizerState> m_raster;
    UINT m_viewportCount = 0;
    D3D11_VIEWPORT m_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT m_scissorCount = 0;
    D3D11_RECT m_scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    ID3D11RenderTargetView* m_rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ComPtr<ID3D11DepthStencilView> m_dsv;
    ComPtr<ID3D11BlendState> m_blend;
    FLOAT m_blendFactor[4] = {};
    UINT m_sampleMask = 0;
    ComPtr<ID3D11DepthStencilState> m_depth;
    UINT m_stencilRef = 0;
};

} // namespace

DXGI_FORMAT TypelessOf(DXGI_FORMAT format)
{
    const FormatFamily* fam = FamilyOf(format);
    return fam ? fam->typeless : format;
}

DXGI_FORMAT LinearViewOf(DXGI_FORMAT format)
{
    const FormatFamily* fam = FamilyOf(format);
    return fam ? fam->linear : format;
}

bool IsSrgb(DXGI_FORMAT format)
{
    const FormatFamily* fam = FamilyOf(format);
    return fam && fam->srgb != DXGI_FORMAT_UNKNOWN && format == fam->srgb;
}

bool SameFamily(DXGI_FORMAT a, DXGI_FORMAT b)
{
    if (a == b) {
        return true;
    }
    const FormatFamily* fa = FamilyOf(a);
    return fa && fa == FamilyOf(b);
}

bool Blitter::Init(ID3D11Device* device)
{
    Release();
    m_device = device;
    InternalScope internal; // our shaders and states stay out of traces
    const auto vs = Compile("VS", "vs_5_0");
    const auto psRaw = Compile("PSRaw", "ps_5_0");
    const auto psDecode = Compile("PSDecode", "ps_5_0");
    const auto psOpaque = Compile("PSOpaque", "ps_5_0");
    const auto psOver = Compile("PSOver", "ps_5_0");
    if (!vs || !psRaw || !psDecode || !psOpaque || !psOver) {
        return false;
    }
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE; // the destination rectangle
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_BLEND_DESC ad{};
    ad.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
    D3D11_BLEND_DESC od{};
    od.RenderTarget[0].BlendEnable = TRUE;
    od.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    od.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    od.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    od.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    od.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    od.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    od.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vs)) ||
        FAILED(device->CreatePixelShader(psRaw->GetBufferPointer(), psRaw->GetBufferSize(), nullptr, &m_psRaw)) ||
        FAILED(device->CreatePixelShader(psDecode->GetBufferPointer(), psDecode->GetBufferSize(), nullptr, &m_psDecode)) ||
        FAILED(device->CreatePixelShader(psOpaque->GetBufferPointer(), psOpaque->GetBufferSize(), nullptr, &m_psOpaque)) ||
        FAILED(device->CreatePixelShader(psOver->GetBufferPointer(), psOver->GetBufferSize(), nullptr, &m_psOver)) ||
        FAILED(device->CreateBlendState(&ad, &m_blendAlpha)) || FAILED(device->CreateBlendState(&od, &m_blendOver)) ||
        FAILED(device->CreateSamplerState(&sd, &m_sampler)) || FAILED(device->CreateRasterizerState(&rd, &m_raster)) ||
        FAILED(device->CreateBlendState(&bd, &m_blend)) || FAILED(device->CreateDepthStencilState(&dd, &m_depth))) {
        TT2VR_LOG_ERROR("blit: creating shaders or states failed");
        Release();
        return false;
    }
    m_ready = true;
    return true;
}

bool Blitter::PrepareSource(ID3D11Texture2D* source)
{
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (desc.SampleDesc.Count != 1) {
        return false;
    }
    if (m_copy && desc.Width == m_copyDesc.Width && desc.Height == m_copyDesc.Height &&
        TypelessOf(desc.Format) == m_copyDesc.Format) {
        return true;
    }
    m_copySrv.Reset();
    m_copy.Reset();
    D3D11_TEXTURE2D_DESC cd{};
    cd.Width = desc.Width;
    cd.Height = desc.Height;
    cd.MipLevels = 1;
    cd.ArraySize = 1;
    cd.Format = TypelessOf(desc.Format);
    cd.SampleDesc.Count = 1;
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = LinearViewOf(desc.Format);
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (FAILED(m_device->CreateTexture2D(&cd, nullptr, &m_copy)) ||
        FAILED(m_device->CreateShaderResourceView(m_copy.Get(), &sv, &m_copySrv))) {
        TT2VR_LOG_ERROR("blit: cannot create a {}x{} copy of format {}", desc.Width, desc.Height,
            static_cast<int>(desc.Format));
        m_copySrv.Reset();
        m_copy.Reset();
        return false;
    }
    m_copyDesc = cd;
    TT2VR_LOG_INFO("blit: source copy {}x{} format {} (view {})", cd.Width, cd.Height, static_cast<int>(cd.Format),
        static_cast<int>(sv.Format));
    return true;
}

bool Blitter::Blit(ID3D11DeviceContext* context, ID3D11Texture2D* source, ID3D11RenderTargetView* target, UINT dstW,
    UINT dstH, bool decode, float u0, float uw, bool copySource)
{
    const D3D11_VIEWPORT dst{0.0f, 0.0f, static_cast<float>(dstW), static_cast<float>(dstH), 0.0f, 1.0f};
    return BlitRect(context, source, target, dst, decode, u0, uw, copySource);
}

bool Blitter::BlitRect(ID3D11DeviceContext* context, ID3D11Texture2D* source, ID3D11RenderTargetView* target,
    const D3D11_VIEWPORT& dst, bool decode, float u0, float uw, bool copySource)
{
    if (!m_ready || !context || !source || !target || uw <= 0.0f || dst.Width <= 0.0f || dst.Height <= 0.0f) {
        return false;
    }
    InternalScope internal;
    if (!PrepareSource(source)) {
        return false;
    }
    if (copySource) {
        context->CopyResource(m_copy.Get(), source);
    }
    StateBackup backup(context);
    // A sub-range of the source = the whole source drawn into a wider viewport shifted left; the scissor keeps the
    // draw inside the destination rectangle (viewport origins may be negative).
    const float width = dst.Width / uw;
    const D3D11_VIEWPORT vp{dst.TopLeftX - u0 * width, dst.TopLeftY, width, dst.Height, 0.0f, 1.0f};
    const D3D11_RECT scissor{static_cast<LONG>(dst.TopLeftX), static_cast<LONG>(dst.TopLeftY),
        static_cast<LONG>(dst.TopLeftX + dst.Width), static_cast<LONG>(dst.TopLeftY + dst.Height)};
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vs.Get(), nullptr, 0);
    context->PSSetShader(decode ? m_psDecode.Get() : m_psRaw.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->RSSetState(m_raster.Get());
    context->RSSetViewports(1, &vp);
    context->RSSetScissorRects(1, &scissor);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(m_blend.Get(), nullptr, 0xffffffff);
    context->OMSetDepthStencilState(m_depth.Get(), 0);
    context->PSSetShaderResources(0, 1, m_copySrv.GetAddressOf());
    context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    context->PSSetShaderResources(0, 1, &none); // before the restore re-binds the game's slot 0
    return true;
}

bool Blitter::Composite(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* target,
    const D3D11_VIEWPORT& dst)
{
    if (!m_ready || !context || !source || !target || dst.Width <= 0.0f || dst.Height <= 0.0f) {
        return false;
    }
    InternalScope internal;
    StateBackup backup(context);
    const D3D11_VIEWPORT vp{dst.TopLeftX, dst.TopLeftY, dst.Width, dst.Height, 0.0f, 1.0f};
    const D3D11_RECT scissor{static_cast<LONG>(dst.TopLeftX), static_cast<LONG>(dst.TopLeftY),
        static_cast<LONG>(dst.TopLeftX + dst.Width), static_cast<LONG>(dst.TopLeftY + dst.Height)};
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vs.Get(), nullptr, 0);
    context->PSSetShader(m_psOver.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->RSSetState(m_raster.Get());
    context->RSSetViewports(1, &vp);
    context->RSSetScissorRects(1, &scissor);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(m_blendOver.Get(), nullptr, 0xffffffff);
    context->OMSetDepthStencilState(m_depth.Get(), 0);
    context->PSSetShaderResources(0, 1, &source);
    context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
    return true;
}

bool Blitter::FillAlpha(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, UINT width, UINT height)
{
    if (!m_ready || !context || !target) {
        return false;
    }
    InternalScope internal;
    StateBackup backup(context);
    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    const D3D11_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vs.Get(), nullptr, 0);
    context->PSSetShader(m_psOpaque.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->RSSetState(m_raster.Get());
    context->RSSetViewports(1, &vp);
    context->RSSetScissorRects(1, &scissor);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(m_blendAlpha.Get(), nullptr, 0xffffffff);
    context->OMSetDepthStencilState(m_depth.Get(), 0);
    context->Draw(3, 0);
    return true;
}

void Blitter::Release()
{
    m_ready = false;
    m_blendOver.Reset();
    m_psOver.Reset();
    m_blendAlpha.Reset();
    m_psOpaque.Reset();
    m_copySrv.Reset();
    m_copy.Reset();
    m_depth.Reset();
    m_blend.Reset();
    m_raster.Reset();
    m_sampler.Reset();
    m_psDecode.Reset();
    m_psRaw.Reset();
    m_vs.Reset();
    m_copyDesc = {};
}

} // namespace d3d
