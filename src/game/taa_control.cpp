#include "game/taa_control.hpp"

#include "core/hooks.hpp"
#include "core/log.hpp"
#include "d3d/internal.hpp"
#include "game/engine_layout.hpp"
#include "game/signatures.hpp"
#include "game/split_view.hpp"

#include <spdlog/fmt/fmt.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace game {
namespace {

using namespace layout;
using Microsoft::WRL::ComPtr;

using RenderTaaFn = void (*)(void* renderLoop, void* renderContext);
using ClearRtFn = void (*)(void* renderTarget, unsigned flags, std::uint32_t color);
using CurrentRtFn = void* (*)(const void* postProcessManager);

RenderTaaFn g_renderTaa = nullptr;
ClearRtFn g_clearRt = nullptr;
CurrentRtFn g_currentRt = nullptr;
bool g_active = false;

// Main thread state.
bool g_resetPending = false; // this frame: reset both eyes' TAA history
bool g_resetNext = false;
unsigned g_recenterSeen = 0;
bool g_recenterInit = false;

// Leak test state machine, advanced at Present.
enum class Leak { Idle, Baseline, Tint, After };
Leak g_leak = Leak::Idle;
bool g_tintThisFrame = false; // armed for the coming frame's recording
bool g_tinted = false; // the tint was queued in this frame
int g_afterFrames = 0;
std::vector<std::string> g_leakLines;
double g_baseLeft = 0, g_baseRight = 0, g_tintLeft = 0, g_maxRightDelta = 0;
std::function<void(const std::string&)> g_leakDone;
ComPtr<ID3D11Texture2D> g_staging;
D3D11_TEXTURE2D_DESC g_stagingDesc{};

constexpr unsigned kClearColor = 1; // RenderLayer::ClearRT flags: colour (CopySplitRT passes 0xd = colour|depth|stencil)
constexpr std::uint32_t kRed = 0xffff0000; // Color32 ARGB
constexpr int kAfterFrames = 6;

// Thread: game main thread (per-view chain of DeferredRenderLoop::Render, record time).
void HookedRenderTaa(void* renderLoop, void* renderContext)
{
    if (renderLoop && renderContext) {
        if (g_resetPending) {
            Write<std::uint8_t>(renderLoop, kRenderLoopTaaReset, 1); // cleared again at the end of this view's TAA
        }
        if (g_tintThisFrame && Read<std::uint16_t>(renderContext, kRenderContextCameraIndex) == 0) {
            if (const void* ppm = Read<const void*>(renderLoop, kRenderLoopPostProcess)) {
                if (void* input = g_currentRt(ppm)) {
                    g_clearRt(input, kClearColor, kRed); // queued before this view's TAA passes
                    g_tinted = true;
                }
            }
            g_tintThisFrame = false;
        }
    }
    g_renderTaa(renderLoop, renderContext);
}

// Red-dominant share (%) of each back-buffer half, sampled every 4th pixel. Stalls once (staging read).
bool RedShares(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context, double& left,
    double& right)
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
    std::uint64_t red[2] = {0, 0}, total[2] = {0, 0};
    const UINT half = d.Width / 2;
    for (UINT y = 0; y < d.Height; y += 4) {
        const auto* row = static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch;
        for (UINT x = 0; x < d.Width; x += 4) {
            const std::uint8_t* p = row + x * 4;
            const int r = bgra ? p[2] : p[0], g = p[1], b = bgra ? p[0] : p[2];
            const int side = x < half ? 0 : 1;
            ++total[side];
            if (r > 128 && r * 2 > g * 5 && r * 2 > b * 5) {
                ++red[side];
            }
        }
    }
    context->Unmap(g_staging.Get(), 0);
    left = total[0] ? 100.0 * static_cast<double>(red[0]) / static_cast<double>(total[0]) : 0.0;
    right = total[1] ? 100.0 * static_cast<double>(red[1]) / static_cast<double>(total[1]) : 0.0;
    return true;
}

void FinishLeak(const std::string& result)
{
    TT2VR_LOG_INFO("leaktest: {}", result);
    for (const auto& line : g_leakLines) {
        TT2VR_LOG_INFO("leaktest:   {}", line);
    }
    g_leak = Leak::Idle;
    g_tintThisFrame = false;
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
    g_clearRt = reinterpret_cast<ClearRtFn>(SignatureAddress(Sig::RenderLayerClearRt));
    g_currentRt = reinterpret_cast<CurrentRtFn>(SignatureAddress(Sig::PostProcessCurrentRt));
    if (!taa || !g_clearRt || !g_currentRt ||
        core::hooks::Create("RenderLoop::RenderTAA", taa, &HookedRenderTaa, &g_renderTaa) != core::hooks::Result::Ok) {
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
    g_resetPending = g_resetNext;
    g_resetNext = false;
    g_tinted = false;
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
    g_leak = Leak::Baseline;
}

void OnTaaPresent(IDXGISwapChain* swapChain, ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (g_leak == Leak::Idle || !swapChain || !device || !context) {
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
        g_leak = Leak::Tint;
        break;
    case Leak::Tint:
        if (!g_tinted) {
            FinishLeak("FAILED: the tint was not queued (no eye-0 RenderTAA this frame; TAA off?)");
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
