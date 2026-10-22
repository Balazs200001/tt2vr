#include "d3d/screenshot.hpp"

#include "core/log.hpp"
#include "core/paths.hpp"
#include "d3d/internal.hpp"
#include "d3d/texture_dump.hpp"

#include <spdlog/fmt/fmt.h>
#include <wrl/client.h>

#include <objbase.h>

#include <cstdint>
#include <system_error>
#include <vector>

namespace d3d::screenshot {
namespace {

using Microsoft::WRL::ComPtr;

struct Pending {
    std::string name;
    std::function<void(const std::string&)> done;
};
std::vector<Pending> g_pending;
unsigned g_counter = 0;

constexpr UINT kMaxWidth = 1920;

} // namespace

std::string SaveTexture(ID3D11Texture2D* texture, ID3D11Device* device, ID3D11DeviceContext* context,
    const std::string& name, UINT maxWidth)
{
    D3D11_TEXTURE2D_DESC d{};
    texture->GetDesc(&d);
    bool bgra = false;
    switch (d.Format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        bgra = true;
        break;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        break;
    default:
        return fmt::format("FAILED: format {} not supported", static_cast<int>(d.Format));
    }
    D3D11_TEXTURE2D_DESC s = d;
    s.Usage = D3D11_USAGE_STAGING;
    s.BindFlags = 0;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    s.MiscFlags = 0;
    s.MipLevels = 1;
    s.ArraySize = 1;
    s.SampleDesc = {1, 0};
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&s, nullptr, &staging))) {
        return "FAILED: staging texture";
    }
    InternalScope internal;
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        return "FAILED: map";
    }
    const UINT f = maxWidth == 0 ? 1 : (d.Width + maxWidth - 1) / maxWidth; // integer box filter
    const UINT w = d.Width / f, h = d.Height / f;
    std::vector<std::uint8_t> bgr(static_cast<std::size_t>(w) * h * 3);
    for (UINT y = 0; y < h; ++y) {
        for (UINT x = 0; x < w; ++x) {
            UINT sum[3] = {0, 0, 0};
            for (UINT dy = 0; dy < f; ++dy) {
                const auto* p = static_cast<const std::uint8_t*>(m.pData) +
                                static_cast<std::size_t>(y * f + dy) * m.RowPitch + static_cast<std::size_t>(x) * f * 4;
                for (UINT dx = 0; dx < f; ++dx, p += 4) {
                    sum[0] += bgra ? p[0] : p[2];
                    sum[1] += p[1];
                    sum[2] += bgra ? p[2] : p[0];
                }
            }
            std::uint8_t* o = &bgr[(static_cast<std::size_t>(y) * w + x) * 3];
            for (int c = 0; c < 3; ++c) {
                o[c] = static_cast<std::uint8_t>(sum[c] / (f * f));
            }
        }
    }
    context->Unmap(staging.Get(), 0);
    const auto dir = core::CaptureDir() / "screens";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / (name + ".png");
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ok = dump::WritePngBgr(path, w, h, bgr);
    if (SUCCEEDED(co)) {
        CoUninitialize();
    }
    if (!ok) {
        return fmt::format("FAILED: cannot write {}", path.string());
    }
    return fmt::format("ok {} ({}x{} from {}x{})", path.string(), w, h, d.Width, d.Height);
}

namespace {

std::string Save(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context,
    const std::string& name)
{
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        return "FAILED: no back buffer";
    }
    return SaveTexture(back.Get(), device, context, name, kMaxWidth);
}

} // namespace

void Request(std::string name, std::function<void(const std::string&)> done)
{
    if (name.empty()) {
        name = fmt::format("screen_{:03}", ++g_counter);
    }
    g_pending.push_back({std::move(name), std::move(done)});
}

void OnPresent(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (g_pending.empty() || !swapChain || !device || !context) {
        return;
    }
    auto pending = std::move(g_pending);
    g_pending.clear();
    for (auto& p : pending) {
        std::string result;
        try {
            result = "screenshot " + Save(swapChain, device, context, p.name);
        }
        catch (...) {
            result = "screenshot FAILED: exception";
        }
        TT2VR_LOG_INFO("{}", result);
        if (p.done) {
            p.done(result);
        }
    }
}

} // namespace d3d::screenshot
