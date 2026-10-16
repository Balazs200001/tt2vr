#include "d3d/describe.hpp"

#include <spdlog/fmt/fmt.h>
#include <wrl/client.h>

#include <array>

namespace d3d {
namespace {

using Microsoft::WRL::ComPtr;

// Indexed by DXGI_FORMAT value (0 .. DXGI_FORMAT_B4G4R4A4_UNORM).
constexpr std::array<const char*, 116> kFormatNames = {
    "UNKNOWN", "R32G32B32A32_TYPELESS", "R32G32B32A32_FLOAT", "R32G32B32A32_UINT", "R32G32B32A32_SINT",
    "R32G32B32_TYPELESS", "R32G32B32_FLOAT", "R32G32B32_UINT", "R32G32B32_SINT", "R16G16B16A16_TYPELESS",
    "R16G16B16A16_FLOAT", "R16G16B16A16_UNORM", "R16G16B16A16_UINT", "R16G16B16A16_SNORM", "R16G16B16A16_SINT",
    "R32G32_TYPELESS", "R32G32_FLOAT", "R32G32_UINT", "R32G32_SINT", "R32G8X24_TYPELESS", "D32_FLOAT_S8X24_UINT",
    "R32_FLOAT_X8X24_TYPELESS", "X32_TYPELESS_G8X24_UINT", "R10G10B10A2_TYPELESS", "R10G10B10A2_UNORM",
    "R10G10B10A2_UINT", "R11G11B10_FLOAT", "R8G8B8A8_TYPELESS", "R8G8B8A8_UNORM", "R8G8B8A8_UNORM_SRGB",
    "R8G8B8A8_UINT", "R8G8B8A8_SNORM", "R8G8B8A8_SINT", "R16G16_TYPELESS", "R16G16_FLOAT", "R16G16_UNORM",
    "R16G16_UINT", "R16G16_SNORM", "R16G16_SINT", "R32_TYPELESS", "D32_FLOAT", "R32_FLOAT", "R32_UINT",
    "R32_SINT", "R24G8_TYPELESS", "D24_UNORM_S8_UINT", "R24_UNORM_X8_TYPELESS", "X24_TYPELESS_G8_UINT",
    "R8G8_TYPELESS", "R8G8_UNORM", "R8G8_UINT", "R8G8_SNORM", "R8G8_SINT", "R16_TYPELESS", "R16_FLOAT",
    "D16_UNORM", "R16_UNORM", "R16_UINT", "R16_SNORM", "R16_SINT", "R8_TYPELESS", "R8_UNORM", "R8_UINT",
    "R8_SNORM", "R8_SINT", "A8_UNORM", "R1_UNORM", "R9G9B9E5_SHAREDEXP", "R8G8_B8G8_UNORM", "G8R8_G8B8_UNORM",
    "BC1_TYPELESS", "BC1_UNORM", "BC1_UNORM_SRGB", "BC2_TYPELESS", "BC2_UNORM", "BC2_UNORM_SRGB",
    "BC3_TYPELESS", "BC3_UNORM", "BC3_UNORM_SRGB", "BC4_TYPELESS", "BC4_UNORM", "BC4_SNORM", "BC5_TYPELESS",
    "BC5_UNORM", "BC5_SNORM", "B5G6R5_UNORM", "B5G5R5A1_UNORM", "B8G8R8A8_UNORM", "B8G8R8X8_UNORM",
    "R10G10B10_XR_BIAS_A2_UNORM", "B8G8R8A8_TYPELESS", "B8G8R8A8_UNORM_SRGB", "B8G8R8X8_TYPELESS",
    "B8G8R8X8_UNORM_SRGB", "BC6H_TYPELESS", "BC6H_UF16", "BC6H_SF16", "BC7_TYPELESS", "BC7_UNORM",
    "BC7_UNORM_SRGB", "AYUV", "Y410", "Y416", "NV12", "P010", "P016", "420_OPAQUE", "YUY2", "Y210", "Y216",
    "NV11", "AI44", "IA44", "P8", "A8P8", "B4G4R4A4_UNORM",
};
static_assert(kFormatNames.size() == DXGI_FORMAT_B4G4R4A4_UNORM + 1);

std::string BindFlags(UINT flags)
{
    std::string s;
    auto add = [&](UINT bit, const char* name) {
        if (flags & bit) {
            if (!s.empty()) {
                s += '|';
            }
            s += name;
        }
    };
    add(D3D11_BIND_VERTEX_BUFFER, "vb");
    add(D3D11_BIND_INDEX_BUFFER, "ib");
    add(D3D11_BIND_CONSTANT_BUFFER, "cb");
    add(D3D11_BIND_SHADER_RESOURCE, "srv");
    add(D3D11_BIND_STREAM_OUTPUT, "so");
    add(D3D11_BIND_RENDER_TARGET, "rtv");
    add(D3D11_BIND_DEPTH_STENCIL, "dsv");
    add(D3D11_BIND_UNORDERED_ACCESS, "uav");
    return s.empty() ? "none" : s;
}

std::string NameSuffix(ID3D11DeviceChild* object)
{
    const std::string name = DebugName(object);
    return name.empty() ? std::string() : fmt::format(" \"{}\"", name);
}

const char* ComparisonName(D3D11_COMPARISON_FUNC func)
{
    static constexpr const char* kNames[] = {"?", "never", "less", "equal", "less_equal", "greater",
        "not_equal", "greater_equal", "always"};
    return (func >= 1 && func <= 8) ? kNames[func] : kNames[0];
}

} // namespace

const char* FormatName(DXGI_FORMAT format)
{
    if (static_cast<size_t>(format) < kFormatNames.size()) {
        return kFormatNames[format];
    }
    thread_local char buf[16];
    *fmt::format_to_n(buf, sizeof(buf) - 1, "fmt{}", static_cast<int>(format)).out = '\0';
    return buf;
}

DXGI_FORMAT ReadableFormat(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return format;
    }
}

std::string DebugName(ID3D11DeviceChild* object)
{
    if (!object) {
        return {};
    }
    char buf[256] = {};
    UINT size = sizeof(buf) - 1;
    if (FAILED(object->GetPrivateData(WKPDID_D3DDebugObjectName, &size, buf))) {
        return {};
    }
    return std::string(buf, size < sizeof(buf) ? size : sizeof(buf) - 1);
}

std::string DescribeResource(ID3D11Resource* resource)
{
    if (!resource) {
        return "null";
    }
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    resource->GetType(&dim);
    std::string s;
    switch (dim) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
        D3D11_BUFFER_DESC d{};
        static_cast<ID3D11Buffer*>(resource)->GetDesc(&d);
        s = fmt::format("buf {} B bind={} usage={} stride={} misc=0x{:x}", d.ByteWidth, BindFlags(d.BindFlags),
            static_cast<int>(d.Usage), d.StructureByteStride, d.MiscFlags);
        break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
        D3D11_TEXTURE1D_DESC d{};
        static_cast<ID3D11Texture1D*>(resource)->GetDesc(&d);
        s = fmt::format("tex1d {} {} mips={} arr={} bind={}", d.Width, FormatName(d.Format), d.MipLevels,
            d.ArraySize, BindFlags(d.BindFlags));
        break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(resource)->GetDesc(&d);
        s = fmt::format("tex2d {}x{} {} mips={} arr={} ms={} bind={}{}", d.Width, d.Height, FormatName(d.Format),
            d.MipLevels, d.ArraySize, d.SampleDesc.Count, BindFlags(d.BindFlags),
            (d.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) ? " cube" : "");
        break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
        D3D11_TEXTURE3D_DESC d{};
        static_cast<ID3D11Texture3D*>(resource)->GetDesc(&d);
        s = fmt::format("tex3d {}x{}x{} {} mips={} bind={}", d.Width, d.Height, d.Depth, FormatName(d.Format),
            d.MipLevels, BindFlags(d.BindFlags));
        break;
    }
    default:
        s = "res?";
    }
    return s + NameSuffix(resource);
}

std::string DescribeView(ID3D11View* view)
{
    if (!view) {
        return "null";
    }
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    std::string s;
    if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&rtv)))) {
        D3D11_RENDER_TARGET_VIEW_DESC d{};
        rtv->GetDesc(&d);
        s = fmt::format("rtv {} dim={} mip={} slice={}", FormatName(d.Format), static_cast<int>(d.ViewDimension),
            d.Texture2DArray.MipSlice, d.Texture2DArray.FirstArraySlice);
    }
    else if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&dsv)))) {
        D3D11_DEPTH_STENCIL_VIEW_DESC d{};
        dsv->GetDesc(&d);
        s = fmt::format("dsv {} dim={} mip={} slice={} readonly=0x{:x}", FormatName(d.Format),
            static_cast<int>(d.ViewDimension), d.Texture2DArray.MipSlice, d.Texture2DArray.FirstArraySlice, d.Flags);
    }
    else if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&srv)))) {
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};
        srv->GetDesc(&d);
        s = fmt::format("srv {} dim={}", FormatName(d.Format), static_cast<int>(d.ViewDimension));
    }
    else if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&uav)))) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
        uav->GetDesc(&d);
        s = fmt::format("uav {} dim={} mip={}", FormatName(d.Format), static_cast<int>(d.ViewDimension),
            d.Texture2D.MipSlice);
    }
    else {
        s = "view?";
    }
    return s + NameSuffix(view);
}

std::string DescribeDepthStencilState(ID3D11DepthStencilState* state)
{
    if (!state) {
        return "null (default: depth less, write on)";
    }
    D3D11_DEPTH_STENCIL_DESC d{};
    state->GetDesc(&d);
    return fmt::format("depth={} write={} func={} stencil={}", d.DepthEnable ? "on" : "off",
        d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL ? "on" : "off", ComparisonName(d.DepthFunc),
        d.StencilEnable ? "on" : "off");
}

std::string DescribeRasterizerState(ID3D11RasterizerState* state)
{
    if (!state) {
        return "null (default: solid, cull back, cw front)";
    }
    D3D11_RASTERIZER_DESC d{};
    state->GetDesc(&d);
    static constexpr const char* kCull[] = {"?", "none", "front", "back"};
    return fmt::format("fill={} cull={} ccw_front={} depth_bias={} slope_bias={} depth_clip={} scissor={}",
        d.FillMode == D3D11_FILL_WIREFRAME ? "wire" : "solid", d.CullMode <= 3 ? kCull[d.CullMode] : "?",
        d.FrontCounterClockwise ? 1 : 0, d.DepthBias, d.SlopeScaledDepthBias, d.DepthClipEnable ? 1 : 0,
        d.ScissorEnable ? 1 : 0);
}

} // namespace d3d
