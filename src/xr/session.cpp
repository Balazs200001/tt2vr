#include "xr/session.hpp"

#include "core/config.hpp"
#include "core/log.hpp"
#include "core/test_mode.hpp"
#include "d3d/blit.hpp"
#include "d3d/internal.hpp"
#include "d3d/screenshot.hpp"
#include "xr/headset_profile.hpp"
#include "xr/openxr.hpp"
#include "xr/swapchain.hpp"

#include <spdlog/fmt/fmt.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace xr {

std::string ResultString(XrInstance instance, XrResult result)
{
    char text[XR_MAX_RESULT_STRING_SIZE] = {};
    if (instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance, result, text))) {
        return text;
    }
    return fmt::format("XrResult {}", static_cast<int>(result));
}

namespace {

using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;

constexpr auto kInstanceRetry = std::chrono::seconds(10);
constexpr auto kSystemRetry = std::chrono::seconds(2);
constexpr auto kSessionLostRetry = std::chrono::seconds(3);

// Test mode: stand-in for an eye swapchain image (typeless R8G8B8A8, raw view, as SteamVR's), so the simulated
// headset runs the same eye copy (GPU profiles include it) and "eyeshot" can save what the headset would get.
struct SimEye {
    bool Create(ID3D11Device* device, UINT w, UINT h)
    {
        rtv.Reset();
        texture.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &texture)) ||
            FAILED(device->CreateRenderTargetView(texture.Get(), &rd, &rtv))) {
            rtv.Reset();
            texture.Reset();
            return false;
        }
        width = w;
        height = h;
        return true;
    }
    std::uint32_t Width() const { return width; }
    std::uint32_t Height() const { return height; }
    DXGI_FORMAT Format() const { return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; }
    DXGI_FORMAT TextureFormat() const { return DXGI_FORMAT_R8G8B8A8_TYPELESS; }
    ID3D11Texture2D* Texture() const { return texture.Get(); }
    ID3D11RenderTargetView* Rtv() const { return rtv.Get(); }
    bool Decode() const { return false; }

    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    std::uint32_t width = 0, height = 0;
};

struct EyeShot {
    std::string name;
    std::function<void(const std::string&)> done;
};

// All state lives on the game main thread (every entry point is called from it).
struct State {
    ID3D11Device* device = nullptr; // the game's; not owned
    bool configured = false; // Init ran
    bool stopped = false; // permanent flat mode (missing extension, adapter mismatch) or the runtime asked us to exit
    Clock::time_point nextAttempt{};

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE;
    XrSpace view = XR_NULL_HANDLE; // the head ([hud] follow = "head")
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    XrEnvironmentBlendMode blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    bool running = false;
    EyeSwapchain eyes[2];
    EyeSwapchain screen; // menu screen quad (created on first use, sized like the back buffer)
    EyeSwapchain hud; // HUD panel quad (created on first use, sized like the HUD capture targets)
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    d3d::Blitter blitter;

    // Current frame (xrBeginFrame .. xrEndFrame).
    bool frameOpen = false;
    XrTime displayTime = 0;
    bool shouldRender = false;
    bool located = false;
    bool rendered = false;
    bool sideBySide = false; // the back buffer holds view 0 | view 1 (game::SplitView, Phase 4)
    int viewEye[2] = {0, 1}; // eye whose located pose and FOV each rendered view used
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    FrameViews frameViews{};

    // Recenter (XR LOCAL space, yaw-only head reference).
    stereo::Mat34 recenter = stereo::Identity34();
    bool recenterPending = true;
    XrTime recenterAfter = 0;
    stereo::Vec3 lastPosition[2] = {};
    unsigned recenterCount = 0;

    Status status;
    bool syncWarned = false;

    // Simulated headset (test mode, core::TestMode): no OpenXR; views from the headset profile and a scripted head.
    bool simulated = false;
    HeadsetProfile profile;
    bool profileSaved = false; // real session: profile written for this process
    float simYaw = 0, simPitch = 0, simRoll = 0; // degrees; yaw left +, pitch up +
    float shakeAmp = 0, shakePeriod = 1, shakeSeconds = 0; // yaw oscillation (degrees, s, s; 0 s = off)
    Clock::time_point shakeStart{};
    SimEye simEyes[2];
    bool simEyesFailed = false;

    // [xr] render_scale as the render size and the eye copy use it (UpdateRenderScale).
    bool renderScaleInit = false;
    float renderScale = 1.0f; // applied
    float renderScaleSeen = 1.0f; // last config value
    Clock::time_point renderScaleChanged{};
    std::vector<EyeShot> eyeShots; // pending "eyeshot" requests (simulated headset)
};

State g;

// Logs a failed call; the same call failing with the same result again is only counted.
bool Ok(XrResult r, const char* what)
{
    if (XR_SUCCEEDED(r)) {
        return true;
    }
    static std::string s_last;
    static int s_repeats = 0;
    const std::string line = fmt::format("{} failed: {}", what, ResultString(g.instance, r));
    if (line == s_last) {
        if (++s_repeats % 600 == 0) {
            TT2VR_LOG_ERROR("xr: {} (x{})", line, s_repeats);
        }
    }
    else {
        s_last = line;
        s_repeats = 0;
        TT2VR_LOG_ERROR("xr: {}", line);
    }
    g.status.error = line;
    return false;
}

const char* StateName(XrSessionState s)
{
    switch (s) {
    case XR_SESSION_STATE_IDLE: return "idle";
    case XR_SESSION_STATE_READY: return "ready";
    case XR_SESSION_STATE_SYNCHRONIZED: return "synchronized";
    case XR_SESSION_STATE_VISIBLE: return "visible";
    case XR_SESSION_STATE_FOCUSED: return "focused";
    case XR_SESSION_STATE_STOPPING: return "stopping";
    case XR_SESSION_STATE_LOSS_PENDING: return "loss pending";
    case XR_SESSION_STATE_EXITING: return "exiting";
    default: return "unknown";
    }
}

void DestroySession()
{
    if (g.frameOpen && g.session != XR_NULL_HANDLE) {
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = g.displayTime;
        end.environmentBlendMode = g.blendMode;
        xrEndFrame(g.session, &end);
    }
    g.frameOpen = g.located = g.rendered = false;
    for (auto& eye : g.eyes) {
        eye.Destroy();
    }
    g.screen.Destroy();
    g.hud.Destroy();
    if (g.view != XR_NULL_HANDLE) {
        xrDestroySpace(g.view);
        g.view = XR_NULL_HANDLE;
    }
    if (g.local != XR_NULL_HANDLE) {
        xrDestroySpace(g.local);
        g.local = XR_NULL_HANDLE;
    }
    if (g.session != XR_NULL_HANDLE) {
        if (g.running) {
            xrEndSession(g.session);
        }
        xrDestroySession(g.session);
        g.session = XR_NULL_HANDLE;
        TT2VR_LOG_INFO("xr: session destroyed");
    }
    g.running = false;
    g.sessionState = XR_SESSION_STATE_UNKNOWN;
}

void DestroyAll()
{
    DestroySession();
    g.system = XR_NULL_SYSTEM_ID;
    if (g.instance != XR_NULL_HANDLE) {
        xrDestroyInstance(g.instance);
        g.instance = XR_NULL_HANDLE;
        TT2VR_LOG_INFO("xr: instance destroyed");
    }
}

void StopFlat(const std::string& why)
{
    TT2VR_LOG_ERROR("xr: {}; staying flat", why);
    g.status.error = why;
    g.stopped = true;
    DestroyAll();
}

bool CreateInstance()
{
    std::uint32_t count = 0;
    XrResult r = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    std::vector<XrExtensionProperties> exts(count, {XR_TYPE_EXTENSION_PROPERTIES});
    if (XR_SUCCEEDED(r) && count > 0) {
        r = xrEnumerateInstanceExtensionProperties(nullptr, count, &count, exts.data());
    }
    if (XR_FAILED(r)) {
        // Usually no active OpenXR runtime (Meta Link / SteamVR not set as the OpenXR runtime).
        Ok(r, "xrEnumerateInstanceExtensionProperties (no OpenXR runtime?)");
        return false;
    }
    const bool hasD3D11 = std::any_of(exts.begin(), exts.end(),
        [](const XrExtensionProperties& e) { return std::strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0; });
    if (!hasD3D11) {
        StopFlat("the OpenXR runtime has no " XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        return false;
    }
    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(info.applicationInfo.applicationName, "tt2vr");
    info.applicationInfo.applicationVersion = 1;
    strcpy_s(info.applicationInfo.engineName, "KT Engine");
    info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = enabled;
    if (!Ok(xrCreateInstance(&info, &g.instance), "xrCreateInstance")) {
        g.instance = XR_NULL_HANDLE;
        return false;
    }
    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(g.instance, &props))) {
        g.status.runtime = fmt::format("{} {}.{}.{}", props.runtimeName, XR_VERSION_MAJOR(props.runtimeVersion),
            XR_VERSION_MINOR(props.runtimeVersion), XR_VERSION_PATCH(props.runtimeVersion));
    }
    TT2VR_LOG_INFO("xr: instance created, runtime {}", g.status.runtime);
    // PLAN Phase 9 (depth submission, refresh rate, space warp): what this runtime offers.
    std::string names;
    for (const XrExtensionProperties& e : exts) {
        names += names.empty() ? "" : " ";
        names += e.extensionName;
    }
    TT2VR_LOG_INFO("xr: runtime extensions ({}): {}", exts.size(), names);
    g.status.state = "looking for headset";
    g.nextAttempt = Clock::time_point{}; // look for the headset right away
    return true;
}

bool AdapterMatches(const LUID& required)
{
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(g.device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&desc))) {
        TT2VR_LOG_WARN("xr: cannot read the game device's adapter LUID; assuming it matches");
        return true;
    }
    const bool same = desc.AdapterLuid.LowPart == required.LowPart && desc.AdapterLuid.HighPart == required.HighPart;
    char name[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
    TT2VR_LOG_INFO("xr: game adapter \"{}\" LUID {:08x}:{:08x}, runtime wants {:08x}:{:08x} -> {}", name,
        static_cast<unsigned>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart,
        static_cast<unsigned>(required.HighPart), required.LowPart, same ? "match" : "MISMATCH");
    return same;
}

// xrGetSystem, graphics requirements, view configuration, session, LOCAL space, swapchains.
bool CreateSession()
{
    XrSystemGetInfo sysInfo{XR_TYPE_SYSTEM_GET_INFO};
    sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    const XrResult sr = xrGetSystem(g.instance, &sysInfo, &g.system);
    if (sr == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        g.system = XR_NULL_SYSTEM_ID;
        g.status.state = "waiting for headset";
        return false; // headset not connected yet; retried quietly
    }
    if (!Ok(sr, "xrGetSystem")) {
        g.system = XR_NULL_SYSTEM_ID;
        return false;
    }
    XrSystemProperties sysProps{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(g.instance, g.system, &sysProps))) {
        g.status.system = sysProps.systemName;
    }
    TT2VR_LOG_INFO("xr: system \"{}\"", g.status.system);

    // Required before xrCreateSession (XR_KHR_D3D11_enable).
    PFN_xrGetD3D11GraphicsRequirementsKHR getRequirements = nullptr;
    if (!Ok(xrGetInstanceProcAddr(g.instance, "xrGetD3D11GraphicsRequirementsKHR",
                reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)),
            "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)")) {
        return false;
    }
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!Ok(getRequirements(g.instance, g.system, &req), "xrGetD3D11GraphicsRequirementsKHR")) {
        return false;
    }
    if (!AdapterMatches(req.adapterLuid)) {
        StopFlat("the headset runs on a different GPU than the game (adapter LUID mismatch)");
        return false;
    }
    if (g.device->GetFeatureLevel() < req.minFeatureLevel) {
        StopFlat(fmt::format("game device feature level 0x{:x} is below the runtime's minimum 0x{:x}",
            static_cast<unsigned>(g.device->GetFeatureLevel()), static_cast<unsigned>(req.minFeatureLevel)));
        return false;
    }

    std::uint32_t viewCount = 0;
    XrViewConfigurationView configViews[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    if (!Ok(xrEnumerateViewConfigurationViews(g.instance, g.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                &viewCount, configViews),
            "xrEnumerateViewConfigurationViews") ||
        viewCount != 2) {
        StopFlat("the runtime has no two-view stereo configuration");
        return false;
    }
    std::uint32_t blendCount = 0;
    XrEnvironmentBlendMode blendModes[8] = {};
    if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(g.instance, g.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            8, &blendCount, blendModes)) &&
        blendCount > 0) {
        // A game view must be opaque. The runtime lists its modes in its own preference order, which need not start
        // with OPAQUE (2026-10-20: the SteamVR background showed through the riding view at ~20%).
        g.blendMode = blendModes[0];
        std::string modes;
        for (std::uint32_t i = 0; i < blendCount && i < 8; ++i) {
            modes += fmt::format("{}{}", i ? ", " : "", static_cast<int>(blendModes[i]));
            if (blendModes[i] == XR_ENVIRONMENT_BLEND_MODE_OPAQUE) {
                g.blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            }
        }
        TT2VR_LOG_INFO("xr: environment blend modes offered [{}] (1 opaque, 2 additive, 3 alpha blend); using {}",
            modes, static_cast<int>(g.blendMode));
    }

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = g.device;
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next = &binding;
    info.systemId = g.system;
    if (!Ok(xrCreateSession(g.instance, &info, &g.session), "xrCreateSession")) {
        g.session = XR_NULL_HANDLE;
        return false;
    }
    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    if (!Ok(xrCreateReferenceSpace(g.session, &spaceInfo, &g.local), "xrCreateReferenceSpace(LOCAL)")) {
        g.local = XR_NULL_HANDLE;
        DestroySession();
        return false;
    }
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!Ok(xrCreateReferenceSpace(g.session, &spaceInfo, &g.view), "xrCreateReferenceSpace(VIEW)")) {
        g.view = XR_NULL_HANDLE; // a head-following HUD panel falls back to the seat
    }

    std::uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(g.session, 0, &formatCount, nullptr);
    std::vector<std::int64_t> formats(formatCount);
    if (formatCount > 0) {
        xrEnumerateSwapchainFormats(g.session, formatCount, &formatCount, formats.data());
    }
    const DXGI_FORMAT format = ChooseSwapchainFormat(formats);
    const float scale = core::GetConfig().xr.resolution_scale;
    g.status.recommendedW = configViews[0].recommendedImageRectWidth;
    g.status.recommendedH = configViews[0].recommendedImageRectHeight;
    const auto scaled = [scale](std::uint32_t recommended, std::uint32_t max) {
        const auto v = static_cast<std::uint32_t>(std::lround(static_cast<double>(recommended) * scale));
        return std::clamp<std::uint32_t>(v, 16u, std::max(16u, max));
    };
    // Even sizes: the engine rounds its render size down to even, and a mismatch forces a blit instead of a copy.
    const std::uint32_t w = scaled(configViews[0].recommendedImageRectWidth, configViews[0].maxImageRectWidth) & ~1u;
    const std::uint32_t h = scaled(configViews[0].recommendedImageRectHeight, configViews[0].maxImageRectHeight) & ~1u;
    for (int eye = 0; eye < 2; ++eye) {
        if (!g.eyes[eye].Create(g.instance, g.session, g.device, format, w, h, eye == 0 ? "left eye" : "right eye")) {
            DestroySession();
            g.status.error = "swapchain creation failed (see log)";
            return false;
        }
    }
    g.status.eyeW = w;
    g.status.eyeH = h;
    g.status.format = static_cast<int>(format);
    g.format = format;
    TT2VR_LOG_INFO("xr: session created; recommended eye size {}x{} (max {}x{}), scale {} -> swapchains {}x{} format {}",
        configViews[0].recommendedImageRectWidth, configViews[0].recommendedImageRectHeight,
        configViews[0].maxImageRectWidth, configViews[0].maxImageRectHeight, scale, w, h, static_cast<int>(format));
    g.status.state = "session created";
    return true;
}

void OnSessionState(XrSessionState state)
{
    g.sessionState = state;
    g.status.state = StateName(state);
    TT2VR_LOG_INFO("xr: session state {}", StateName(state));
    switch (state) {
    case XR_SESSION_STATE_READY: {
        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (Ok(xrBeginSession(g.session, &begin), "xrBeginSession")) {
            g.running = true;
            g.recenterPending = true; // the first tracked pose becomes the rider's eye point
            g.recenterAfter = 0;
        }
        break;
    }
    case XR_SESSION_STATE_STOPPING:
        if (g.frameOpen) {
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
            end.displayTime = g.displayTime;
            end.environmentBlendMode = g.blendMode;
            xrEndFrame(g.session, &end);
            g.frameOpen = false;
        }
        Ok(xrEndSession(g.session), "xrEndSession");
        g.running = false;
        break;
    case XR_SESSION_STATE_EXITING:
        // The runtime or the user closed us (e.g. from the headset menu): stay flat until "Restart XR".
        DestroySession();
        g.stopped = true;
        g.status.state = "exited (overlay: Restart XR)";
        break;
    case XR_SESSION_STATE_LOSS_PENDING:
        DestroySession();
        g.system = XR_NULL_SYSTEM_ID; // headset disconnected: look for it again
        g.nextAttempt = Clock::now() + kSessionLostRetry;
        g.status.state = "session lost, retrying";
        break;
    default:
        break;
    }
}

void PumpEvents()
{
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (g.instance != XR_NULL_HANDLE && xrPollEvent(g.instance, &event) == XR_SUCCESS) {
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto& e = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            if (e.session == g.session && g.session != XR_NULL_HANDLE) {
                OnSessionState(e.state);
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            TT2VR_LOG_WARN("xr: instance loss pending (runtime restarting?)");
            DestroyAll();
            g.nextAttempt = Clock::now() + kInstanceRetry;
            g.status.state = "runtime lost, retrying";
            return; // the instance is gone
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
            const auto& e = *reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&event);
            if (e.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                // The runtime recentered LOCAL (e.g. holding the Oculus button): take the new head pose again.
                TT2VR_LOG_INFO("xr: runtime recenter of LOCAL space; recentering at the change time");
                g.recenterPending = true;
                g.recenterAfter = e.changeTime;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_EVENTS_LOST:
            TT2VR_LOG_WARN("xr: runtime event queue overflowed");
            break;
        default:
            break;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// Ends the open frame with `count` layers, back to front (0 = none).
void EndFrame(const XrCompositionLayerBaseHeader* const* layers, std::uint32_t count)
{
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = g.displayTime;
    end.environmentBlendMode = g.blendMode;
    end.layerCount = count;
    end.layers = count ? layers : nullptr;
    if (Ok(xrEndFrame(g.session, &end), "xrEndFrame") && count) {
        ++g.status.framesSubmitted;
    }
    g.frameOpen = g.located = g.rendered = false;
}

// Ends the open frame. With `layer`, submits that one layer.
void EndFrame(const XrCompositionLayerBaseHeader* layer)
{
    EndFrame(&layer, layer ? 1 : 0);
}

// Back buffer -> the target's current image: raw copy when size and family match, else a stretch blit (or FSR1 for
// an eye image rendered at [xr] render_scale < 1). Returns "copy", "blit", "fsr" or nullptr on failure. The caller
// has acquired the image. Columns [srcX, srcX + srcW) of the back buffer -> the whole target. copySource = false
// reuses the blitter's source copy from the previous call this frame (the second eye half). Target: EyeSwapchain or
// SimEye.
template <typename Target>
const char* CopyBackBuffer(const Target& target, ID3D11Texture2D* back, const D3D11_TEXTURE2D_DESC& bd,
    ID3D11DeviceContext* context, UINT srcX, UINT srcW, bool copySource = true, bool eye = false)
{
    const bool copy = srcW == target.Width() && bd.Height == target.Height() && bd.SampleDesc.Count == 1 &&
                      d3d::IsSrgb(target.Format()) && d3d::SameFamily(bd.Format, target.TextureFormat());
    if (!g.blitter.Ready() && !g.blitter.Init(g.device)) {
        return nullptr;
    }
    if (copy) {
        // 2026-10-16: with the raw copy SteamVR showed its background through the riding view (~20%): the game's
        // back-buffer alpha came along. Eye and screen images are opaque.
        const D3D11_BOX box{srcX, 0, 0, srcX + srcW, bd.Height, 1};
        context->CopySubresourceRegion(target.Texture(), 0, 0, 0, 0, back, 0, &box);
        g.blitter.FillAlpha(context, target.Rtv(), target.Width(), target.Height());
        return "copy";
    }
    const auto& xc = core::GetConfig().xr;
    if (eye && g.renderScale < 1.0f && srcW < target.Width() && bd.Height < target.Height() &&
        g.blitter.Fsr(context, back, srcX, srcW, target.Rtv(), target.Width(), target.Height(), target.Decode(),
            xc.fsr_sharpness, copySource)) {
        return "fsr";
    }
    const float u0 = static_cast<float>(srcX) / static_cast<float>(bd.Width);
    const float uw = static_cast<float>(srcW) / static_cast<float>(bd.Width);
    return g.blitter.Blit(context, back, target.Rtv(), target.Width(), target.Height(), target.Decode(), u0, uw,
               copySource)
               ? "blit"
               : nullptr;
}

ComPtr<ID3D11Texture2D> BackBuffer(IDXGISwapChain* swapChain, D3D11_TEXTURE2D_DESC& desc)
{
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        return nullptr;
    }
    back->GetDesc(&desc);
    g.status.backBufferW = desc.Width;
    g.status.backBufferH = desc.Height;
    return back;
}

// Alpha probe ("xrprobe"): min / mean / max alpha of a 64x64 patch at (x, y), via a staging copy (stalls once).
std::string ProbeAlpha(ID3D11DeviceContext* context, ID3D11Texture2D* texture, UINT x, UINT y, const char* name)
{
    D3D11_TEXTURE2D_DESC d{};
    texture->GetDesc(&d);
    const DXGI_FORMAT typeless = d3d::TypelessOf(d.Format);
    if (typeless != DXGI_FORMAT_R8G8B8A8_TYPELESS && typeless != DXGI_FORMAT_B8G8R8A8_TYPELESS) {
        return fmt::format("{}: format {} not probed", name, static_cast<int>(d.Format));
    }
    x = std::min(x, d.Width > 64 ? d.Width - 64 : 0u);
    y = std::min(y, d.Height > 64 ? d.Height - 64 : 0u);
    const UINT w = std::min(64u, d.Width), h = std::min(64u, d.Height);
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = w;
    sd.Height = h;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = d.Format;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(g.device->CreateTexture2D(&sd, nullptr, &staging))) {
        return fmt::format("{}: staging texture failed", name);
    }
    const D3D11_BOX box{x, y, 0, x + w, y + h, 1};
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        return fmt::format("{}: map failed", name);
    }
    int lo = 255, hi = 0;
    double sum = 0;
    for (UINT row = 0; row < h; ++row) {
        const auto* p = static_cast<const std::uint8_t*>(m.pData) + row * m.RowPitch;
        for (UINT col = 0; col < w; ++col) {
            const int a = p[col * 4 + 3];
            lo = std::min(lo, a);
            hi = std::max(hi, a);
            sum += a;
        }
    }
    context->Unmap(staging.Get(), 0);
    return fmt::format("{} {}x{} fmt {}: alpha min {} mean {:.1f} max {}", name, d.Width, d.Height,
        static_cast<int>(d.Format), lo, sum / (w * h), hi);
}

std::function<void(const std::string&)> g_probe; // pending "xrprobe"

void FinishProbe(std::string text)
{
    if (!g_probe) {
        return;
    }
    text += fmt::format("; swapchain format {}, layer flags 0, blend mode {}, presentation {}, copy {}",
        static_cast<int>(g.format), static_cast<int>(g.blendMode), g.status.presentation, g.status.copyMode);
    TT2VR_LOG_INFO("xrprobe: {}", text);
    auto done = std::move(g_probe);
    g_probe = nullptr;
    done("xrprobe ok " + text);
}

// Side by side (two views): eye k <- back-buffer half k. Otherwise eye 0 <- back buffer, eye 1 <- black (left eye
// only, Phase 3 mode).
bool WriteEyes(IDXGISwapChain* swapChain, ID3D11DeviceContext* context)
{
    D3D11_TEXTURE2D_DESC bd{};
    const ComPtr<ID3D11Texture2D> back = BackBuffer(swapChain, bd);
    if (!back) {
        return false;
    }
    d3d::InternalScope internal; // keep our copies out of frame traces and dumps
    const UINT halfW = bd.Width / 2;
    EyeSwapchain& left = g.eyes[0];
    if (!left.Acquire(g.instance)) {
        return false;
    }
    const char* mode = CopyBackBuffer(left, back.Get(), bd, context, 0, g.sideBySide ? halfW : bd.Width, true, true);
    left.Release(g.instance);
    g.status.copyMode = mode ? mode : "-";

    EyeSwapchain& right = g.eyes[1];
    if (!right.Acquire(g.instance)) {
        return false;
    }
    const char* rightMode = "black";
    if (g.sideBySide) {
        rightMode = CopyBackBuffer(right, back.Get(), bd, context, halfW, halfW, false, true);
    }
    else {
        constexpr float kBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        context->ClearRenderTargetView(right.Rtv(), kBlack);
    }
    if (g_probe) {
        // The left image is released already; probe the back buffer and the right image (same copy path).
        FinishProbe(ProbeAlpha(context, back.Get(), bd.Width / 4, bd.Height / 2, "back buffer") + "; " +
                    ProbeAlpha(context, right.Texture(), right.Width() / 2, right.Height() / 2, "right eye image"));
    }
    right.Release(g.instance);
    return mode != nullptr && rightMode != nullptr;
}

// Test mode: WriteEyes into the SimEye stand-ins, then the pending "eyeshot" requests (left eye).
void WriteSimulatedEyes(IDXGISwapChain* swapChain, ID3D11DeviceContext* context)
{
    D3D11_TEXTURE2D_DESC bd{};
    const ComPtr<ID3D11Texture2D> back = BackBuffer(swapChain, bd);
    if (!back) {
        return;
    }
    d3d::InternalScope internal;
    for (SimEye& e : g.simEyes) {
        if ((e.Width() != g.status.eyeW || e.Height() != g.status.eyeH) &&
            !e.Create(g.device, g.status.eyeW, g.status.eyeH)) {
            TT2VR_LOG_ERROR("xr: cannot create the {}x{} simulated eye images", g.status.eyeW, g.status.eyeH);
            g.simEyesFailed = true; // no retry every frame
            return;
        }
    }
    const UINT halfW = bd.Width / 2;
    const char* mode = CopyBackBuffer(g.simEyes[0], back.Get(), bd, context, 0, halfW, true, true);
    CopyBackBuffer(g.simEyes[1], back.Get(), bd, context, halfW, halfW, false, true);
    g.status.copyMode = mode ? mode : "-";
    auto shots = std::move(g.eyeShots);
    g.eyeShots.clear();
    for (auto& s : shots) {
        const std::string r = fmt::format("eyeshot {}; {}; copy {}",
            d3d::screenshot::SaveTexture(g.simEyes[0].Texture(), g.device, context, s.name + "_l", 0),
            d3d::screenshot::SaveTexture(g.simEyes[1].Texture(), g.device, context, s.name + "_r", 0),
            g.status.copyMode);
        TT2VR_LOG_INFO("{}", r);
        if (s.done) {
            s.done(r);
        }
    }
}

// Menu screen (Phase 3 interim): the whole flat frame on a quad fixed in LOCAL space in front of the recentered head.
// Returns false if nothing could be written (the frame then ends without layers).
bool WriteScreen(IDXGISwapChain* swapChain, ID3D11DeviceContext* context, XrCompositionLayerQuad& quad)
{
    D3D11_TEXTURE2D_DESC bd{};
    const ComPtr<ID3D11Texture2D> back = BackBuffer(swapChain, bd);
    if (!back || bd.Width == 0 || bd.Height == 0) {
        return false;
    }
    const UINT w = g.sideBySide ? bd.Width / 2 : bd.Width; // two-view frame: show view 0 only
    if (!g.screen.Valid() || g.screen.Width() != w || g.screen.Height() != bd.Height) {
        if (!g.screen.Create(g.instance, g.session, g.device, g.format, w, bd.Height, "screen")) {
            return false;
        }
    }
    d3d::InternalScope internal;
    if (!g.screen.Acquire(g.instance)) {
        return false;
    }
    const char* mode = CopyBackBuffer(g.screen, back.Get(), bd, context, 0, w);
    if (g_probe && mode) {
        FinishProbe(ProbeAlpha(context, back.Get(), bd.Width / 4, bd.Height / 2, "back buffer") + "; " +
                    ProbeAlpha(context, g.screen.Texture(), g.screen.Width() / 2, g.screen.Height() / 2,
                        "screen image"));
    }
    g.screen.Release(g.instance);
    if (!mode) {
        return false;
    }
    g.status.copyMode = mode;
    const auto& xc = core::GetConfig().xr;
    const stereo::Pose pose = stereo::ScreenPose(g.recenter, xc.screen_distance);
    quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.space = g.local;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = g.screen.Handle();
    quad.subImage.imageRect.extent = {static_cast<std::int32_t>(w), static_cast<std::int32_t>(bd.Height)};
    quad.pose.orientation = {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
    quad.pose.position = {pose.position.x, pose.position.y, pose.position.z};
    quad.size = {xc.screen_width, xc.screen_width * static_cast<float>(bd.Height) / static_cast<float>(w)};
    return true;
}

// A render scale change (VR Options tab list, remote vreffect) applies once it has held for 0.75 s, so stepping
// through the list resizes the game's render targets once instead of at every step.
void UpdateRenderScale()
{
    const float wanted = core::GetConfig().xr.render_scale;
    const auto now = Clock::now();
    if (!g.renderScaleInit) {
        g.renderScaleInit = true;
        g.renderScale = g.renderScaleSeen = wanted;
        return;
    }
    if (wanted != g.renderScaleSeen) {
        g.renderScaleSeen = wanted;
        g.renderScaleChanged = now;
    }
    else if (wanted != g.renderScale && now - g.renderScaleChanged > std::chrono::milliseconds(750)) {
        TT2VR_LOG_INFO("xr: render scale {:.2f} -> {:.2f}", g.renderScale, wanted);
        g.renderScale = wanted;
    }
}

// Test mode: a headset that needs no runtime. Eye size = profile recommendation * [xr] resolution_scale, as for a
// real session; the session counts as running from the start.
void StartSimulated()
{
    g.simulated = true;
    g.profile = LoadHeadsetProfile();
    const float scale = core::GetConfig().xr.resolution_scale;
    const auto scaled = [scale](std::uint32_t recommended) {
        const auto v = static_cast<std::uint32_t>(std::lround(static_cast<double>(recommended) * scale));
        return std::max(16u, v) & ~1u;
    };
    g.status.recommendedW = g.profile.recommendedW;
    g.status.recommendedH = g.profile.recommendedH;
    g.status.eyeW = scaled(g.profile.recommendedW);
    g.status.eyeH = scaled(g.profile.recommendedH);
    g.status.runtime = "tt2vr simulated headset";
    g.status.system = g.profile.system;
    g.status.state = "simulated (test mode)";
    g.status.tracked = true;
    g.running = true;
    TT2VR_LOG_INFO("xr: TEST MODE (-tt2vr-test): simulated headset, profile '{}', eye {}x{} (scale {}), ipd {:.4f} m; "
                   "OpenXR is not started",
        g.profile.system, g.status.eyeW, g.status.eyeH, scale, g.profile.ipd);
}

} // namespace

void Init(ID3D11Device* device)
{
    g.device = device;
    g.configured = true;
    g.status.enabled = core::GetConfig().xr.enabled;
    g.status.state = g.status.enabled ? "starting" : "disabled ([xr] enabled = false)";
    if (!g.status.enabled) {
        TT2VR_LOG_INFO("xr: disabled in config; the game stays flat");
    }
    else if (core::TestMode()) {
        StartSimulated();
    }
}

void OnFrameStart()
{
    UpdateRenderScale();
    g.sideBySide = false; // set again by this frame's DoUpdate (a movie frame has none)
    if (!g.configured || !g.status.enabled || !g.device) {
        return;
    }
    if (g.simulated) {
        // No runtime to wait for: the game's own pacing. Display time = steady clock (only compared with itself).
        g.frameOpen = true;
        g.displayTime = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        g.shouldRender = true;
        g.status.shouldRender = true;
        g.located = g.rendered = false;
        g.viewEye[0] = 0;
        g.viewEye[1] = 1;
        return;
    }
    try {
        if (g.frameOpen) {
            EndFrame(nullptr); // never leave a frame open across Presents
        }
        const auto now = Clock::now();
        if (!g.stopped && g.instance == XR_NULL_HANDLE && now >= g.nextAttempt) {
            g.nextAttempt = now + kInstanceRetry;
            g.status.state = "no OpenXR runtime";
            CreateInstance();
        }
        if (!g.stopped && g.instance != XR_NULL_HANDLE && g.session == XR_NULL_HANDLE && now >= g.nextAttempt) {
            g.nextAttempt = now + kSystemRetry;
            CreateSession();
        }
        PumpEvents();
        if (!g.running || g.session == XR_NULL_HANDLE) {
            return;
        }
        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frame{XR_TYPE_FRAME_STATE};
        const auto t0 = Clock::now();
        if (!Ok(xrWaitFrame(g.session, &waitInfo, &frame), "xrWaitFrame")) {
            return;
        }
        const double waited = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        g.status.waitMs = g.status.waitMs * 0.95 + waited * 0.05;
        g.status.lastWaitMs = waited;
        g.status.periodMs = static_cast<double>(frame.predictedDisplayPeriod) / 1e6;
        XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        const XrResult br = xrBeginFrame(g.session, &beginInfo);
        if (br != XR_SUCCESS && br != XR_FRAME_DISCARDED) {
            Ok(br, "xrBeginFrame");
            return;
        }
        g.frameOpen = true;
        g.displayTime = frame.predictedDisplayTime;
        g.shouldRender = frame.shouldRender == XR_TRUE;
        g.status.shouldRender = g.shouldRender;
        g.located = g.rendered = false;
        g.viewEye[0] = 0;
        g.viewEye[1] = 1;
    }
    catch (...) {
        TT2VR_LOG_ERROR("xr: exception in OnFrameStart");
    }
}

namespace {

XrQuaternionf QuatMul(const XrQuaternionf& a, const XrQuaternionf& b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

XrQuaternionf AxisAngle(float x, float y, float z, float degrees)
{
    const float h = degrees * 0.5f * 0.01745329f;
    const float s = std::sin(h);
    return {x * s, y * s, z * s, std::cos(h)};
}

XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v)
{
    const XrQuaternionf p{v.x, v.y, v.z, 0.0f};
    const XrQuaternionf r = QuatMul(QuatMul(q, p), {-q.x, -q.y, -q.z, q.w});
    return {r.x, r.y, r.z};
}

// Simulated head at the LOCAL origin (XR axes: +y up, -z forward): yaw about +y, then pitch about +x, then roll about
// -z; eyes at -/+ ipd/2 on the head's x axis; FOV from the profile.
void SimulatedViews(XrView (&views)[2])
{
    float yaw = g.simYaw;
    if (g.shakeSeconds > 0.0f) {
        const float t = std::chrono::duration<float>(Clock::now() - g.shakeStart).count();
        if (t < g.shakeSeconds) {
            yaw += g.shakeAmp * std::sin(6.2831853f * t / g.shakePeriod);
        }
        else {
            g.shakeSeconds = 0.0f;
        }
    }
    const XrQuaternionf q =
        QuatMul(QuatMul(AxisAngle(0, 1, 0, yaw), AxisAngle(1, 0, 0, g.simPitch)), AxisAngle(0, 0, -1, g.simRoll));
    for (int i = 0; i < 2; ++i) {
        views[i].pose.orientation = q;
        views[i].pose.position = Rotate(q, {(i == 0 ? -0.5f : 0.5f) * g.profile.ipd, 0.0f, 0.0f});
        views[i].fov = {g.profile.fov[i][0], g.profile.fov[i][1], g.profile.fov[i][2], g.profile.fov[i][3]};
    }
}

// Real session: the located FOV, IPD and recommended size, for the simulated headset of later test runs.
void SaveProfileFrom(const XrView (&views)[2])
{
    HeadsetProfile p;
    p.system = g.status.system.empty() ? "unknown" : g.status.system;
    p.recommendedW = g.status.recommendedW;
    p.recommendedH = g.status.recommendedH;
    const XrVector3f& a = views[0].pose.position;
    const XrVector3f& b = views[1].pose.position;
    const float ipd = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
    if (ipd > 0.04f && ipd < 0.09f) {
        p.ipd = ipd;
    }
    for (int i = 0; i < 2; ++i) {
        const XrFovf& f = views[i].fov;
        p.fov[i][0] = f.angleLeft;
        p.fov[i][1] = f.angleRight;
        p.fov[i][2] = f.angleUp;
        p.fov[i][3] = f.angleDown;
    }
    SaveHeadsetProfile(p);
}

// xrLocateViews for the open frame (once), applying a pending recenter. Also used by the menu screen, which needs
// the head pose for a recenter even though it renders nothing head-tracked.
bool LocateInternal()
{
    if (g.located) {
        return true;
    }
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    if (g.simulated) {
        SimulatedViews(views);
        viewState.viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    }
    else {
        XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
        info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        info.displayTime = g.displayTime;
        info.space = g.local;
        std::uint32_t count = 0;
        if (!Ok(xrLocateViews(g.session, &info, &viewState, 2, &count, views), "xrLocateViews") || count != 2 ||
            !(viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
            g.status.tracked = false;
            return false;
        }
    }
    g.status.tracked = true;
    const bool positionValid = (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
    for (int i = 0; i < 2; ++i) {
        const XrPosef& p = views[i].pose;
        EyeView& e = g.frameViews.eye[i];
        e.pose.orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
        if (positionValid) {
            g.lastPosition[i] = {p.position.x, p.position.y, p.position.z};
        }
        else {
            views[i].pose.position = {g.lastPosition[i].x, g.lastPosition[i].y, g.lastPosition[i].z};
        }
        e.pose.position = g.lastPosition[i];
        e.angleLeft = views[i].fov.angleLeft;
        e.angleRight = views[i].fov.angleRight;
        e.angleUp = views[i].fov.angleUp;
        e.angleDown = views[i].fov.angleDown;
        g.views[i] = views[i]; // submitted exactly as located (CLAUDE.md: pose and FOV actually rendered)
    }
    if (!g.simulated && !g.profileSaved && positionValid) {
        g.profileSaved = true;
        SaveProfileFrom(views);
    }
    if (g.recenterPending && g.displayTime >= g.recenterAfter) {
        // Head = midpoint of the eyes with the left eye's orientation (both eyes share it on current headsets).
        const stereo::Pose& l = g.frameViews.eye[0].pose;
        const stereo::Pose& r = g.frameViews.eye[1].pose;
        const stereo::Pose head{l.orientation,
            {(l.position.x + r.position.x) * 0.5f, (l.position.y + r.position.y) * 0.5f,
                (l.position.z + r.position.z) * 0.5f}};
        g.recenter = stereo::RecenterFromHead(head);
        g.recenterPending = false;
        ++g.recenterCount;
        TT2VR_LOG_INFO("xr: recentered at head position [{:.3f} {:.3f} {:.3f}] (LOCAL space)", head.position.x,
            head.position.y, head.position.z);
    }
    g.frameViews.recenter = g.recenter;
    g.located = true;
    return true;
}

} // namespace

bool LocateViews(FrameViews& out)
{
    if (!g.frameOpen || !g.shouldRender || !LocateInternal()) {
        return false;
    }
    out = g.frameViews;
    return true;
}

void MarkRendered(int view, int eye)
{
    if (g.located && view >= 0 && view < 2 && eye >= 0 && eye < 2) {
        g.rendered = true;
        g.viewEye[view] = eye;
    }
}

// HUD panel ([hud], PLAN Phase 7): the matte of the two HUD renders into the panel's image (premultiplied colour and
// alpha, linear values through an _SRGB view). Fixed in LOCAL space in front of the recentered head ("seat": the
// bike carries the camera, so LOCAL moves with the bike) or in VIEW space ("head"). false = no HUD layer this frame.
bool WriteHud(ID3D11DeviceContext* context, const HudSource& hud, XrCompositionLayerQuad& quad)
{
    if (!hud.black || !hud.white || hud.width == 0 || hud.height == 0) {
        return false;
    }
    if (!g.hud.Valid() || g.hud.Width() != hud.width || g.hud.Height() != hud.height) {
        if (!g.hud.Create(g.instance, g.session, g.device, g.format, hud.width, hud.height, "hud", true)) {
            return false;
        }
    }
    if (!g.blitter.Ready() && !g.blitter.Init(g.device)) {
        return false;
    }
    d3d::InternalScope internal;
    if (!g.hud.Acquire(g.instance)) {
        return false;
    }
    const bool ok = g.blitter.Matte(context, hud.black, hud.white, g.hud.Rtv(), hud.width, hud.height);
    g.hud.Release(g.instance);
    if (!ok) {
        return false;
    }
    const auto& hc = core::GetConfig().hud;
    const auto& xc = core::GetConfig().xr;
    const bool head = !hud.menu && hc.follow == "head" && g.view != XR_NULL_HANDLE;
    const stereo::Pose pose = hud.menu ? stereo::PanelPose(g.recenter, xc.screen_distance, 0.0f, 0.0f)
                                       : stereo::PanelPose(head ? stereo::Identity34() : g.recenter, hc.distance,
                                             hc.height, hc.tilt);
    const float width = hud.menu ? xc.screen_width : hc.width;
    quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; // premultiplied: no UNPREMULTIPLIED bit
    quad.space = head ? g.view : g.local;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = g.hud.Handle();
    quad.subImage.imageRect.extent = {static_cast<std::int32_t>(hud.width), static_cast<std::int32_t>(hud.height)};
    quad.pose.orientation = {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
    quad.pose.position = {pose.position.x, pose.position.y, pose.position.z};
    const float aspect = hud.aspect > 0.0f ? hud.aspect : static_cast<float>(hud.width) / static_cast<float>(hud.height);
    quad.size = {width, width / aspect};
    return true;
}

void SetSideBySide(bool sideBySide)
{
    g.sideBySide = sideBySide;
    g.status.sideBySide = sideBySide;
}

void OnFrameEnd(IDXGISwapChain* swapChain, ID3D11DeviceContext* context, const HudSource& hud)
{
    g.status.hudLayer = false;
    if (!g.frameOpen) {
        return;
    }
    if (g.simulated) {
        g.status.presentation = !g.rendered ? "screen" : g.sideBySide ? "stereo" : "head-tracked";
        g.status.hudLayer = g.rendered && g.sideBySide && hud.black && hud.white;
        g.frameOpen = false;
        if (g.rendered && g.sideBySide && swapChain && context && !g.simEyesFailed) {
            WriteSimulatedEyes(swapChain, context);
        }
        else {
            g.status.copyMode = "-";
        }
        return;
    }
    try {
        if (!g.shouldRender || !swapChain || !context) {
            EndFrame(nullptr);
            return;
        }
        if (!g.rendered) {
            // No head-tracked render this frame (menu, pause, loading, movie, unknown exe): flat frame on the screen.
            XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            if (g.recenterPending) {
                LocateInternal();
            }
            const bool screen = core::GetConfig().xr.menu_screen && WriteScreen(swapChain, context, quad);
            g.status.presentation = screen ? "screen" : "-";
            if (!screen) {
                g.status.copyMode = "-";
            }
            EndFrame(screen ? reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad) : nullptr);
            return;
        }
        if (!g.eyes[0].Valid() || !g.eyes[1].Valid() || !WriteEyes(swapChain, context)) {
            EndFrame(nullptr);
            return;
        }
        g.status.presentation = g.sideBySide ? "stereo" : "head-tracked";
        XrCompositionLayerProjectionView projViews[2] = {
            {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        for (int i = 0; i < 2; ++i) {
            // Each eye image is submitted with the pose and FOV it was rendered from (CLAUDE.md); in left-eye-only
            // mode eye 1 is black and keeps its own located view.
            const XrView& rendered = g.views[g.sideBySide ? g.viewEye[i] : i];
            projViews[i].pose = rendered.pose;
            projViews[i].fov = rendered.fov;
            projViews[i].subImage.swapchain = g.eyes[i].Handle();
            projViews[i].subImage.imageRect.extent = {static_cast<std::int32_t>(g.eyes[i].Width()),
                static_cast<std::int32_t>(g.eyes[i].Height())};
        }
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        layer.space = g.local;
        layer.viewCount = 2;
        layer.views = projViews;
        XrCompositionLayerQuad hudQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        g.status.hudLayer = g.sideBySide && WriteHud(context, hud, hudQuad);
        const XrCompositionLayerBaseHeader* layers[2] = {
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer),
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hudQuad)};
        EndFrame(layers, g.status.hudLayer ? 2u : 1u);
    }
    catch (...) {
        TT2VR_LOG_ERROR("xr: exception in OnFrameEnd");
        if (g.frameOpen) {
            EndFrame(nullptr);
        }
    }
}

namespace {

// The desktop mirror's eye image ([xr] desktop_view left / both). The window stretches the back buffer by
// (cw / bw, ch / bh): fit the image's shape into the window (or cover it, cropping: [xr] desktop_fill), then map that
// rectangle back into back-buffer pixels. A covering viewport extends past the back buffer and the rasterizer clips it.
void DrawMirrorImage(ID3D11DeviceContext* context, ID3D11Texture2D* back, const D3D11_TEXTURE2D_DESC& bd, float cw,
    float ch, float uw)
{
    const float bw = static_cast<float>(bd.Width), bh = static_cast<float>(bd.Height);
    const float aspect = uw * bw / bh;
    const float dw = core::GetConfig().xr.desktop_fill ? std::max(cw, ch * aspect) : std::min(cw, ch * aspect);
    const float dh = dw / aspect;
    const D3D11_VIEWPORT dst{(cw - dw) * 0.5f * bw / cw, (ch - dh) * 0.5f * bh / ch, dw * bw / cw, dh * bh / ch,
        0.0f, 1.0f};
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = d3d::LinearViewOf(bd.Format); // raw gamma-encoded values in, raw out
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(g.device->CreateRenderTargetView(back, &rd, &rtv))) {
        return;
    }
    d3d::InternalScope internal;
    // Copy first (the blitter's source copy, 1x1 draw), then clear and draw the letterboxed image.
    constexpr float kBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const D3D11_VIEWPORT corner{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    g.blitter.BlitRect(context, back, rtv.Get(), corner, false, 0.0f, 1.0f, true);
    context->ClearRenderTargetView(rtv.Get(), kBlack);
    g.blitter.BlitRect(context, back, rtv.Get(), dst, false, 0.0f, uw, false);
}

// The HUD panel's image (premultiplied colour and alpha, sRGB-encoded) over the whole window at its own shape (fitted,
// [hud] desktop), blended in linear space through an _SRGB view of the back buffer, as the game draws its HUD.
void DrawMirrorHud(ID3D11DeviceContext* context, ID3D11Texture2D* back, const D3D11_TEXTURE2D_DESC& bd, float cw,
    float ch, ID3D11ShaderResourceView* hud, float hudAspect)
{
    const float bw = static_cast<float>(bd.Width), bh = static_cast<float>(bd.Height);
    const float hw = std::min(cw, ch * hudAspect);
    const float hh = hw / hudAspect;
    const D3D11_VIEWPORT dst{(cw - hw) * 0.5f * bw / cw, (ch - hh) * 0.5f * bh / ch, hw * bw / cw, hh * bh / ch,
        0.0f, 1.0f};
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = d3d::SrgbViewOf(bd.Format);
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (rd.Format == DXGI_FORMAT_UNKNOWN || FAILED(g.device->CreateRenderTargetView(back, &rd, &rtv))) {
        return;
    }
    g.blitter.Composite(context, hud, rtv.Get(), dst);
}

} // namespace

void DrawDesktopMirror(IDXGISwapChain* swapChain, ID3D11DeviceContext* context, HWND window,
    ID3D11ShaderResourceView* hud, float hudAspect)
{
    const std::string& view = core::GetConfig().xr.desktop_view;
    const bool drawHud = hud && hudAspect > 0.0f && core::GetConfig().hud.desktop;
    if (!g.running || !swapChain || !context || !window || (view == "game" && !drawHud)) {
        return;
    }
    try {
        RECT rc{};
        if (!GetClientRect(window, &rc) || rc.right <= 0 || rc.bottom <= 0) {
            return;
        }
        D3D11_TEXTURE2D_DESC bd{};
        const ComPtr<ID3D11Texture2D> back = BackBuffer(swapChain, bd);
        if (!back || bd.Width == 0 || bd.Height == 0 || (!g.blitter.Ready() && !g.blitter.Init(g.device))) {
            return;
        }
        const float cw = static_cast<float>(rc.right), ch = static_cast<float>(rc.bottom);
        if (view != "game") {
            const float uw = g.sideBySide && view == "left" ? 0.5f : 1.0f; // one-view frames: the whole image
            DrawMirrorImage(context, back.Get(), bd, cw, ch, uw);
        }
        if (drawHud) {
            DrawMirrorHud(context, back.Get(), bd, cw, ch, hud, hudAspect);
        }
    }
    catch (...) {
        TT2VR_LOG_ERROR("xr: exception in DrawDesktopMirror");
    }
}

void RequestAlphaProbe(std::function<void(const std::string&)> done)
{
    if (!g.running || g.simulated) {
        done(g.simulated ? "xrprobe FAILED: simulated headset (test mode)" : "xrprobe FAILED: no XR session running");
        return;
    }
    if (g_probe) {
        g_probe("xrprobe FAILED: superseded by a newer request");
    }
    g_probe = std::move(done);
}

bool SessionRunning()
{
    return g.running;
}

unsigned RecenterCount()
{
    return g.recenterCount;
}

void RequestRecenter()
{
    g.recenterPending = true;
    g.recenterAfter = 0;
    TT2VR_LOG_INFO("xr: recenter requested");
}

bool Simulated()
{
    return g.simulated;
}

void SetSimulatedHead(float yawDeg, float pitchDeg, float rollDeg)
{
    g.simYaw = yawDeg;
    g.simPitch = pitchDeg;
    g.simRoll = rollDeg;
    g.shakeSeconds = 0.0f;
}

void SimulatedHeadShake(float amplitudeDeg, float periodS, float seconds)
{
    g.shakeAmp = amplitudeDeg;
    g.shakePeriod = std::max(periodS, 0.05f);
    g.shakeSeconds = std::max(seconds, 0.0f);
    g.shakeStart = Clock::now();
}

void RequestRestart()
{
    if (g.simulated) {
        TT2VR_LOG_INFO("xr: restart ignored (simulated headset)");
        return;
    }
    TT2VR_LOG_INFO("xr: restart requested");
    DestroyAll();
    g.blitter.Release();
    g.stopped = false;
    g.status.error.clear();
    g.nextAttempt = Clock::time_point{};
}

double LastWaitMs()
{
    return g.status.lastWaitMs;
}

Status GetStatus()
{
    Status s = g.status;
    s.running = g.running;
    return s;
}

std::uint32_t RenderEyeSize(std::uint32_t eyeSize)
{
    const float scale = g.renderScaleInit ? g.renderScale : core::GetConfig().xr.render_scale;
    if (scale >= 1.0f) {
        return eyeSize;
    }
    return static_cast<std::uint32_t>(std::lround(static_cast<float>(eyeSize) * scale)) & ~1u;
}

void RequestEyeShot(std::string name, std::function<void(const std::string&)> done)
{
    if (!g.simulated) {
        if (done) {
            done("eyeshot FAILED: simulated headset only (test mode)");
        }
        return;
    }
    if (name.empty()) {
        name = "eye";
    }
    g.eyeShots.push_back({std::move(name), std::move(done)});
}

} // namespace xr
