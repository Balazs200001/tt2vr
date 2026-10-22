#include "d3d/flash_detector.hpp"

#include "core/config.hpp"
#include "core/glitch.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "d3d/blit.hpp"
#include "d3d/internal.hpp"
#include "d3d/texture_dump.hpp"

#include <spdlog/fmt/fmt.h>
#include <wrl/client.h>

#include <objbase.h>

#include <cstdint>
#include <cstring>
#include <system_error>
#include <vector>

namespace d3d::flash {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kW = 1024, kH = 512; // mip 0: both eyes side by side
constexpr UINT kMipW = kW / 2, kMipH = kH / 2; // mip 1, read back
constexpr int kRing = 4;
constexpr int kMinTiles = 3; // glitch tiles in one eye for a glitch frame
// Only the upper 5 / 8 of each eye: the scenery. Below is the bike, fixed to the camera, whose chrome tank flickers
// with sun glints from frame to frame (test mode 2026-10-22: most false hits).
constexpr int kSceneRows = 5 * core::kGlitchTile;
constexpr int kMaxSaved = 30;

struct Frame {
    std::uint64_t id = 0;
    std::uint64_t resets = 0; // TAA resets when it was captured
    std::vector<std::uint8_t> bgra; // kMipW x kMipH, tightly packed
};

Blitter g_blitter;
bool g_failed = false;
ComPtr<ID3D11Texture2D> g_tex;
ComPtr<ID3D11RenderTargetView> g_rtv;
ComPtr<ID3D11ShaderResourceView> g_srv;
ComPtr<ID3D11Texture2D> g_staging[kRing];
std::uint64_t g_stagingId[kRing] = {}; // 0 = free
std::uint64_t g_stagingResets[kRing] = {};
std::uint64_t g_next = 1; // id of the next captured frame
Frame g_hist[3]; // consecutive frames, oldest first
int g_histCount = 0;
std::uint64_t g_glitches = 0;
bool g_loggedOn = false;

bool Create(ID3D11Device* device)
{
    if (!g_blitter.Init(device)) {
        return false;
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = kW;
    d.Height = kH;
    d.MipLevels = 2;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = d.Format;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &g_tex)) ||
        FAILED(device->CreateRenderTargetView(g_tex.Get(), &rd, &g_rtv)) ||
        FAILED(device->CreateShaderResourceView(g_tex.Get(), nullptr, &g_srv))) {
        return false;
    }
    D3D11_TEXTURE2D_DESC s{};
    s.Width = kMipW;
    s.Height = kMipH;
    s.MipLevels = 1;
    s.ArraySize = 1;
    s.Format = d.Format;
    s.SampleDesc.Count = 1;
    s.Usage = D3D11_USAGE_STAGING;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& st : g_staging) {
        if (FAILED(device->CreateTexture2D(&s, nullptr, &st))) {
            return false;
        }
    }
    return true;
}

// N-1 | N | N+1 side by side, as captures\flashes\flash_<id>.png.
std::string Save(const Frame* f)
{
    constexpr UINT w = kMipW * 3;
    std::vector<std::uint8_t> bgr(static_cast<std::size_t>(w) * kMipH * 3);
    for (int k = 0; k < 3; ++k) {
        for (UINT y = 0; y < kMipH; ++y) {
            for (UINT x = 0; x < kMipW; ++x) {
                const std::uint8_t* p = &f[k].bgra[(static_cast<std::size_t>(y) * kMipW + x) * 4];
                std::uint8_t* o = &bgr[(static_cast<std::size_t>(y) * w + k * kMipW + x) * 3];
                o[0] = p[0];
                o[1] = p[1];
                o[2] = p[2];
            }
        }
    }
    const auto dir = core::CaptureDir() / "flashes";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / fmt::format("flash_{}.png", f[1].id);
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ok = dump::WritePngBgr(path, w, kMipH, bgr);
    if (SUCCEEDED(co)) {
        CoUninitialize();
    }
    return ok ? path.filename().string() : "not saved";
}

void Evaluate()
{
    const Frame* f = g_hist;
    if (f[0].resets != f[2].resets) {
        return; // a camera cut (crash camera, respawn) or another TAA reset in between
    }
    constexpr std::size_t stride = kMipW * 4;
    const auto left = core::ScoreGlitch(f[0].bgra.data(), f[1].bgra.data(), f[2].bgra.data(), stride, 0, kMipW / 2,
        kSceneRows);
    const auto right = core::ScoreGlitch(f[0].bgra.data(), f[1].bgra.data(), f[2].bgra.data(), stride, kMipW / 2,
        kMipW, kSceneRows);
    if (left.tiles < kMinTiles && right.tiles < kMinTiles) {
        return;
    }
    ++g_glitches;
    const std::string file = g_glitches <= kMaxSaved ? Save(f) : "not saved (limit)";
    TT2VR_LOG_INFO("flash detector: frame {} unlike both neighbours: left eye {} / {} tiles (score {:.0f} vs {:.0f}), "
                   "right eye {} / {} tiles (score {:.0f} vs {:.0f}); {}",
        f[1].id, left.tiles, left.total, left.maxScore, left.maxScoreD2, right.tiles, right.total, right.maxScore,
        right.maxScoreD2, file);
}

void Push(Frame&& frame)
{
    if (g_histCount > 0 && frame.id != g_hist[g_histCount - 1].id + 1) {
        g_histCount = 0; // a gap: start again
    }
    if (g_histCount == 3) {
        g_hist[0] = std::move(g_hist[1]);
        g_hist[1] = std::move(g_hist[2]);
        g_histCount = 2;
    }
    g_hist[g_histCount++] = std::move(frame);
    if (g_histCount == 3) {
        Evaluate();
    }
}

// Reads back every finished copy, oldest first.
void ReadBack(ID3D11DeviceContext* context)
{
    for (;;) {
        int oldest = -1;
        for (int i = 0; i < kRing; ++i) {
            if (g_stagingId[i] != 0 && (oldest < 0 || g_stagingId[i] < g_stagingId[oldest])) {
                oldest = i;
            }
        }
        if (oldest < 0) {
            return;
        }
        D3D11_MAPPED_SUBRESOURCE m{};
        if (context->Map(g_staging[oldest].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) {
            return; // not finished yet: try again next frame
        }
        Frame frame;
        frame.id = g_stagingId[oldest];
        frame.resets = g_stagingResets[oldest];
        frame.bgra.resize(static_cast<std::size_t>(kMipW) * kMipH * 4);
        for (UINT y = 0; y < kMipH; ++y) {
            const auto* src = static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch;
            std::uint8_t* dst = &frame.bgra[static_cast<std::size_t>(y) * kMipW * 4];
            for (UINT x = 0; x < kMipW; ++x) { // RGBA -> BGRA
                dst[x * 4 + 0] = src[x * 4 + 2];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 0];
                dst[x * 4 + 3] = 255;
            }
        }
        context->Unmap(g_staging[oldest].Get(), 0);
        g_stagingId[oldest] = 0;
        Push(std::move(frame));
    }
}

} // namespace

void OnPresent(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context, bool stereo,
    std::uint64_t resets)
{
    if (g_failed || !swapChain || !device || !context || !core::GetConfig().debug.flash_detect) {
        return;
    }
    InternalScope internal;
    if (!g_tex && !Create(device)) {
        g_failed = true;
        TT2VR_LOG_WARN("flash detector: cannot create its textures; off");
        return;
    }
    ReadBack(context);
    if (!stereo) {
        g_histCount = 0;
        return;
    }
    if (!g_loggedOn) {
        g_loggedOn = true;
        TT2VR_LOG_INFO("flash detector: watching the two-view frames ([debug] flash_detect)");
    }
    const int slot = static_cast<int>(g_next % kRing);
    if (g_stagingId[slot] != 0) {
        g_stagingId[slot] = 0; // still unread after four frames: dropped (a gap)
    }
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&back))) ||
        !g_blitter.Blit(context, back.Get(), g_rtv.Get(), kW, kH, false)) {
        ++g_next;
        return;
    }
    context->GenerateMips(g_srv.Get());
    context->CopySubresourceRegion(g_staging[slot].Get(), 0, 0, 0, 0, g_tex.Get(), 1, nullptr);
    g_stagingResets[slot] = resets;
    g_stagingId[slot] = g_next++;
}

} // namespace d3d::flash
