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
#include "game/signatures.hpp"
#include "game/split_view.hpp"

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
#include <vector>

namespace game {
namespace {

using namespace layout;
using Microsoft::WRL::ComPtr;

using RenderTaaFn = void (*)(void* renderLoop, void* renderContext);
using GetTextureFn = void* (*)(void* renderTarget, int index); // RenderTarget vtable slot 8 (+0x40): BaseTexture*

RenderTaaFn g_renderTaa = nullptr;
bool g_active = false;

// Leak-test tint: eye 0's TAA history texture this frame (set at record time); the pre-draw callback clears the
// FrameBuffer input (slot t1) of the one draw whose LastFrameBuffer (t2) is this texture, i.e. eye 0's TAA resolve.
std::atomic<ID3D11Texture2D*> g_tintHistory{nullptr};
std::atomic<bool> g_tintApplied{false};

// Main thread state.
bool g_resetPending = false; // this frame: reset both eyes' TAA history
bool g_resetNext = false;
unsigned g_recenterSeen = 0;
bool g_recenterInit = false;

// Leak test state machine, advanced at Present.
enum class Leak { Idle, Baseline, Tint, After };
Leak g_leak = Leak::Idle;
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

// Camera-cut detector (main thread): view 0's game camera last frame (before the head pose).
bool g_haveLastCamera = false;
float g_lastCameraPos[3] = {};
float g_lastCameraFwd[3] = {};
constexpr float kCutDistance = 15.0f; // m in one frame; a bike at 330 km/h moves under 4 m per frame at 25 fps
constexpr float kCutCos = 0.7071f; // the camera's z axis turns more than 45 deg in one frame

// Eye 0's TAA history RT (RenderLoop+0xce8, read by RenderTAA as LastFrameBuffer) as a D3D texture:
// RenderTarget vtbl+0x40(0) -> Texture2DDX11, GetDeviceTexture = +0x68. SEH: engine objects.
ID3D11Texture2D* Eye0History(void* renderLoop)
{
    __try {
        void* rt = Read<void*>(renderLoop, kRenderLoopTaaHistory);
        if (!rt) {
            return nullptr;
        }
        const auto get = reinterpret_cast<GetTextureFn>((*static_cast<void* const* const*>(rt))[kRenderTargetTextureSlot]);
        void* texture = get(rt, 0);
        return texture ? Read<ID3D11Texture2D*>(texture, kTexture2DDeviceTexture) : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
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
        if (g_resetPending) {
            Write<std::uint8_t>(renderLoop, kRenderLoopTaaReset, 1); // cleared again at the end of this view's TAA
        }
        if (g_tintThisFrame && Read<std::uint16_t>(renderContext, kRenderContextCameraIndex) == 0) {
            g_tintHistory = Eye0History(renderLoop); // before RenderTAA swaps the pair
            g_tintThisFrame = false;
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

// Red-dominant share (%) of each back-buffer half.
bool RedShares(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context, double& left,
    double& right)
{
    std::uint64_t red[2] = {0, 0}, total[2] = {0, 0};
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
                }
            }
        });
    left = total[0] ? 100.0 * static_cast<double>(red[0]) / static_cast<double>(total[0]) : 0.0;
    right = total[1] ? 100.0 * static_cast<double>(red[1]) / static_cast<double>(total[1]) : 0.0;
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
    TT2VR_LOG_INFO("cuttest: {}", result);
    g_cutTest = CutTest::Idle;
    g_cutNoReset = false;
    if (g_cutDone) {
        auto done = std::move(g_cutDone);
        g_cutDone = nullptr;
        done("cuttest " + result);
    }
}

// After the cut: frame k's luma against frame k-1's, per eye; screenshots of the first two frames after the cut.
void CutTestFrame(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (g_cutTest == CutTest::Waiting) {
        if (std::chrono::steady_clock::now() > g_cutDeadline) {
            FinishCutTest("FAILED: no camera cut within 60 s");
        }
        return;
    }
    std::vector<std::uint8_t> current[2], full;
    UINT w = 0, h = 0;
    if (!LumaSamples(swapChain, device, context, current, full, w, h)) {
        FinishCutTest("FAILED: back buffer readback");
        return;
    }
    if (g_cutFrame < 2) {
        // Captured at the next Present: frames +1 and +2 after the cut.
        d3d::screenshot::Request(fmt::format("cut_{}_f{}", g_cutNoReset ? "noreset" : "reset", g_cutFrame + 1),
            nullptr);
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
    FinishCutTest(fmt::format("{} cut ({}), history {}: mean |d luma| frame to frame after the cut:{}",
        g_cutNoReset ? "B" : "A", g_cutWhat, g_cutNoReset ? "kept (noreset)" : "reset", lines));
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
    TT2VR_LOG_INFO("taa: reset and leak-test hook installed");
}

void OnTaaFrame(unsigned recenterCount)
{
    if (!g_recenterInit) {
        g_recenterInit = true;
        g_recenterSeen = recenterCount;
    }
    if (recenterCount != g_recenterSeen) {
        g_recenterSeen = recenterCount;
        RequestTaaReset("recenter");
    }
    g_resetPending = g_resetNext || (g_convFrames > 0 && g_convPhase == 1); // convergence reference phase
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

void RequestLeakTest(std::function<void(const std::string&)> done)
{
    if (!g_active) {
        done("leaktest FAILED: RenderTAA hook not installed");
        return;
    }
    if (g_leakDone) {
        g_leakDone("leaktest FAILED: superseded by a newer request");
    }
    g_leakDone = std::move(done);
    g_leakLines.clear();
    g_maxRightDelta = 0;
    g_tintApplied = false;
    g_leak = Leak::Baseline;
}

void OnMainCameraWorld(const float (&world)[3][4])
{
    const float pos[3] = {world[0][3], world[1][3], world[2][3]};
    const float fwd[3] = {world[0][2], world[1][2], world[2][2]};
    if (g_haveLastCamera && g_active) {
        const float dx = pos[0] - g_lastCameraPos[0], dy = pos[1] - g_lastCameraPos[1], dz = pos[2] - g_lastCameraPos[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float cosTurn = fwd[0] * g_lastCameraFwd[0] + fwd[1] * g_lastCameraFwd[1] + fwd[2] * g_lastCameraFwd[2];
        if (g_convFrames > 0) {
            g_convCamMove = std::max(g_convCamMove, dist);
            g_convCamTurn = std::max(g_convCamTurn, std::acos(std::clamp(cosTurn, -1.0f, 1.0f)) * 57.29578f);
        }
        if (dist > kCutDistance || cosTurn < kCutCos) {
            const float deg = std::acos(std::clamp(cosTurn, -1.0f, 1.0f)) * 57.29578f;
            // The cut test counts only cuts inside two-view riding (menu and loading cameras jump too).
            const bool testCut = g_cutTest == CutTest::Waiting && SplitViewActive();
            const bool suppress = testCut && g_cutNoReset;
            if (!suppress) {
                // The engine has no cut detection (docs/re/temporal.md N6). This frame's views are recorded after
                // DoUpdate; the next frame is reset too, in case its recording is the first to see the new camera.
                g_resetPending = true;
                g_resetNext = true;
            }
            TT2VR_LOG_INFO("taa: {} (camera cut: {:.1f} m, {:.0f} deg)",
                suppress ? "cut test: history KEPT for this cut (noreset)" : "history reset for both eyes", dist, deg);
            if (testCut) {
                g_cutTest = CutTest::After;
                g_cutFrame = 0;
                g_cutWhat = fmt::format("{:.1f} m, {:.0f} deg", dist, deg);
            }
        }
    }
    std::copy(pos, pos + 3, g_lastCameraPos);
    std::copy(fwd, fwd + 3, g_lastCameraFwd);
    g_haveLastCamera = true;
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
    g_cutTest = CutTest::Waiting;
    TT2VR_LOG_INFO("cuttest: armed{}; waiting up to 60 s for a camera cut", noReset ? " (noreset)" : "");
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
    double left = 0, right = 0;
    if (!RedShares(swapChain, device, context, left, right)) {
        FinishLeak("FAILED: back buffer readback");
        return;
    }
    switch (g_leak) {
    case Leak::Baseline:
        g_baseLeft = left;
        g_baseRight = right;
        g_leakLines.push_back(fmt::format("baseline: left {:.2f}% right {:.2f}% red", left, right));
        g_tintThisFrame = true; // the next frame's eye-0 TAA input becomes red
        g_tintApplied = false;
        d3d::SetPreDrawCallback(&TintPreDraw);
        g_leak = Leak::Tint;
        break;
    case Leak::Tint:
        d3d::SetPreDrawCallback(nullptr);
        if (!g_tintApplied) {
            FinishLeak(g_tintHistory.load() ? "FAILED: eye 0's TAA draw was not found (history texture not bound)"
                                            : "FAILED: no eye-0 RenderTAA this frame (TAA off?)");
            return;
        }
        g_tintLeft = left;
        g_maxRightDelta = std::max(g_maxRightDelta, right - g_baseRight);
        g_leakLines.push_back(fmt::format("tint frame: left {:.2f}% right {:.2f}%", left, right));
        g_afterFrames = 0;
        g_leak = Leak::After;
        break;
    case Leak::After:
        g_maxRightDelta = std::max(g_maxRightDelta, right - g_baseRight);
        g_leakLines.push_back(fmt::format("frame +{}: left {:.2f}% right {:.2f}%", ++g_afterFrames, left, right));
        if (g_afterFrames >= kAfterFrames) {
            const bool tintSeen = g_tintLeft - g_baseLeft > 20.0;
            const bool noLeak = g_maxRightDelta < 1.0;
            FinishLeak(fmt::format("{}: left eye {:.1f}% -> {:.1f}% red on the tint frame, right eye max +{:.2f}% "
                                   "over its baseline {:.2f}%",
                tintSeen && noLeak ? "PASS" : !tintSeen ? "INCONCLUSIVE (tint not visible in the left eye)" : "FAIL",
                g_baseLeft, g_tintLeft, g_maxRightDelta, g_baseRight));
        }
        break;
    case Leak::Idle:
        break;
    }
}

} // namespace game
