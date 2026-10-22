#include "game/temporal_debug.hpp"

#include "core/log.hpp"
#include "core/paths.hpp"
#include "d3d/blit.hpp"
#include "d3d/context_hooks.hpp"
#include "d3d/internal.hpp"
#include "d3d/texture_dump.hpp"
#include "game/engine_layout.hpp"
#include "game/engine_textures.hpp"
#include "game/split_view.hpp"

#include <DirectXPackedVector.h>
#include <objbase.h>
#include <spdlog/fmt/fmt.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

namespace game {
namespace {

using namespace layout;
using Microsoft::WRL::ComPtr;

constexpr unsigned kThumbWidth = 240;
constexpr int kViews = 3; // TemporalView

struct Thumb {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
};

// Main thread.
bool g_shown = false;
bool g_freeze = false;
bool g_armed = false;
void* g_renderLoop = nullptr; // the main RenderLoop while two views run, else null
d3d::Blitter g_blitter;
bool g_blitterTried = false;
ComPtr<ID3D11Texture2D> g_motion[2]; // each eye's motion vectors, copied from the shared MV RT at its TAA draw
ComPtr<ID3D11Texture2D> g_frozen; // eye R's frozen TAA history
Thumb g_thumbs[2][kViews];
unsigned g_thumbW = 0, g_thumbH = 0;
std::vector<std::pair<ID3D11Texture2D*, ComPtr<ID3D11ShaderResourceView>>> g_srvs; // views of engine textures
TemporalVram g_vram;
int g_vramCountdown = 0;
std::function<void(const std::string&)> g_motionSaveDone;

// Read by the pre-draw callback on the recording threads. The main thread changes them only in the Render pre-hook
// (before this frame's recording starts) and releases textures only there or at Present, after the recording.
std::atomic<ID3D11Texture2D*> g_history[2]{};
std::atomic<ID3D11Texture2D*> g_motionCopy[2]{};
std::atomic<ID3D11Texture2D*> g_frozenCopy{nullptr};
std::atomic<bool> g_frozenValid{false};

// Thread: any context's Draw while armed. The 'TAA resolve' draw of view k is the one whose t2 (LastFrameBuffer) is
// view k's history; its t3 is the motion-vector RT, still holding view k's vectors.
void DebugPreDraw(ID3D11DeviceContext* c)
{
    ID3D11Texture2D* const h0 = g_history[0].load(std::memory_order_acquire);
    ID3D11Texture2D* const h1 = g_history[1].load(std::memory_order_acquire);
    if (!h0 && !h1) {
        return;
    }
    ID3D11ShaderResourceView* srv[2] = {}; // t2 LastFrameBuffer, t3 MotionVectors
    c->PSGetShaderResources(2, 2, srv);
    ComPtr<ID3D11ShaderResourceView> history(srv[0]), motion(srv[1]);
    if (srv[0]) srv[0]->Release();
    if (srv[1]) srv[1]->Release();
    if (!history) {
        return;
    }
    ComPtr<ID3D11Resource> historyRes;
    history->GetResource(&historyRes);
    const int eye = historyRes.Get() == h0 ? 0 : historyRes.Get() == h1 ? 1 : -1;
    if (eye < 0) {
        return;
    }
    d3d::InternalScope internal; // our copies on the hooked context are not traced
    if (ID3D11Texture2D* copy = g_motionCopy[eye].load(std::memory_order_acquire); copy && motion) {
        ComPtr<ID3D11Resource> motionRes;
        motion->GetResource(&motionRes);
        c->CopyResource(copy, motionRes.Get());
    }
    if (ID3D11Texture2D* frozen = g_frozenCopy.load(std::memory_order_acquire); eye == 1 && frozen) {
        if (!g_frozenValid.exchange(true)) {
            c->CopyResource(frozen, historyRes.Get()); // the first frozen frame: keep this history
        }
        else {
            c->CopyResource(historyRes.Get(), frozen); // eye R's resolve reads the frozen history
        }
    }
}

bool SameDesc(ID3D11Texture2D* a, ID3D11Texture2D* b)
{
    D3D11_TEXTURE2D_DESC da{}, db{};
    a->GetDesc(&da);
    b->GetDesc(&db);
    return da.Width == db.Width && da.Height == db.Height && da.Format == db.Format && da.MipLevels == db.MipLevels &&
           da.ArraySize == db.ArraySize;
}

// A texture CopyResource can fill from `like` (same size, format, mips), shader-readable.
bool EnsureCopyOf(ComPtr<ID3D11Texture2D>& copy, ID3D11Texture2D* like)
{
    if (!like) {
        copy.Reset();
        return false;
    }
    if (copy && SameDesc(copy.Get(), like)) {
        return true;
    }
    D3D11_TEXTURE2D_DESC d{};
    like->GetDesc(&d);
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    ComPtr<ID3D11Device> device;
    like->GetDevice(&device);
    copy.Reset();
    return SUCCEEDED(device->CreateTexture2D(&d, nullptr, &copy));
}

void Disarm()
{
    if (!g_armed) {
        return;
    }
    g_armed = false;
    d3d::SetPreDrawCallback(nullptr, 1);
    for (int k = 0; k < 2; ++k) {
        g_history[k] = nullptr;
        g_motionCopy[k] = nullptr;
    }
    g_frozenCopy = nullptr;
}

UINT BitsPerPixel(DXGI_FORMAT f)
{
    const auto v = static_cast<int>(f);
    if (v >= DXGI_FORMAT_R32G32B32A32_TYPELESS && v <= DXGI_FORMAT_R32G32B32A32_SINT) return 128;
    if (v >= DXGI_FORMAT_R32G32B32_TYPELESS && v <= DXGI_FORMAT_R32G32B32_SINT) return 96;
    if (v >= DXGI_FORMAT_R16G16B16A16_TYPELESS && v <= DXGI_FORMAT_X32_TYPELESS_G8X24_UINT) return 64;
    if (v >= DXGI_FORMAT_R8G8_TYPELESS && v <= DXGI_FORMAT_R16_SINT) return 16;
    if (v >= DXGI_FORMAT_R8_TYPELESS && v <= DXGI_FORMAT_A8_UNORM) return 8;
    return 32; // R10G10B10A2 .. X24G8, B8G8R8A8 and the rest used here
}

double Megabytes(ID3D11Resource* resource)
{
    if (!resource) {
        return 0.0;
    }
    double bytes = 0.0;
    ComPtr<ID3D11Texture2D> t2;
    ComPtr<ID3D11Texture3D> t3;
    if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&t2)))) {
        D3D11_TEXTURE2D_DESC d{};
        t2->GetDesc(&d);
        for (UINT m = 0; m < d.MipLevels; ++m) {
            bytes += static_cast<double>(std::max(1u, d.Width >> m)) * std::max(1u, d.Height >> m) * d.ArraySize *
                     BitsPerPixel(d.Format) / 8.0;
        }
    }
    else if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&t3)))) {
        D3D11_TEXTURE3D_DESC d{};
        t3->GetDesc(&d);
        for (UINT m = 0; m < d.MipLevels; ++m) {
            bytes += static_cast<double>(std::max(1u, d.Width >> m)) * std::max(1u, d.Height >> m) *
                     std::max(1u, d.Depth >> m) * BitsPerPixel(d.Format) / 8.0;
        }
    }
    return bytes / (1024.0 * 1024.0);
}

void UpdateVram(void* renderLoop)
{
    TemporalVram v;
    void* lighting = Read<void*>(renderLoop, kRenderLoopDeferredLighting);
    void* tonemap = Read<void*>(renderLoop, kRenderLoopTonemap);
    for (int k = 0; k < 2; ++k) {
        v.taaMB[k] = Megabytes(RenderTargetTextureAt(renderLoop, kRenderLoopTaaHistory + k * 0x10)) +
                     Megabytes(RenderTargetTextureAt(renderLoop, kRenderLoopTaaHistory + k * 0x10 + 8));
        v.lastFrameMB[k] = Megabytes(RenderTargetTextureAt(renderLoop, kRenderLoopLastFrameDepth + k * 8)) +
                           Megabytes(RenderTargetTextureAt(renderLoop, kRenderLoopLastFrameBuffer + k * 8));
        if (lighting) {
            for (int pp = 0; pp < 2; ++pp) {
                v.froxelMB[k] += Megabytes(
                    Texture3DResource(Read<void*>(lighting, kLightingFroxelVolumes + (pp * 2 + k) * 8)));
            }
        }
        v.luminanceMB[k] = tonemap ? Megabytes(RenderTargetTextureAt(tonemap, kTonemapPrevious + k * 8)) : 0.0;
        v.totalMB[k] = v.taaMB[k] + v.lastFrameMB[k] + v.froxelMB[k] + v.luminanceMB[k];
        v.debugMB += Megabytes(g_motion[k].Get());
        for (const Thumb& t : g_thumbs[k]) {
            v.debugMB += Megabytes(t.tex.Get());
        }
    }
    v.debugMB += Megabytes(g_frozen.Get());
    v.valid = true;
    if (!g_vram.valid || static_cast<int>(v.totalMB[1]) != static_cast<int>(g_vram.totalMB[1])) {
        TT2VR_LOG_INFO("temporal debug: per-eye history VRAM {:.1f} / {:.1f} MB (TAA {:.1f}, last frame {:.1f}, froxel "
                       "{:.1f}, luminance {:.3f} per eye); debug copies {:.1f} MB",
            v.totalMB[0], v.totalMB[1], v.taaMB[1], v.lastFrameMB[1], v.froxelMB[1], v.luminanceMB[1], v.debugMB);
    }
    g_vram = v;
}

// A shader view of an engine texture (typeless families get their plain typed format), cached per texture: the view
// keeps its texture alive, so a cached pointer is never reused for another texture.
ID3D11ShaderResourceView* ViewOf(ID3D11Texture2D* texture)
{
    if (!texture) {
        return nullptr;
    }
    for (const auto& [tex, srv] : g_srvs) {
        if (tex == texture) {
            return srv.Get();
        }
    }
    D3D11_TEXTURE2D_DESC d{};
    texture->GetDesc(&d);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = d.Format == DXGI_FORMAT_R32_TYPELESS             ? DXGI_FORMAT_R32_FLOAT
                : d.Format == DXGI_FORMAT_R16G16_TYPELESS       ? DXGI_FORMAT_R16G16_FLOAT
                : d.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                                 : d3d::LinearViewOf(d.Format);
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    ComPtr<ID3D11Device> device;
    texture->GetDevice(&device);
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateShaderResourceView(texture, &sd, &srv))) {
        return nullptr;
    }
    if (g_srvs.size() >= 16) {
        g_srvs.clear(); // the TAA pair and last-frame buffers are recreated on resets and resizes
    }
    g_srvs.emplace_back(texture, srv);
    return srv.Get();
}

bool EnsureThumbs(ID3D11Device* device, ID3D11Texture2D* like)
{
    D3D11_TEXTURE2D_DESC eye{};
    like->GetDesc(&eye);
    const unsigned w = kThumbWidth;
    const unsigned h = std::max(1u, kThumbWidth * eye.Height / std::max(1u, eye.Width));
    if (g_thumbs[0][0].tex && g_thumbW == w && g_thumbH == h) {
        return true;
    }
    g_thumbW = w;
    g_thumbH = h;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc = {1, 0};
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    for (auto& eyeThumbs : g_thumbs) {
        for (Thumb& t : eyeThumbs) {
            t = {};
            if (FAILED(device->CreateTexture2D(&d, nullptr, &t.tex)) ||
                FAILED(device->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv)) ||
                FAILED(device->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv))) {
                t = {};
                return false;
            }
        }
    }
    return true;
}

// Both eyes' motion-vector copies read back (R16G16_FLOAT, UV units per frame), statistics in pixels, one PNG.
std::string SaveMotion(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (!g_motion[0] || !g_motion[1]) {
        return "FAILED: no motion-vector copies (views off or not two views)";
    }
    D3D11_TEXTURE2D_DESC d{};
    g_motion[0]->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_R16G16_FLOAT) {
        return fmt::format("FAILED: motion vectors are format {}, expected R16G16_FLOAT", static_cast<int>(d.Format));
    }
    D3D11_TEXTURE2D_DESC s = d;
    s.Usage = D3D11_USAGE_STAGING;
    s.BindFlags = 0;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&s, nullptr, &staging))) {
        return "FAILED: staging texture";
    }
    constexpr UINT kStep = 2; // the PNG and the statistics use every 2nd pixel
    const UINT w = d.Width / kStep, h = d.Height / kStep;
    std::vector<std::uint8_t> bgr(static_cast<std::size_t>(w) * 2 * h * 3);
    std::string stats;
    d3d::InternalScope internal;
    for (int k = 0; k < 2; ++k) {
        context->CopyResource(staging.Get(), g_motion[k].Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
            return "FAILED: map";
        }
        double sum = 0, peak = 0;
        std::uint64_t moving = 0, count = 0;
        for (UINT y = 0; y < h; ++y) {
            const auto* row = reinterpret_cast<const DirectX::PackedVector::HALF*>(
                static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * kStep * m.RowPitch);
            for (UINT x = 0; x < w; ++x) {
                const float px = DirectX::PackedVector::XMConvertHalfToFloat(row[x * kStep * 2]) * d.Width;
                const float py = DirectX::PackedVector::XMConvertHalfToFloat(row[x * kStep * 2 + 1]) * d.Height;
                const double len = std::sqrt(static_cast<double>(px) * px + static_cast<double>(py) * py);
                sum += len;
                peak = std::max(peak, len);
                moving += len > 1.0 ? 1 : 0;
                ++count;
                std::uint8_t* out = &bgr[(static_cast<std::size_t>(y) * w * 2 + k * w + x) * 3];
                out[0] = 128;
                out[1] = static_cast<std::uint8_t>(std::clamp(128.0f + py * 16.0f, 0.0f, 255.0f));
                out[2] = static_cast<std::uint8_t>(std::clamp(128.0f + px * 16.0f, 0.0f, 255.0f));
            }
        }
        context->Unmap(staging.Get(), 0);
        stats += fmt::format("{}{} eye: mean {:.3f} px, max {:.1f} px, {:.2f}% above 1 px", k ? "; " : "", k ? "right" : "left",
            count ? sum / count : 0.0, peak, count ? 100.0 * moving / count : 0.0);
    }
    const auto dir = core::CaptureDir() / "screens";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / "motion_vectors.png";
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ok = d3d::dump::WritePngBgr(path, w * 2, h, bgr);
    if (SUCCEEDED(co)) {
        CoUninitialize();
    }
    return stats + (ok ? "; " + path.string() : "; PNG not written");
}

} // namespace

void RequestMotionSave(std::function<void(const std::string&)> done)
{
    if (g_motionSaveDone) {
        g_motionSaveDone("mvsave FAILED: superseded by a newer request");
    }
    g_motionSaveDone = std::move(done);
}

void OnTemporalDebugLoopRender(void* renderLoop)
{
    const bool twoViews = SplitViewActive() && renderLoop;
    if (g_renderLoop != (twoViews ? renderLoop : nullptr)) {
        g_srvs.clear();
    }
    g_renderLoop = twoViews ? renderLoop : nullptr;
    if (twoViews && --g_vramCountdown <= 0) {
        g_vramCountdown = 30;
        UpdateVram(renderLoop);
    }
    if (!twoViews || (!g_shown && !g_freeze)) {
        Disarm();
        if (!g_freeze) {
            g_frozen.Reset();
        }
        if (!g_shown) {
            g_motion[0].Reset();
            g_motion[1].Reset();
        }
        return;
    }
    ID3D11Texture2D* history[2];
    for (int k = 0; k < 2; ++k) {
        history[k] = RenderTargetTextureAt(renderLoop, kRenderLoopTaaHistory + k * 0x10); // read by this frame's TAA
    }
    ID3D11Texture2D* motion = g_shown ? RenderTargetTextureAt(renderLoop, kRenderLoopMotionVectors) : nullptr;
    for (int k = 0; k < 2; ++k) {
        g_motionCopy[k] = EnsureCopyOf(g_motion[k], motion) ? g_motion[k].Get() : nullptr;
    }
    if (g_freeze && history[1]) {
        if (!g_frozen || !SameDesc(g_frozen.Get(), history[1])) {
            g_frozenValid = false; // recaptured on the next view-1 resolve (new history RTs after a reset)
        }
        g_frozenCopy = EnsureCopyOf(g_frozen, history[1]) ? g_frozen.Get() : nullptr;
    }
    else {
        g_frozenCopy = nullptr;
        g_frozen.Reset();
        g_frozenValid = false;
    }
    for (int k = 0; k < 2; ++k) {
        g_history[k] = history[k];
    }
    if (!g_armed) {
        g_armed = true;
        d3d::SetPreDrawCallback(&DebugPreDraw, 1);
    }
}

void UpdateTemporalViews(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (g_motionSaveDone && device && context) {
        auto done = std::move(g_motionSaveDone);
        g_motionSaveDone = nullptr;
        const std::string result = "mvsave " + SaveMotion(device, context);
        TT2VR_LOG_INFO("{}", result);
        done(result);
    }
    if (!g_shown || !g_renderLoop || !device || !context) {
        return;
    }
    if (!g_blitterTried) {
        g_blitterTried = true;
        if (!g_blitter.Init(device)) {
            TT2VR_LOG_WARN("temporal debug: visualizer shaders failed; no history views");
        }
    }
    ID3D11Texture2D* eye0 = RenderTargetTextureAt(g_renderLoop, kRenderLoopTaaHistory);
    if (!g_blitter.Ready() || !eye0 || !EnsureThumbs(device, eye0)) {
        return;
    }
    for (int k = 0; k < 2; ++k) {
        // After the frame: +0xce8 holds the history the next frame reads (this frame's TAA output).
        ID3D11Texture2D* sources[kViews] = {
            RenderTargetTextureAt(g_renderLoop, kRenderLoopTaaHistory + k * 0x10),
            g_motion[k].Get(),
            RenderTargetTextureAt(g_renderLoop, kRenderLoopLastFrameDepth + k * 8),
        };
        constexpr d3d::Blitter::View kModes[kViews] = {
            d3d::Blitter::View::Raw, d3d::Blitter::View::Motion, d3d::Blitter::View::Depth};
        for (int v = 0; v < kViews; ++v) {
            if (ID3D11ShaderResourceView* srv = ViewOf(sources[v])) {
                g_blitter.Visualize(context, srv, g_thumbs[k][v].rtv.Get(), g_thumbW, g_thumbH, kModes[v]);
            }
        }
    }
}

void SetTemporalViewsShown(bool shown)
{
    if (shown != g_shown) {
        TT2VR_LOG_INFO("temporal debug: history views {}", shown ? "on" : "off");
    }
    g_shown = shown;
}

bool TemporalViewsShown()
{
    return g_shown;
}

ID3D11ShaderResourceView* EyeDepthView(int eye)
{
    return ViewOf(EyeDepthTexture(eye));
}

ID3D11Texture2D* EyeDepthTexture(int eye)
{
    return g_renderLoop && (eye == 0 || eye == 1)
               ? RenderTargetTextureAt(g_renderLoop, kRenderLoopLastFrameDepth + eye * 8)
               : nullptr;
}

void SetFreezeRightHistory(bool freeze)
{
    if (freeze != g_freeze) {
        TT2VR_LOG_INFO("temporal debug: [debug] eye R TAA history {}", freeze ? "FROZEN" : "running");
    }
    g_freeze = freeze;
}

bool FreezeRightHistory()
{
    return g_freeze;
}

ID3D11ShaderResourceView* TemporalThumbnail(int eye, TemporalView view)
{
    const int v = static_cast<int>(view);
    if (!g_shown || !g_renderLoop || eye < 0 || eye > 1 || v < 0 || v >= kViews) {
        return nullptr;
    }
    return g_thumbs[eye][v].srv.Get();
}

void TemporalThumbnailSize(unsigned& width, unsigned& height)
{
    width = g_thumbW;
    height = g_thumbH;
}

TemporalVram TemporalVramUsage()
{
    return g_renderLoop ? g_vram : TemporalVram{};
}

} // namespace game
