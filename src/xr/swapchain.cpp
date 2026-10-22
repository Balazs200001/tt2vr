#include "xr/swapchain.hpp"

#include "core/log.hpp"
#include "d3d/blit.hpp"

#include <algorithm>

namespace xr {

DXGI_FORMAT ChooseSwapchainFormat(const std::vector<std::int64_t>& offered)
{
    constexpr DXGI_FORMAT kPreferred[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
    for (const DXGI_FORMAT f : kPreferred) {
        if (std::find(offered.begin(), offered.end(), static_cast<std::int64_t>(f)) != offered.end()) {
            return f;
        }
    }
    return offered.empty() ? DXGI_FORMAT_UNKNOWN : static_cast<DXGI_FORMAT>(offered.front());
}

DXGI_FORMAT ChooseDepthFormat(const std::vector<std::int64_t>& offered)
{
    constexpr DXGI_FORMAT kPreferred[] = {DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT,
        DXGI_FORMAT_D32_FLOAT_S8X24_UINT};
    for (const DXGI_FORMAT f : kPreferred) {
        if (std::find(offered.begin(), offered.end(), static_cast<std::int64_t>(f)) != offered.end()) {
            return f;
        }
    }
    return DXGI_FORMAT_UNKNOWN;
}

bool EyeSwapchain::CreateDepth(XrInstance instance, XrSession session, ID3D11Device* device, DXGI_FORMAT format,
    std::uint32_t width, std::uint32_t height, const char* name)
{
    Destroy();
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    XrResult r = xrCreateSwapchain(session, &info, &m_swapchain);
    if (XR_FAILED(r)) {
        TT2VR_LOG_ERROR("xr: {} depth swapchain {}x{} format {} failed: {}", name, width, height,
            static_cast<int>(format), ResultString(instance, r));
        m_swapchain = XR_NULL_HANDLE;
        return false;
    }
    std::uint32_t count = 0;
    r = xrEnumerateSwapchainImages(m_swapchain, 0, &count, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (XR_SUCCEEDED(r) && count > 0) {
        r = xrEnumerateSwapchainImages(m_swapchain, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    }
    if (XR_FAILED(r) || count == 0 || !images[0].texture) {
        TT2VR_LOG_ERROR("xr: {} depth swapchain has no images: {}", name, ResultString(instance, r));
        Destroy();
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    images[0].texture->GetDesc(&desc);
    m_width = width;
    m_height = height;
    m_format = format;
    m_textureFormat = desc.Format;
    for (const auto& image : images) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
        dd.Format = format; // typeless images get the depth format the swapchain was created with
        dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
        if (FAILED(device->CreateDepthStencilView(image.texture, &dd, &dsv))) {
            TT2VR_LOG_ERROR("xr: {} depth swapchain: cannot create a depth-stencil view (format {})", name,
                static_cast<int>(format));
            Destroy();
            return false;
        }
        m_textures.push_back(image.texture);
        m_dsvs.push_back(dsv);
    }
    TT2VR_LOG_INFO("xr: {} depth swapchain {}x{} format {}, {} images, texture format {}", name, width, height,
        static_cast<int>(format), count, static_cast<int>(desc.Format));
    return true;
}

bool EyeSwapchain::Create(XrInstance instance, XrSession session, ID3D11Device* device, DXGI_FORMAT format,
    std::uint32_t width, std::uint32_t height, const char* name, bool srgbView)
{
    Destroy();
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    XrResult r = xrCreateSwapchain(session, &info, &m_swapchain);
    if (XR_FAILED(r)) {
        TT2VR_LOG_ERROR("xr: {} swapchain {}x{} format {} failed: {}", name, width, height, static_cast<int>(format),
            ResultString(instance, r));
        m_swapchain = XR_NULL_HANDLE;
        return false;
    }
    std::uint32_t count = 0;
    r = xrEnumerateSwapchainImages(m_swapchain, 0, &count, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (XR_SUCCEEDED(r) && count > 0) {
        r = xrEnumerateSwapchainImages(m_swapchain, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
    }
    if (XR_FAILED(r) || count == 0 || !images[0].texture) {
        TT2VR_LOG_ERROR("xr: {} swapchain has no images: {}", name, ResultString(instance, r));
        Destroy();
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    images[0].texture->GetDesc(&desc);
    m_width = width;
    m_height = height;
    m_format = format;
    m_textureFormat = desc.Format;
    const bool typeless = d3d::TypelessOf(desc.Format) == desc.Format;
    // Typeless image: write through the non-sRGB view, so the stored bits are exactly what the shader returns.
    // Typed image: the view must use its format; an _SRGB view re-encodes, so the shader must decode first.
    const bool encode = typeless && srgbView && d3d::IsSrgb(format);
    const DXGI_FORMAT rtvFormat = encode ? format : typeless ? d3d::LinearViewOf(format) : desc.Format;
    m_decode = typeless && !encode ? !d3d::IsSrgb(format) : true;
    for (const auto& image : images) {
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = rtvFormat;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(device->CreateRenderTargetView(image.texture, &rd, &rtv))) {
            TT2VR_LOG_ERROR("xr: {} swapchain: cannot create a render target view (format {})", name, static_cast<int>(rtvFormat));
            Destroy();
            return false;
        }
        m_textures.push_back(image.texture);
        m_rtvs.push_back(rtv);
    }
    TT2VR_LOG_INFO("xr: {} swapchain {}x{} format {}, {} images, texture format {}, view format {}, decode {}", name,
        width, height, static_cast<int>(format), count, static_cast<int>(desc.Format), static_cast<int>(rtvFormat),
        m_decode);
    return true;
}

void EyeSwapchain::Destroy()
{
    m_dsvs.clear();
    m_rtvs.clear();
    m_textures.clear();
    if (m_swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_swapchain);
        m_swapchain = XR_NULL_HANDLE;
    }
    m_width = m_height = 0;
    m_index = 0;
}

bool EyeSwapchain::Acquire(XrInstance instance)
{
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult r = xrAcquireSwapchainImage(m_swapchain, &acquire, &m_index);
    if (XR_FAILED(r) || m_index >= m_textures.size()) {
        TT2VR_LOG_ERROR("xr: xrAcquireSwapchainImage failed: {}", ResultString(instance, r));
        return false;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION; // the runtime hands images back within a display period
    r = xrWaitSwapchainImage(m_swapchain, &wait);
    if (XR_FAILED(r)) {
        TT2VR_LOG_ERROR("xr: xrWaitSwapchainImage failed: {}", ResultString(instance, r));
        return false;
    }
    return true;
}

void EyeSwapchain::Release(XrInstance instance)
{
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const XrResult r = xrReleaseSwapchainImage(m_swapchain, &release);
    if (XR_FAILED(r)) {
        TT2VR_LOG_ERROR("xr: xrReleaseSwapchainImage failed: {}", ResultString(instance, r));
    }
}

} // namespace xr
