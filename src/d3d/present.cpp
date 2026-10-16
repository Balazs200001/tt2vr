// present.cpp - IDXGISwapChain::Present / ResizeBuffers hooks: the per-frame tick of the mod.
//
// Per Present on the game's swap chain (thread: game main thread, docs/re/README.md):
//   before the real Present: end an active frame trace / texture dump, draw the overlay;
//   after it returns: count the frame, poll hotkeys and remote commands, start requested traces/dumps.
// Present calls on other swap chains (the engine can add windows) pass through untouched.
#include "d3d/present.hpp"

#include "core/commands.hpp"
#include "core/config.hpp"
#include "core/hooks.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "d3d/context_hooks.hpp"
#include "d3d/frame_trace.hpp"
#include "d3d/internal.hpp"
#include "d3d/renderdoc.hpp"
#include "d3d/shader_registry.hpp"
#include "d3d/texture_dump.hpp"
#include "d3d/vtable.hpp"
#include "game/camera.hpp"
#include "game/version.hpp"
#include "ui/overlay.hpp"

#include <spdlog/fmt/fmt.h>
#include <windows.h>

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>

namespace d3d {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn g_present = nullptr;
ResizeBuffersFn g_resizeBuffers = nullptr;

IDXGISwapChain* g_swapChain = nullptr; // the game's; not AddRef'd (lives as long as the game renders)
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_immediate = nullptr;
HWND g_window = nullptr;
std::filesystem::path g_commandsDir;

std::mutex g_statsMutex;
FrameStats g_stats;
std::uint64_t g_frame = 0; // present thread
std::chrono::steady_clock::time_point g_lastPresent;
std::chrono::steady_clock::time_point g_fpsWindowStart;
int g_fpsWindowFrames = 0;

std::deque<std::string> g_results; // present thread
bool g_keyDown[256] = {};

void AddResult(const std::string& origin, const std::string& result)
{
    g_results.push_back(fmt::format("[{}] {}", origin, result));
    while (g_results.size() > 6) {
        g_results.pop_front();
    }
}

// One remote request file: collects a result per command and writes <id>.done when all are in.
struct Request {
    std::filesystem::path path;
    std::vector<std::string> results;
    std::size_t remaining = 0;
};

auto Completion(const std::shared_ptr<Request>& req, std::size_t index)
{
    return [req, index](const std::string& result) {
        req->results[index] = result;
        if (--req->remaining == 0) {
            core::WriteDone(req->path, req->results);
        }
    };
}

void RunCommands()
{
    const auto& cfg = core::GetConfig().commands;
    if (!cfg.enabled || g_commandsDir.empty() || g_frame % static_cast<std::uint64_t>(cfg.poll_frames) != 0) {
        return;
    }
    for (auto& file : core::TakeCommandFiles(g_commandsDir)) {
        auto req = std::make_shared<Request>();
        req->path = file.path;
        req->results.resize(file.commands.size());
        req->remaining = file.commands.size();
        TT2VR_LOG_INFO("commands: {} ({} commands)", file.path.filename().string(), file.commands.size());
        if (file.commands.empty()) {
            core::WriteDone(file.path, {"no commands"});
            continue;
        }
        for (std::size_t i = 0; i < file.commands.size(); ++i) {
            const auto& cmd = file.commands[i];
            auto done = Completion(req, i);
            const std::string origin = "remote";
            switch (cmd.kind) {
            case core::CommandKind::FrameTrace:
                trace::Request([done, origin](const std::string& r) { AddResult(origin, r); done(r); });
                break;
            case core::CommandKind::TextureDump:
                dump::Request([done, origin](const std::string& r) { AddResult(origin, r); done(r); });
                break;
            case core::CommandKind::RenderDocCapture:
                renderdoc::Request(cmd.arg, [done, origin](const std::string& r) { AddResult(origin, r); done(r); });
                break;
            case core::CommandKind::ShaderRegistry: {
                const auto path = core::CaptureDir() / "shader_registry.txt";
                const std::string r = shaders::WriteRegistry(path)
                                          ? fmt::format("shaders ok {} ({} unique)", path.string(), shaders::Count())
                                          : "shaders FAILED: cannot write registry";
                AddResult(origin, r);
                done(r);
                break;
            }
            case core::CommandKind::Camera: {
                TT2VR_LOG_INFO("{}", game::DescribeCamera());
                const std::string r = game::CameraSummary();
                AddResult(origin, r);
                done(r);
                break;
            }
            case core::CommandKind::Unknown:
                done(fmt::format("unknown command: {}", cmd.text));
                break;
            }
        }
    }
}

bool KeyDown(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

void PollHotkeys()
{
    const bool focused = g_window && GetForegroundWindow() == g_window;
    const auto& hk = core::GetConfig().hotkeys;
    const core::Hotkey* keys[] = {&hk.overlay, &hk.frame_trace, &hk.texture_dump, &hk.renderdoc_capture};
    const bool ctrl = KeyDown(VK_CONTROL);
    const bool shift = KeyDown(VK_SHIFT);
    const bool alt = KeyDown(VK_MENU);
    bool fired[4] = {};
    bool down[4] = {};
    for (int i = 0; i < 4; ++i) {
        const unsigned vk = keys[i]->vk;
        if (!keys[i]->Valid() || vk >= 256) {
            continue;
        }
        down[i] = KeyDown(static_cast<int>(vk));
        fired[i] = focused && down[i] && !g_keyDown[vk] && keys[i]->Matches(vk, ctrl, shift, alt);
    }
    // Update after all edges are known: several hotkeys may share a key with different modifiers.
    for (int i = 0; i < 4; ++i) {
        if (keys[i]->Valid() && keys[i]->vk < 256) {
            g_keyDown[keys[i]->vk] = down[i];
        }
    }
    if (fired[0]) {
        ui::ToggleVisible();
    }
    if (fired[1]) {
        RequestFrameTrace("hotkey");
    }
    if (fired[2]) {
        RequestTextureDump("hotkey");
    }
    if (fired[3]) {
        RequestRenderDocCapture("hotkey", "tt2vr");
    }
}

// Engine hooks go in once the exe hash is known (CLAUDE.md rule 3/4: unknown build -> D3D layer only).
// Installing here, between frames on the main thread, keeps the hooked engine functions idle during install.
void InstallEngineHooksOnce()
{
    static bool s_done = false;
    if (s_done || game::ExeVersion().status == game::VersionStatus::NotChecked) {
        return;
    }
    s_done = true;
    if (game::EngineHooksAllowed()) {
        game::InstallCameraHooks();
    }
    else {
        TT2VR_LOG_WARN("engine hooks skipped: unknown game version");
    }
}

void UpdateStats()
{
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(g_statsMutex);
    g_stats.frame = g_frame;
    if (g_lastPresent.time_since_epoch().count() != 0) {
        ++g_fpsWindowFrames;
        const double window = std::chrono::duration<double>(now - g_fpsWindowStart).count();
        if (window >= 0.5) {
            g_stats.fps = g_fpsWindowFrames / window;
            g_stats.ms = 1000.0 * window / g_fpsWindowFrames;
            g_fpsWindowFrames = 0;
            g_fpsWindowStart = now;
        }
    }
    else {
        g_fpsWindowStart = now;
    }
    g_lastPresent = now;
}

// Thread: game main thread.
HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* self, UINT sync, UINT flags)
{
    const bool ours = self == g_swapChain && !(flags & DXGI_PRESENT_TEST);
    if (ours) {
        try {
            if (trace::Active()) {
                trace::Line(g_immediate, fmt::format("Present sync={} flags=0x{:x}", sync, flags));
            }
            trace::End(g_frame);
            dump::End(g_frame, self);
            if (!renderdoc::Busy()) {
                ui::Render(self, g_device, g_immediate);
            }
        }
        catch (...) {
            TT2VR_LOG_ERROR("exception in Present pre-hook (frame {})", g_frame);
        }
    }
    const HRESULT hr = g_present(self, sync, flags);
    if (ours) {
        try {
            ++g_frame;
            game::OnPresent();
            InstallEngineHooksOnce();
            UpdateStats();
            PollHotkeys();
            RunCommands();
            renderdoc::Tick(g_frame);
            trace::Begin(g_frame, g_immediate);
            dump::Begin(g_frame, g_device, g_immediate);
            if (core::GetVerbose() && g_frame % 600 == 0) {
                TT2VR_LOG_DEBUG("frame {}: {:.1f} fps", g_frame, Stats().fps);
            }
        }
        catch (...) {
            TT2VR_LOG_ERROR("exception in Present post-hook (frame {})", g_frame);
        }
    }
    return hr;
}

// Thread: game main thread.
HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* self, UINT count, UINT width, UINT height,
    DXGI_FORMAT format, UINT flags)
{
    if (self == g_swapChain) {
        ui::ReleaseTargets(); // all back-buffer references must be gone before ResizeBuffers
    }
    const HRESULT hr = g_resizeBuffers(self, count, width, height, format, flags);
    if (self == g_swapChain) {
        DXGI_SWAP_CHAIN_DESC desc{};
        self->GetDesc(&desc);
        {
            std::lock_guard lock(g_statsMutex);
            g_stats.width = desc.BufferDesc.Width;
            g_stats.height = desc.BufferDesc.Height;
        }
        TT2VR_LOG_INFO("ResizeBuffers {}x{} format {} -> hr=0x{:08x}, back buffer now {}x{}", width, height,
            static_cast<int>(format), static_cast<unsigned>(hr), desc.BufferDesc.Width, desc.BufferDesc.Height);
    }
    return hr;
}

} // namespace

void OnDeviceCreated(ID3D11Device* device, ID3D11DeviceContext* immediate, IDXGISwapChain* swapChain)
{
    g_device = device;
    g_immediate = immediate;
    g_swapChain = swapChain;
    g_commandsDir = core::CommandsDir();
    if (device) {
        shaders::InstallHooks(device);
    }
    InstallContextHooks(device, immediate);
    if (!swapChain) {
        TT2VR_LOG_ERROR("no swap chain from D3D11CreateDeviceAndSwapChain; overlay, traces and dumps disabled");
        return;
    }
    DXGI_SWAP_CHAIN_DESC desc{};
    swapChain->GetDesc(&desc);
    g_window = desc.OutputWindow;
    {
        std::lock_guard lock(g_statsMutex);
        g_stats.width = desc.BufferDesc.Width;
        g_stats.height = desc.BufferDesc.Height;
    }
    TT2VR_LOG_INFO("swap chain {}x{} format {} buffers {} windowed {} swap effect {}", desc.BufferDesc.Width,
        desc.BufferDesc.Height, static_cast<int>(desc.BufferDesc.Format), desc.BufferCount, desc.Windowed,
        static_cast<int>(desc.SwapEffect));
    void** vtbl = *reinterpret_cast<void***>(swapChain);
    const bool present = core::hooks::Create("IDXGISwapChain::Present", vtbl[vt::swapchain::Present], &HookedPresent,
                             &g_present) == core::hooks::Result::Ok;
    core::hooks::Create("IDXGISwapChain::ResizeBuffers", vtbl[vt::swapchain::ResizeBuffers], &HookedResizeBuffers,
        &g_resizeBuffers);
    std::lock_guard lock(g_statsMutex);
    g_stats.presentHooked = present;
}

FrameStats Stats()
{
    std::lock_guard lock(g_statsMutex);
    return g_stats;
}

void RequestFrameTrace(const std::string& origin)
{
    trace::Request([origin](const std::string& r) { AddResult(origin, r); });
}

void RequestTextureDump(const std::string& origin)
{
    dump::Request([origin](const std::string& r) { AddResult(origin, r); });
}

void RequestRenderDocCapture(const std::string& origin, const std::string& name)
{
    renderdoc::Request(name, [origin](const std::string& r) { AddResult(origin, r); });
}

void WriteShaderRegistry(const std::string& origin)
{
    const auto path = core::CaptureDir() / "shader_registry.txt";
    AddResult(origin, shaders::WriteRegistry(path) ? fmt::format("shaders ok {} ({} unique)", path.string(), shaders::Count())
                                                   : "shaders FAILED: cannot write registry");
}

std::vector<std::string> RecentResults()
{
    return {g_results.begin(), g_results.end()};
}

} // namespace d3d
