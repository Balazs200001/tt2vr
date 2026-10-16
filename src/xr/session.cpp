#include "xr/session.hpp"

#include "core/config.hpp"
#include "core/log.hpp"
#include "d3d/blit.hpp"
#include "d3d/internal.hpp"
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
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    XrEnvironmentBlendMode blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    bool running = false;
    EyeSwapchain eyes[2];
    EyeSwapchain screen; // menu screen quad (created on first use, sized like the back buffer)
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    d3d::Blitter blitter;

    // Current frame (xrBeginFrame .. xrEndFrame).
    bool frameOpen = false;
    XrTime displayTime = 0;
    bool shouldRender = false;
    bool located = false;
    bool rendered = false;
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    FrameViews frameViews{};

    // Recenter (XR LOCAL space, yaw-only head reference).
    stereo::Mat34 recenter = stereo::Identity34();
    bool recenterPending = true;
    XrTime recenterAfter = 0;
    stereo::Vec3 lastPosition[2] = {};

    Status status;
    bool syncWarned = false;
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
        g.blendMode = blendModes[0];
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
    const std::uint32_t w = scaled(configViews[0].recommendedImageRectWidth, configViews[0].maxImageRectWidth);
    const std::uint32_t h = scaled(configViews[0].recommendedImageRectHeight, configViews[0].maxImageRectHeight);
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
    TT2VR_LOG_INFO("xr: session created; recommended eye size {}x{} (max {}x{}), scale {} -> swapchains {}x{} format {}; "
                   "run tools/set-eye-resolution.ps1 -Width {} -Height {} so the game renders at this size",
        configViews[0].recommendedImageRectWidth, configViews[0].recommendedImageRectHeight,
        configViews[0].maxImageRectWidth, configViews[0].maxImageRectHeight, scale, w, h, static_cast<int>(format), w, h);
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

// Ends the open frame. With `layer`, submits the projection layer.
void EndFrame(const XrCompositionLayerBaseHeader* layer)
{
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = g.displayTime;
    end.environmentBlendMode = g.blendMode;
    end.layerCount = layer ? 1 : 0;
    end.layers = layer ? &layer : nullptr;
    if (Ok(xrEndFrame(g.session, &end), "xrEndFrame") && layer) {
        ++g.status.framesSubmitted;
    }
    g.frameOpen = g.located = g.rendered = false;
}

// Back buffer -> the target's current image: raw copy when size and family match, else a stretch blit.
// Returns "copy", "blit" or nullptr on failure. The caller has acquired the image.
const char* CopyBackBuffer(EyeSwapchain& target, ID3D11Texture2D* back, const D3D11_TEXTURE2D_DESC& bd,
    ID3D11DeviceContext* context)
{
    const bool copy = bd.Width == target.Width() && bd.Height == target.Height() && bd.SampleDesc.Count == 1 &&
                      d3d::IsSrgb(target.Format()) && d3d::SameFamily(bd.Format, target.TextureFormat());
    if (copy) {
        context->CopySubresourceRegion(target.Texture(), 0, 0, 0, 0, back, 0, nullptr);
        return "copy";
    }
    if (!g.blitter.Ready() && !g.blitter.Init(g.device)) {
        return nullptr;
    }
    return g.blitter.Blit(context, back, target.Rtv(), target.Width(), target.Height(), target.Decode()) ? "blit"
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

// Eye 0 <- back buffer; eye 1 <- black (Phase 3: left eye only).
bool WriteEyes(IDXGISwapChain* swapChain, ID3D11DeviceContext* context)
{
    D3D11_TEXTURE2D_DESC bd{};
    const ComPtr<ID3D11Texture2D> back = BackBuffer(swapChain, bd);
    if (!back) {
        return false;
    }
    d3d::InternalScope internal; // keep our copies out of frame traces and dumps
    EyeSwapchain& left = g.eyes[0];
    if (!left.Acquire(g.instance)) {
        return false;
    }
    const char* mode = CopyBackBuffer(left, back.Get(), bd, context);
    left.Release(g.instance);
    g.status.copyMode = mode ? mode : "-";

    EyeSwapchain& right = g.eyes[1];
    if (!right.Acquire(g.instance)) {
        return false;
    }
    constexpr float kBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    context->ClearRenderTargetView(right.Rtv(), kBlack);
    right.Release(g.instance);
    return mode != nullptr;
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
    if (!g.screen.Valid() || g.screen.Width() != bd.Width || g.screen.Height() != bd.Height) {
        if (!g.screen.Create(g.instance, g.session, g.device, g.format, bd.Width, bd.Height, "screen")) {
            return false;
        }
    }
    d3d::InternalScope internal;
    if (!g.screen.Acquire(g.instance)) {
        return false;
    }
    const char* mode = CopyBackBuffer(g.screen, back.Get(), bd, context);
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
    quad.subImage.imageRect.extent = {static_cast<std::int32_t>(bd.Width), static_cast<std::int32_t>(bd.Height)};
    quad.pose.orientation = {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
    quad.pose.position = {pose.position.x, pose.position.y, pose.position.z};
    quad.size = {xc.screen_width, xc.screen_width * static_cast<float>(bd.Height) / static_cast<float>(bd.Width)};
    return true;
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
}

void OnFrameStart()
{
    if (!g.configured || !g.status.enabled || !g.device) {
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
    }
    catch (...) {
        TT2VR_LOG_ERROR("xr: exception in OnFrameStart");
    }
}

namespace {

// xrLocateViews for the open frame (once), applying a pending recenter. Also used by the menu screen, which needs
// the head pose for a recenter even though it renders nothing head-tracked.
bool LocateInternal()
{
    if (g.located) {
        return true;
    }
    XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
    info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    info.displayTime = g.displayTime;
    info.space = g.local;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    std::uint32_t count = 0;
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    if (!Ok(xrLocateViews(g.session, &info, &viewState, 2, &count, views), "xrLocateViews") || count != 2 ||
        !(viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
        g.status.tracked = false;
        return false;
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
    if (g.recenterPending && g.displayTime >= g.recenterAfter) {
        // Head = midpoint of the eyes with the left eye's orientation (both eyes share it on current headsets).
        const stereo::Pose& l = g.frameViews.eye[0].pose;
        const stereo::Pose& r = g.frameViews.eye[1].pose;
        const stereo::Pose head{l.orientation,
            {(l.position.x + r.position.x) * 0.5f, (l.position.y + r.position.y) * 0.5f,
                (l.position.z + r.position.z) * 0.5f}};
        g.recenter = stereo::RecenterFromHead(head);
        g.recenterPending = false;
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

void MarkRendered()
{
    if (g.located) {
        g.rendered = true;
    }
}

void OnFrameEnd(IDXGISwapChain* swapChain, ID3D11DeviceContext* context)
{
    if (!g.frameOpen) {
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
        g.status.presentation = "head-tracked";
        XrCompositionLayerProjectionView projViews[2] = {
            {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        for (int i = 0; i < 2; ++i) {
            projViews[i].pose = g.views[i].pose;
            projViews[i].fov = g.views[i].fov;
            projViews[i].subImage.swapchain = g.eyes[i].Handle();
            projViews[i].subImage.imageRect.extent = {static_cast<std::int32_t>(g.eyes[i].Width()),
                static_cast<std::int32_t>(g.eyes[i].Height())};
        }
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        layer.space = g.local;
        layer.viewCount = 2;
        layer.views = projViews;
        EndFrame(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer));
    }
    catch (...) {
        TT2VR_LOG_ERROR("xr: exception in OnFrameEnd");
        if (g.frameOpen) {
            EndFrame(nullptr);
        }
    }
}

bool SessionRunning()
{
    return g.running;
}

void RequestRecenter()
{
    g.recenterPending = true;
    g.recenterAfter = 0;
    TT2VR_LOG_INFO("xr: recenter requested");
}

void RequestRestart()
{
    TT2VR_LOG_INFO("xr: restart requested");
    DestroyAll();
    g.blitter.Release();
    g.stopped = false;
    g.status.error.clear();
    g.nextAttempt = Clock::time_point{};
}

Status GetStatus()
{
    Status s = g.status;
    s.running = g.running;
    return s;
}

} // namespace xr
