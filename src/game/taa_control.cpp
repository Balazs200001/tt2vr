#include "game/taa_control.hpp"

#include "core/hooks.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "d3d/blit.hpp"
#include "d3d/context_hooks.hpp"
#include "d3d/internal.hpp"
#include "d3d/screenshot.hpp"
#include "d3d/texture_dump.hpp"
#include "game/engine_layout.hpp"
#include "game/engine_textures.hpp"
#include "game/signatures.hpp"
#include "game/split_view.hpp"
#include "stereo/math/cut_detector.hpp"

#include <spdlog/fmt/fmt.h>
#include <wrl/client.h>

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace game {
namespace {

using namespace layout;
using Microsoft::WRL::ComPtr;

using RenderTaaFn = void (*)(void* renderLoop, void* renderContext);

using PostUpdateFn = void (*)(void* renderLoop, const void* renderContext); // RenderLoop::PostUpdateLastFrameCamera

RenderTaaFn g_renderTaa = nullptr;
PostUpdateFn g_postUpdate = nullptr; // null: a reset leaves the previous camera (signature missing)
bool g_active = false;
bool g_fullResetLogged = false;

// Leak-test tint: eye 0's TAA history texture this frame (set at record time); the pre-draw callback clears the
// FrameBuffer input (slot t1) of the one draw whose LastFrameBuffer (t2) is this texture, i.e. eye 0's TAA resolve.
std::atomic<ID3D11Texture2D*> g_tintHistory{nullptr};
std::atomic<bool> g_tintApplied{false};

// Main thread state.
bool g_resetPending = false; // this frame: reset both eyes' TAA history
bool g_fullReset = false; // ... and the other histories (froxel, previous camera, SSR); not for the convergence
                          // reference, which measures TAA alone
bool g_resetNext = false;
unsigned g_recenterSeen = 0;
bool g_recenterInit = false;

// Leak test state machine, advanced at Present. The row says whose eye-0 history is tinted: taa (eye 0's TAA input
// for one frame, so its history turns red), ssr (slot 0's 'Last Frame Buffer', SSR's history) or froxel (slot 0's
// froxel volume written this frame, read as history by the next one); the two last ones are overwritten between two
// frames on the immediate context (PLAN 6g).
enum class Leak { Idle, Baseline, Tint, After };
enum class LeakRow { Taa, Ssr, Froxel };
Leak g_leak = Leak::Idle;
LeakRow g_leakRow = LeakRow::Taa;
void* g_lastRenderLoop = nullptr; // the RenderLoop of the last RenderTAA (main thread)
double g_baseExcess[2] = {}, g_maxExcessDelta[2] = {};
bool g_tintThisFrame = false; // armed for the coming frame's recording
int g_afterFrames = 0;
std::vector<std::string> g_leakLines;
double g_baseLeft = 0, g_baseRight = 0, g_tintLeft = 0, g_maxRightDelta = 0;
std::function<void(const std::string&)> g_leakDone;
ComPtr<ID3D11Texture2D> g_staging;
D3D11_TEXTURE2D_DESC g_stagingDesc{};

constexpr int kAfterFrames = 6;

// Static convergence test, advanced at Present.
int g_convFrames = 0; // frames still to read; 0 = idle
std::vector<std::uint8_t> g_convPrev[2];
std::uint64_t g_convSum[2] = {}, g_convHot[2] = {}, g_convCount[2] = {};
std::function<void(const std::string&)> g_convDone;
std::vector<std::uint8_t> g_convPrevFull; // whole back buffer, sample grid order
std::vector<std::uint8_t> g_convDiff; // |d luma| * 8 of the last frame pair, sample grid order
UINT g_convGridW = 0, g_convGridH = 0;
float g_convCamMove = 0, g_convCamTurn = 0; // largest game-camera step between two frames while measuring (m, deg)
constexpr int kConvFrames = 11; // 10 consecutive-frame differences per eye
constexpr int kConvHot = 8; // a sample whose luma changes by more than this (of 255) counts as shimmering
// Pass: per eye, the frame-to-frame change with history is below kConvMaxRatio x the change with the history reset
// every frame, and the eyes' changes differ by less than kConvMaxAsym. An absolute limit does not work: wind in the
// foliage and the bike's idle shake (0.4 mm / 0.04 deg per frame) keep changing in any frame (2026-10-21 diff image).
constexpr double kConvMaxRatio = 0.6;
constexpr double kConvMaxAsym = 0.25;
int g_convPhase = 0; // 0 = with history, 1 = history reset every frame
double g_convMeanA[2] = {}, g_convHotA[2] = {};
std::string g_convImage;

// Cut test (remote "cuttest [noreset]"), advanced at Present: waits for the detector's next cut, then reads the
// frames after it. noreset = that one cut keeps its history (A/B: shows what the reset prevents).
enum class CutTest { Idle, Waiting, After };
CutTest g_cutTest = CutTest::Idle;
bool g_cutNoReset = false;
int g_cutFrame = 0;
std::chrono::steady_clock::time_point g_cutDeadline{};
std::vector<std::uint8_t> g_cutPrev[2];
std::vector<double> g_cutDiff[2]; // mean |d luma| between consecutive frames after the cut, per eye
std::string g_cutWhat; // the detector's line for the cut
std::function<void(const std::string&)> g_cutDone;
constexpr int kCutFrames = 12;
constexpr auto kCutWait = std::chrono::seconds(60);
// Shift test (remote "shifttest", PLAN 6c): the same frame readings after the next World Shift inside two-view riding
// instead of a cut; on the shift frame each view's RenderTAA pre-hook checks its slot's rebased previous camera
// position (RenderLoop+0xf58+idx*0xc, rebased by PreUpdateLastFrameCamera right before) against the view's camera.
bool g_cutIsShift = false;
bool g_shiftCheck = false; // this frame is the shift frame: record the per-slot check at RenderTAA
std::vector<std::string> g_shiftLines;
int g_resetsDuringTest = 0; // history resets while the frames after the shift are read (expected 0)
constexpr auto kShiftWait = std::chrono::seconds(300);

// Camera-cut detector (main thread): view 0's game camera (before the head pose), rules in stereo/math/cut_detector.hpp.
stereo::CutDetector g_cutDetector;
bool g_shiftThisFrame = false; // Renderer+0xd28 was set at this frame's DoUpdate: the world moved by -g_shift
stereo::Vec3 g_shift{};
// Other reset triggers (main thread).
const void* g_lastModeKey = nullptr;
float g_lastEyeDistance = 0;
bool g_lastTwoViews = false;
constexpr float kIpdChange = 0.001f; // m

float Distance(const float (&a)[3], const float (&b)[3])
{
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Eye 0's TAA history RT (read by RenderTAA as LastFrameBuffer).
ID3D11Texture2D* Eye0History(void* renderLoop)
{
    return RenderTargetTextureAt(renderLoop, kRenderLoopTaaHistory);
}

// Froxel history of both camera indices: -1 makes LightFroxel run its non-temporal technique once (temporal.md 10.1).
// SEH: engine objects.
bool ResetFroxelHistory(void* renderLoop)
{
    __try {
        void* lighting = Read<void*>(renderLoop, kRenderLoopDeferredLighting);
        if (!lighting) {
            return false;
        }
        for (int idx = 0; idx < 2; ++idx) {
            Write<int>(lighting, kLightingFroxelIndex + idx * 4, -1);
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Clears every mip of a render-target texture to `color` on the immediate context (a reset clears the last-frame RTs to
// 0: far depth, black). Thread: game main thread, outside the engine's recording and execution of a frame.
bool ClearTexture(ID3D11Texture2D* texture, const float (&color)[4])
{
    D3D11_TEXTURE2D_DESC d{};
    texture->GetDesc(&d);
    if (!(d.BindFlags & D3D11_BIND_RENDER_TARGET)) {
        return false;
    }
    ComPtr<ID3D11Device> device;
    texture->GetDevice(&device);
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = d.Format == DXGI_FORMAT_R32_TYPELESS             ? DXGI_FORMAT_R32_FLOAT
                : d.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                                 : d.Format;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    d3d::InternalScope internal;
    for (UINT mip = 0; mip < d.MipLevels; ++mip) {
        rd.Texture2D.MipSlice = mip;
        ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(device->CreateRenderTargetView(texture, &rd, &rtv))) {
            return false;
        }
        context->ClearRenderTargetView(rtv.Get(), color);
    }
    return true;
}

// The froxel volume (Texture3D*) camera index `idx` wrote this frame: index DeferredLighting+0x780+idx*4 holds the
// volume just written, which the next frame reads as its history. SEH: engine objects.
void* CurrentFroxelVolume(void* renderLoop, int idx)
{
    __try {
        void* lighting = Read<void*>(renderLoop, kRenderLoopDeferredLighting);
        if (!lighting) {
            return nullptr;
        }
        const int pp = Read<int>(lighting, kLightingFroxelIndex + idx * 4);
        return pp == 0 || pp == 1 ? Read<void*>(lighting, kLightingFroxelVolumes + (pp * 2 + idx) * 8) : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Leak-test tint of eye 0's SSR or froxel history, between two frames. Returns an error, or empty.
std::string TintRow(LeakRow row)
{
    void* loop = g_lastRenderLoop;
    if (!loop) {
        return "no RenderTAA seen yet";
    }
    if (row == LeakRow::Ssr) {
        // HDR, far brighter than the scene: dry-road reflections are weak (an 8.0 tint changed the image by 0.02).
        constexpr float kRed[4] = {512.0f, 0.0f, 0.0f, 1.0f};
        ID3D11Texture2D* texture = RenderTargetTextureAt(loop, kRenderLoopLastFrameBuffer); // slot 0
        return !texture ? "slot 0 'Last Frame Buffer' missing" : ClearTexture(texture, kRed) ? "" : "clear failed";
    }
    ID3D11Resource* volume = Texture3DResource(CurrentFroxelVolume(loop, 0));
    ComPtr<ID3D11Texture3D> texture;
    if (!volume || FAILED(volume->QueryInterface(IID_PPV_ARGS(&texture)))) {
        return "slot 0 froxel volume missing";
    }
    D3D11_TEXTURE3D_DESC d{};
    texture->GetDesc(&d);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = d.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS ? DXGI_FORMAT_R16G16B16A16_FLOAT : d.Format;
    ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
    ud.Texture3D.WSize = static_cast<UINT>(-1);
    ComPtr<ID3D11Device> device;
    texture->GetDevice(&device);
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    ComPtr<ID3D11UnorderedAccessView> uav;
    if (!(d.BindFlags & D3D11_BIND_UNORDERED_ACCESS) ||
        FAILED(device->CreateUnorderedAccessView(texture.Get(), &ud, &uav))) {
        return "no UAV on the froxel volume";
    }
    d3d::InternalScope internal;
    constexpr float kRed[4] = {2.0f, 0.0f, 0.0f, 0.05f}; // in-scattered light, small extinction (layout: guess)
    context->ClearUnorderedAccessViewFloat(uav.Get(), kRed);
    return "";
}

// Thread: any context's Draw (deferred contexts record on job threads). Set only while a leak test runs.
void TintPreDraw(ID3D11DeviceContext* c)
{
    ID3D11Texture2D* target = g_tintHistory.load(std::memory_order_acquire);
    if (!target) {
        return;
    }
    d3d::InternalScope internal; // our own calls on the hooked context are not traced
    ID3D11ShaderResourceView* srv[2] = {}; // t1 FrameBuffer, t2 LastFrameBuffer
    c->PSGetShaderResources(1, 2, srv);
    ComPtr<ID3D11ShaderResourceView> input(srv[0]), history(srv[1]);
    if (srv[0]) srv[0]->Release();
    if (srv[1]) srv[1]->Release();
    if (!input || !history) {
        return;
    }
    ComPtr<ID3D11Resource> historyRes;
    history->GetResource(&historyRes);
    if (historyRes.Get() != target || !g_tintHistory.compare_exchange_strong(target, nullptr)) {
        return;
    }
    ComPtr<ID3D11Resource> inputRes;
    input->GetResource(&inputRes);
    ComPtr<ID3D11Texture2D> inputTex;
    if (!inputRes || FAILED(inputRes.As(&inputTex))) {
        return;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    input->GetDesc(&sd); // typed, also when the texture is typeless
    D3D11_TEXTURE2D_DESC d{};
    inputTex->GetDesc(&d);
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = sd.Format != DXGI_FORMAT_UNKNOWN ? sd.Format : d3d::LinearViewOf(d.Format);
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11Device> device;
    c->GetDevice(&device);
    ComPtr<ID3D11RenderTargetView> rtv;
    if (!device || FAILED(device->CreateRenderTargetView(inputTex.Get(), &rd, &rtv))) {
        return;
    }
    constexpr float kRed[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    c->ClearRenderTargetView(rtv.Get(), kRed); // recorded right before eye 0's TAA resolve on this context
    g_tintApplied = true;
}

// Thread: game main thread (per-view chain of DeferredRenderLoop::Render, record time).
void HookedRenderTaa(void* renderLoop, void* renderContext)
{
    if (renderLoop && renderContext) {
        g_lastRenderLoop = renderLoop;
        if (g_resetPending) {
            Write<std::uint8_t>(renderLoop, kRenderLoopTaaReset, 1); // cleared again at the end of this view's TAA
        }
        if (g_tintThisFrame && Read<std::uint16_t>(renderContext, kRenderContextCameraIndex) == 0) {
            g_tintHistory = Eye0History(renderLoop); // before RenderTAA swaps the pair
            g_tintThisFrame = false;
        }
        if (g_shiftCheck) {
            // RenderContext+0x130 holds the view's ViewContext copy; its camera position is at +0.
            const unsigned idx = Read<std::uint16_t>(renderContext, kRenderContextCameraIndex);
            float last[3], cur[3];
            for (int i = 0; i < 3; ++i) {
                last[i] = Read<float>(renderLoop, kRenderLoopLastCamPos + idx * 0xc + i * 4);
                cur[i] = Read<float>(renderContext, kRenderContextView + kViewCamPos + i * 4);
            }
            g_shiftLines.push_back(fmt::format("view {}: previous camera {:.2f} m from the current after the rebase{}",
                idx, Distance(last, cur), g_resetPending ? " (RESET pending)" : ""));
        }
    }
    g_renderTaa(renderLoop, renderContext);
}

constexpr UINT kStep = 4; // back-buffer readbacks sample every 4th pixel in x and y

void Rgb(const std::uint8_t* p, bool bgra, int& r, int& g, int& b)
{
    r = bgra ? p[2] : p[0];
    g = p[1];
    b = bgra ? p[0] : p[2];
}

// Copies the back buffer to a staging texture, maps it and calls fn(data, rowPitch, width, height, bgra).
// Stalls once per call.
template <class Fn>
bool ReadBackBuffer(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context, Fn&& fn)
{
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        return false;
    }
    D3D11_TEXTURE2D_DESC d{};
    back->GetDesc(&d);
    const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                      d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    if (!g_staging || g_stagingDesc.Width != d.Width || g_stagingDesc.Height != d.Height ||
        g_stagingDesc.Format != d.Format) {
        D3D11_TEXTURE2D_DESC s = d;
        s.Usage = D3D11_USAGE_STAGING;
        s.BindFlags = 0;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        s.MiscFlags = 0;
        s.MipLevels = 1;
        s.ArraySize = 1;
        s.SampleDesc = {1, 0};
        g_staging.Reset();
        if (FAILED(device->CreateTexture2D(&s, nullptr, &g_staging))) {
            return false;
        }
        g_stagingDesc = d;
    }
    d3d::InternalScope internal;
    context->CopyResource(g_staging.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(context->Map(g_staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        return false;
    }
    fn(static_cast<const std::uint8_t*>(m.pData), m.RowPitch, d.Width, d.Height, bgra);
    context->Unmap(g_staging.Get(), 0);
    return true;
}

// Red-dominant share (%) of each back-buffer half, and its mean red excess max(0, r - max(g, b)) (of 255), which also
// sees a tint that only reaches part of the image (SSR reflections, thin fog).
bool RedShares(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context, double& left,
    double& right, double (&excess)[2])
{
    std::uint64_t red[2] = {0, 0}, total[2] = {0, 0}, excessSum[2] = {0, 0};
    const bool ok = ReadBackBuffer(swapChain, device, context,
        [&](const std::uint8_t* data, UINT pitch, UINT width, UINT height, bool bgra) {
            const UINT half = width / 2;
            for (UINT y = 0; y < height; y += kStep) {
                const std::uint8_t* row = data + static_cast<std::size_t>(y) * pitch;
                for (UINT x = 0; x < width; x += kStep) {
                    int r = 0, g = 0, b = 0;
                    Rgb(row + x * 4, bgra, r, g, b);
                    const int side = x < half ? 0 : 1;
                    ++total[side];
                    if (r > 128 && r * 2 > g * 5 && r * 2 > b * 5) {
                        ++red[side];
                    }
                    excessSum[side] += static_cast<std::uint64_t>(std::max(0, r - std::max(g, b)));
                }
            }
        });
    left = total[0] ? 100.0 * static_cast<double>(red[0]) / static_cast<double>(total[0]) : 0.0;
    right = total[1] ? 100.0 * static_cast<double>(red[1]) / static_cast<double>(total[1]) : 0.0;
    for (int e = 0; e < 2; ++e) {
        excess[e] = total[e] ? static_cast<double>(excessSum[e]) / static_cast<double>(total[e]) : 0.0;
    }
    return ok;
}

// Luma (0..255) of each back-buffer half's samples, and of the whole sample grid (`full`, gridW x gridH).
bool LumaSamples(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context,
    std::vector<std::uint8_t> (&out)[2], std::vector<std::uint8_t>& full, UINT& gridW, UINT& gridH)
{
    out[0].clear();
    out[1].clear();
    full.clear();
    return ReadBackBuffer(swapChain, device, context,
        [&](const std::uint8_t* data, UINT pitch, UINT width, UINT height, bool bgra) {
            const UINT half = width / 2;
            gridW = (width + kStep - 1) / kStep;
            gridH = (height + kStep - 1) / kStep;
            for (UINT y = 0; y < height; y += kStep) {
                const std::uint8_t* row = data + static_cast<std::size_t>(y) * pitch;
                for (UINT x = 0; x < width; x += kStep) {
                    int r = 0, g = 0, b = 0;
                    Rgb(row + x * 4, bgra, r, g, b);
                    const auto luma = static_cast<std::uint8_t>((2 * r + 5 * g + b) >> 3);
                    out[x < half ? 0 : 1].push_back(luma);
                    full.push_back(luma);
                }
            }
        });
}

// The last frame pair's change (|d luma| x 8, both eyes side by side) for a look at where the image moves.
std::string SaveConvergenceDiff()
{
    if (g_convDiff.size() != static_cast<std::size_t>(g_convGridW) * g_convGridH || g_convDiff.empty()) {
        return "no diff image";
    }
    std::vector<std::uint8_t> bgr(g_convDiff.size() * 3);
    for (std::size_t i = 0; i < g_convDiff.size(); ++i) {
        bgr[i * 3] = bgr[i * 3 + 1] = bgr[i * 3 + 2] = g_convDiff[i];
    }
    const auto dir = core::CaptureDir() / "screens";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / "convergence_diff.png";
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ok = d3d::dump::WritePngBgr(path, g_convGridW, g_convGridH, bgr);
    if (SUCCEEDED(co)) {
        CoUninitialize();
    }
    return ok ? "diff image " + path.string() : "diff image not written";
}

void FinishConvergence(const std::string& result)
{
    TT2VR_LOG_INFO("convergence: {}", result);
    g_convFrames = 0;
    g_convPrev[0].clear();
    g_convPrev[1].clear();
    g_convPrevFull.clear();
    g_convDiff.clear();
    if (g_convDone) {
        auto done = std::move(g_convDone);
        g_convDone = nullptr;
        done("convergence " + result);
    }
}

void ConvergenceFrame(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (!SplitViewActive()) {
        FinishConvergence("FAILED: not in two-view riding (stand still in the helmet camera, unpaused)");
        return;
    }
    std::vector<std::uint8_t> current[2], full;
    if (!LumaSamples(swapChain, device, context, current, full, g_convGridW, g_convGridH)) {
        FinishConvergence("FAILED: back buffer readback");
        return;
    }
    if (g_convPrevFull.size() == full.size()) {
        g_convDiff.resize(full.size());
        for (std::size_t i = 0; i < full.size(); ++i) {
            const int d = std::abs(static_cast<int>(full[i]) - static_cast<int>(g_convPrevFull[i]));
            g_convDiff[i] = static_cast<std::uint8_t>(std::min(d * 8, 255));
        }
    }
    g_convPrevFull = std::move(full);
    for (int e = 0; e < 2; ++e) {
        if (g_convPrev[e].size() == current[e].size()) {
            for (std::size_t i = 0; i < current[e].size(); ++i) {
                const int d = std::abs(static_cast<int>(current[e][i]) - static_cast<int>(g_convPrev[e][i]));
                g_convSum[e] += static_cast<std::uint64_t>(d);
                g_convHot[e] += d > kConvHot ? 1 : 0;
                ++g_convCount[e];
            }
        }
        g_convPrev[e] = std::move(current[e]);
    }
    if (--g_convFrames > 0) {
        return;
    }
    double mean[2] = {}, hot[2] = {};
    for (int e = 0; e < 2; ++e) {
        const auto n = static_cast<double>(g_convCount[e]);
        mean[e] = n > 0 ? static_cast<double>(g_convSum[e]) / n : 0.0;
        hot[e] = n > 0 ? 100.0 * static_cast<double>(g_convHot[e]) / n : 0.0;
    }
    if (g_convPhase == 0) {
        // Phase 2: the same scene with the history reset before every frame (no temporal accumulation), the
        // reference the normal frames must beat.
        for (int e = 0; e < 2; ++e) {
            g_convMeanA[e] = mean[e];
            g_convHotA[e] = hot[e];
            g_convSum[e] = g_convHot[e] = g_convCount[e] = 0;
            g_convPrev[e].clear();
        }
        g_convImage = SaveConvergenceDiff();
        g_convPrevFull.clear();
        g_convPhase = 1;
        g_convFrames = kConvFrames;
        return;
    }
    double ratio[2] = {};
    for (int e = 0; e < 2; ++e) {
        ratio[e] = mean[e] > 0.0 ? g_convMeanA[e] / mean[e] : 1.0;
    }
    const double asym = std::abs(g_convMeanA[0] - g_convMeanA[1]) / std::max({g_convMeanA[0], g_convMeanA[1], 1e-6});
    const bool pass = g_convCount[0] > 0 && g_convCount[1] > 0 && ratio[0] < kConvMaxRatio &&
                      ratio[1] < kConvMaxRatio && asym < kConvMaxAsym;
    FinishConvergence(fmt::format("{}: mean |d luma| frame to frame with history left {:.2f} right {:.2f}, history reset "
                                  "every frame left {:.2f} right {:.2f} (of 255); ratio left {:.2f} right {:.2f} (limit "
                                  "{:.2f}), eyes differ by {:.0f}% (limit {:.0f}%); samples changing by more than {}: "
                                  "{:.2f}% / {:.2f}% with history, {:.2f}% / {:.2f}% without; {} frames each; game camera "
                                  "moved up to {:.1f} mm / {:.3f} deg per frame; {}",
        pass ? "PASS" : "FAIL", g_convMeanA[0], g_convMeanA[1], mean[0], mean[1], ratio[0], ratio[1], kConvMaxRatio,
        asym * 100.0, kConvMaxAsym * 100.0, kConvHot, g_convHotA[0], g_convHotA[1], hot[0], hot[1], kConvFrames,
        g_convCamMove * 1000.0f, g_convCamTurn, g_convImage));
}

void FinishCutTest(const std::string& result)
{
    const char* name = g_cutIsShift ? "shifttest" : "cuttest";
    TT2VR_LOG_INFO("{}: {}", name, result);
    g_cutTest = CutTest::Idle;
    g_cutNoReset = false;
    g_cutIsShift = false;
    g_shiftCheck = false;
    if (g_cutDone) {
        auto done = std::move(g_cutDone);
        g_cutDone = nullptr;
        done(std::string(name) + " " + result);
    }
}

// After the cut: frame k's luma against frame k-1's, per eye; screenshots of the first two frames after the cut.
void CutTestFrame(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (g_cutTest == CutTest::Waiting) {
        if (std::chrono::steady_clock::now() > g_cutDeadline) {
            FinishCutTest(g_cutIsShift ? "FAILED: no World Shift within 5 min (ride more than 2 km)"
                                       : "FAILED: no camera cut within 60 s");
        }
        return;
    }
    g_shiftCheck = false; // the shift frame's views have been recorded
    std::vector<std::uint8_t> current[2], full;
    UINT w = 0, h = 0;
    if (!LumaSamples(swapChain, device, context, current, full, w, h)) {
        FinishCutTest("FAILED: back buffer readback");
        return;
    }
    if (g_cutFrame < 2) {
        // Captured at the next Present: frames +1 and +2 after the cut.
        const std::string name = g_cutIsShift
                                     ? fmt::format("shift_f{}", g_cutFrame + 1)
                                     : fmt::format("cut_{}_f{}", g_cutNoReset ? "noreset" : "reset", g_cutFrame + 1);
        d3d::screenshot::Request(name, nullptr);
    }
    for (int e = 0; e < 2; ++e) {
        if (g_cutPrev[e].size() == current[e].size() && !current[e].empty()) {
            std::uint64_t sum = 0;
            for (std::size_t i = 0; i < current[e].size(); ++i) {
                sum += static_cast<std::uint64_t>(
                    std::abs(static_cast<int>(current[e][i]) - static_cast<int>(g_cutPrev[e][i])));
            }
            g_cutDiff[e].push_back(static_cast<double>(sum) / static_cast<double>(current[e].size()));
        }
        g_cutPrev[e] = std::move(current[e]);
    }
    if (++g_cutFrame < kCutFrames) {
        return;
    }
    std::string lines;
    for (int e = 0; e < 2; ++e) {
        lines += e == 0 ? " left" : "; right";
        for (double d : g_cutDiff[e]) {
            lines += fmt::format(" {:.1f}", d);
        }
    }
    if (g_cutIsShift) {
        std::string checks;
        for (const auto& l : g_shiftLines) {
            checks += "; " + l;
        }
        const bool pass = g_resetsDuringTest == 0 && g_shiftLines.size() == 2;
        FinishCutTest(fmt::format("{}: {}{}; history resets in the {} frames after: {}; mean |d luma| frame to "
                                  "frame:{}",
            pass ? "PASS (no reset, both slots checked; judge the numbers and screenshots for smear)" : "CHECK",
            g_cutWhat, checks, kCutFrames, g_resetsDuringTest, lines));
        return;
    }
    FinishCutTest(fmt::format("{} cut ({}), history {}: mean |d luma| frame to frame after the cut:{}",
        g_cutNoReset ? "B" : "A", g_cutWhat, g_cutNoReset ? "kept (noreset)" : "reset", lines));
}

// A history reset of both eyes for this frame and the next (the trigger is seen before this frame's views render).
void ResetNow(const std::string& why)
{
    if (!g_active) {
        return;
    }
    g_resetPending = true;
    g_fullReset = true;
    g_resetNext = true;
    if (g_cutTest == CutTest::After) {
        ++g_resetsDuringTest;
    }
    TT2VR_LOG_INFO("taa: history reset for both eyes ({})", why);
}

// A camera cut (jump or mode change): ResetNow, unless a running "cuttest noreset" keeps the history for this one.
void OnCut(const std::string& what)
{
    // The cut test counts only cuts inside two-view riding (menu and loading cameras jump too).
    const bool testCut = g_cutTest == CutTest::Waiting && !g_cutIsShift && SplitViewActive();
    if (testCut && g_cutNoReset) {
        TT2VR_LOG_INFO("taa: cut test: history KEPT for this cut (noreset) ({})", what);
    }
    else {
        ResetNow(what);
    }
    if (testCut) {
        g_cutTest = CutTest::After;
        g_cutFrame = 0;
        g_cutWhat = what;
    }
}

void FinishLeak(const std::string& result)
{
    TT2VR_LOG_INFO("leaktest: {}", result);
    for (const auto& line : g_leakLines) {
        TT2VR_LOG_INFO("leaktest:   {}", line);
    }
    g_leak = Leak::Idle;
    g_tintThisFrame = false;
    g_tintHistory = nullptr;
    d3d::SetPreDrawCallback(nullptr);
    if (g_leakDone) {
        auto done = std::move(g_leakDone);
        g_leakDone = nullptr;
        done("leaktest " + result);
    }
}

} // namespace

void InstallTaaControl()
{
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;
    void* taa = SignatureAddress(Sig::RenderLoopRenderTaa);
    if (!taa || core::hooks::Create("RenderLoop::RenderTAA", taa, &HookedRenderTaa, &g_renderTaa) != core::hooks::Result::Ok) {
        TT2VR_LOG_WARN("taa: RenderTAA hook not installed; no per-eye TAA reset or leak test");
        return;
    }
    g_active = true;
    g_postUpdate = reinterpret_cast<PostUpdateFn>(SignatureAddress(Sig::RenderLoopPostUpdateLastFrameCamera));
    TT2VR_LOG_INFO("taa: reset and leak-test hook installed{}",
        g_postUpdate ? "" : "; PostUpdateLastFrameCamera missing: resets keep the previous camera");
}

void OnTaaFrame(void* renderer, unsigned recenterCount)
{
    g_shiftThisFrame = renderer && Read<std::uint8_t>(renderer, kRendererShiftFlag) != 0;
    if (g_shiftThisFrame) {
        g_shift = Read<stereo::Vec3>(renderer, kRendererShiftVector);
    }
    if (!g_recenterInit) {
        g_recenterInit = true;
        g_recenterSeen = recenterCount;
    }
    if (recenterCount != g_recenterSeen) {
        g_recenterSeen = recenterCount;
        RequestTaaReset("recenter");
    }
    g_fullReset = g_resetNext;
    g_resetPending = g_resetNext || (g_convFrames > 0 && g_convPhase == 1); // convergence reference: TAA only
    g_resetNext = false;
}

void RequestTaaReset(const char* reason)
{
    if (!g_active) {
        return;
    }
    g_resetNext = true;
    TT2VR_LOG_INFO("taa: history reset for both eyes ({})", reason);
}

void RequestLeakTest(const std::string& row, std::function<void(const std::string&)> done)
{
    if (row != "" && row != "taa" && row != "ssr" && row != "froxel") {
        done("leaktest FAILED: unknown row '" + row + "' (taa, ssr, froxel)");
        return;
    }
    if (!g_active) {
        done("leaktest FAILED: RenderTAA hook not installed");
        return;
    }
    if (g_leakDone) {
        g_leakDone("leaktest FAILED: superseded by a newer request");
    }
    g_leakDone = std::move(done);
    g_leakRow = row == "ssr" ? LeakRow::Ssr : row == "froxel" ? LeakRow::Froxel : LeakRow::Taa;
    g_leakLines.clear();
    g_maxRightDelta = 0;
    g_maxExcessDelta[0] = g_maxExcessDelta[1] = 0;
    g_tintApplied = false;
    g_leak = Leak::Baseline;
}

void OnMainCameraWorld(const float (&world)[3][4])
{
    const stereo::Vec3 pos{world[0][3], world[1][3], world[2][3]};
    const stereo::Vec3 fwd{world[0][2], world[1][2], world[2][2]};
    // UpdateShift ran before this DoUpdate when the flag is set: the camera moved with the world.
    const auto r = g_cutDetector.Update(pos, fwd, g_shiftThisFrame ? &g_shift : nullptr);
    if (r.first || !g_active) {
        return;
    }
    if (r.rebased) {
        TT2VR_LOG_INFO("taa: World Shift ({:.0f} {:.0f} {:.0f}): camera step {:.2f} m after the rebase ({:.0f} m "
                       "without), no cut",
            g_shift.x, g_shift.y, g_shift.z, r.distance, r.unrebasedDistance);
        if (g_cutTest == CutTest::Waiting && g_cutIsShift && SplitViewActive()) {
            g_cutTest = CutTest::After;
            g_cutFrame = 0;
            g_cutWhat = fmt::format("World Shift ({:.0f} {:.0f} {:.0f}), camera step {:.2f} m after the rebase",
                g_shift.x, g_shift.y, g_shift.z, r.distance);
            g_shiftCheck = true;
        }
    }
    if (g_convFrames > 0) {
        g_convCamMove = std::max(g_convCamMove, r.distance);
        g_convCamTurn = std::max(g_convCamTurn, r.turnDeg);
    }
    if (r.cut) {
        // The engine has no cut detection (docs/re/temporal.md N6). This frame's views are recorded after DoUpdate;
        // the next frame is reset too, in case its recording is the first to see the new camera.
        OnCut(fmt::format("camera cut: {:.1f} m (limit {:.1f}), {:.0f} deg", r.distance, r.limit, r.turnDeg));
    }
}

void OnTaaCameraMode(const void* modeKey)
{
    if (modeKey && g_lastModeKey && modeKey != g_lastModeKey && g_active) {
        OnCut(fmt::format("camera mode changed {} -> {}", g_lastModeKey, modeKey));
    }
    g_lastModeKey = modeKey;
}

void OnTaaEyeDistance(float eyeDistance)
{
    if (eyeDistance <= 0.0f) {
        return;
    }
    if (g_lastEyeDistance > 0.0f && std::abs(eyeDistance - g_lastEyeDistance) > kIpdChange) {
        ResetNow(fmt::format("IPD {:.1f} -> {:.1f} mm", g_lastEyeDistance * 1000.0f, eyeDistance * 1000.0f));
    }
    g_lastEyeDistance = eyeDistance;
}

void OnTaaTwoViews(bool twoViews)
{
    if (twoViews && !g_lastTwoViews) {
        ResetNow("two views on: new history render targets");
    }
    g_lastTwoViews = twoViews;
}

void OnTaaLoopRender(void* renderLoop)
{
    if (!g_fullReset || !renderLoop || !g_active) {
        return;
    }
    const bool froxel = ResetFroxelHistory(renderLoop);
    int cleared = 0;
    for (int idx = 0; idx < 2; ++idx) {
        for (const std::ptrdiff_t slot : {kRenderLoopLastFrameDepth, kRenderLoopLastFrameBuffer}) {
            if (ID3D11Texture2D* texture = RenderTargetTextureAt(renderLoop, slot + idx * 8)) {
                constexpr float kZero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                cleared += ClearTexture(texture, kZero) ? 1 : 0;
            }
        }
    }
    if (!g_fullResetLogged) {
        g_fullResetLogged = true;
        TT2VR_LOG_INFO("taa: a history reset covers TAA, froxel ({}), previous camera ({}) and SSR's last-frame "
                       "buffers ({} of 4 cleared); exposure keeps adapting",
            froxel ? "index -1" : "NOT reset", g_postUpdate ? "latched per view" : "NOT reset", cleared);
    }
}

void OnTaaViewPrepared(void* renderLoop, void* renderContext)
{
    if (!g_fullReset || !g_postUpdate || !renderLoop || !renderContext) {
        return;
    }
    const unsigned idx = Read<std::uint16_t>(renderContext, kRenderContextCameraIndex);
    if (idx > 1) {
        return;
    }
    // PostUpdateLastFrameCamera also latches the front-pass set from the front ViewContext (RenderLoop+0x1590), which
    // this view builds later: it still holds the previous view's (the other eye's) values, so that set is put back.
    char* loop = static_cast<char*>(renderLoop);
    std::uint8_t front[0x40 + 0xc + 8];
    std::memcpy(front, loop + kRenderLoopFrontLastVP + idx * 0x40, 0x40);
    std::memcpy(front + 0x40, loop + kRenderLoopFrontLastCamPos + idx * 0xc, 0xc);
    std::memcpy(front + 0x4c, loop + kRenderLoopFrontLastJitter + idx * 8, 8);
    g_postUpdate(renderLoop, renderContext);
    std::memcpy(loop + kRenderLoopFrontLastVP + idx * 0x40, front, 0x40);
    std::memcpy(loop + kRenderLoopFrontLastCamPos + idx * 0xc, front + 0x40, 0xc);
    std::memcpy(loop + kRenderLoopFrontLastJitter + idx * 8, front + 0x4c, 8);
}

void RequestCutTest(bool noReset, std::function<void(const std::string&)> done)
{
    if (!g_active) {
        done("cuttest FAILED: RenderTAA hook not installed");
        return;
    }
    if (g_cutDone) {
        g_cutDone("cuttest FAILED: superseded by a newer request");
    }
    g_cutDone = std::move(done);
    g_cutNoReset = noReset;
    g_cutPrev[0].clear();
    g_cutPrev[1].clear();
    g_cutDiff[0].clear();
    g_cutDiff[1].clear();
    g_cutDeadline = std::chrono::steady_clock::now() + kCutWait;
    g_cutIsShift = false;
    g_resetsDuringTest = 0;
    g_cutTest = CutTest::Waiting;
    TT2VR_LOG_INFO("cuttest: armed{}; waiting up to 60 s for a camera cut", noReset ? " (noreset)" : "");
}

void RequestShiftTest(std::function<void(const std::string&)> done)
{
    if (!g_active) {
        done("shifttest FAILED: RenderTAA hook not installed");
        return;
    }
    if (g_cutDone) {
        g_cutDone(std::string(g_cutIsShift ? "shifttest" : "cuttest") + " FAILED: superseded by a newer request");
    }
    g_cutDone = std::move(done);
    g_cutNoReset = false;
    g_cutPrev[0].clear();
    g_cutPrev[1].clear();
    g_cutDiff[0].clear();
    g_cutDiff[1].clear();
    g_shiftLines.clear();
    g_shiftCheck = false;
    g_resetsDuringTest = 0;
    g_cutIsShift = true;
    g_cutDeadline = std::chrono::steady_clock::now() + kShiftWait;
    g_cutTest = CutTest::Waiting;
    TT2VR_LOG_INFO("shifttest: armed; waiting up to 5 min for a World Shift while riding");
}

void RequestConvergenceTest(std::function<void(const std::string&)> done)
{
    if (g_convDone) {
        g_convDone("convergence FAILED: superseded by a newer request");
    }
    g_convDone = std::move(done);
    g_convPrev[0].clear();
    g_convPrev[1].clear();
    for (int e = 0; e < 2; ++e) {
        g_convSum[e] = g_convHot[e] = g_convCount[e] = 0;
    }
    g_convPrevFull.clear();
    g_convDiff.clear();
    g_convCamMove = g_convCamTurn = 0;
    g_convPhase = 0;
    g_convFrames = kConvFrames;
}

void OnTaaPresent(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (!swapChain || !device || !context) {
        return;
    }
    if (g_convFrames > 0) {
        ConvergenceFrame(swapChain, device, context);
    }
    if (g_cutTest != CutTest::Idle) {
        CutTestFrame(swapChain, device, context);
    }
    if (g_leak == Leak::Idle) {
        return;
    }
    if (!SplitViewActive()) {
        FinishLeak("FAILED: not in two-view riding (ride in the helmet camera, unpaused)");
        return;
    }
    double left = 0, right = 0, excess[2] = {};
    if (!RedShares(swapChain, device, context, left, right, excess)) {
        FinishLeak("FAILED: back buffer readback");
        return;
    }
    if (g_leak != Leak::Baseline) {
        for (int e = 0; e < 2; ++e) {
            g_maxExcessDelta[e] = std::max(g_maxExcessDelta[e], excess[e] - g_baseExcess[e]);
        }
    }
    switch (g_leak) {
    case Leak::Baseline:
        g_baseLeft = left;
        g_baseRight = right;
        g_baseExcess[0] = excess[0];
        g_baseExcess[1] = excess[1];
        g_leakLines.push_back(fmt::format("baseline: left {:.2f}% right {:.2f}% red, red excess {:.3f} / {:.3f}", left,
            right, excess[0], excess[1]));
        if (g_leakRow == LeakRow::Taa) {
            g_tintThisFrame = true; // the next frame's eye-0 TAA input becomes red
            g_tintApplied = false;
            d3d::SetPreDrawCallback(&TintPreDraw);
        }
        else if (const std::string error = TintRow(g_leakRow); !error.empty()) {
            FinishLeak("FAILED: " + error);
            return;
        }
        g_leak = Leak::Tint;
        break;
    case Leak::Tint:
        if (g_leakRow == LeakRow::Taa) {
            d3d::SetPreDrawCallback(nullptr);
            if (!g_tintApplied) {
                FinishLeak(g_tintHistory.load() ? "FAILED: eye 0's TAA draw was not found (history texture not bound)"
                                                : "FAILED: no eye-0 RenderTAA this frame (TAA off?)");
                return;
            }
        }
        g_tintLeft = left;
        g_maxRightDelta = std::max(g_maxRightDelta, right - g_baseRight);
        g_leakLines.push_back(fmt::format("tint frame: left {:.2f}% right {:.2f}%, red excess {:.3f} / {:.3f}", left,
            right, excess[0], excess[1]));
        g_afterFrames = 0;
        g_leak = Leak::After;
        break;
    case Leak::After:
        g_maxRightDelta = std::max(g_maxRightDelta, right - g_baseRight);
        g_leakLines.push_back(fmt::format("frame +{}: left {:.2f}% right {:.2f}%, red excess {:.3f} / {:.3f}",
            ++g_afterFrames, left, right, excess[0], excess[1]));
        if (g_afterFrames >= kAfterFrames) {
            const char* name = g_leakRow == LeakRow::Ssr ? "ssr" : g_leakRow == LeakRow::Froxel ? "froxel" : "taa";
            constexpr const char* kInconclusive = "INCONCLUSIVE (tint not visible in the left eye)";
            if (g_leakRow == LeakRow::Taa) {
                const bool tintSeen = g_tintLeft - g_baseLeft > 20.0;
                const bool noLeak = g_maxRightDelta < 1.0;
                FinishLeak(fmt::format("{} {}: left eye {:.1f}% -> {:.1f}% red on the tint frame, right eye max "
                                       "+{:.2f}% over its baseline {:.2f}%",
                    name, tintSeen && noLeak ? "PASS" : !tintSeen ? kInconclusive : "FAIL", g_baseLeft, g_tintLeft,
                    g_maxRightDelta, g_baseRight));
                break;
            }
            // SSR and froxel tints reach part of the image only: judged on the mean red excess. Pass: eye 0 gains
            // more than 0.5 (of 255), eye 1 less than 0.15 or 5% of eye 0's gain (frame noise), and no red share jump.
            const double l = g_maxExcessDelta[0], r = g_maxExcessDelta[1];
            const bool tintSeen = l > 0.5;
            const bool noLeak = r < std::max(0.15, 0.05 * l) && g_maxRightDelta < 1.0;
            FinishLeak(fmt::format("{} {}: red excess (of 255) left eye +{:.3f}, right eye +{:.3f} over the baselines "
                                   "{:.3f} / {:.3f}; red share right eye max +{:.2f}%",
                name, tintSeen && noLeak ? "PASS" : !tintSeen ? kInconclusive : "FAIL", l, r, g_baseExcess[0],
                g_baseExcess[1], g_maxRightDelta));
        }
        break;
    case Leak::Idle:
        break;
    }
}

} // namespace game
