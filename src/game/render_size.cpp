#include "game/render_size.hpp"

#include "core/hooks.hpp"
#include "core/log.hpp"
#include "game/signatures.hpp"

#include <atomic>

namespace game {
namespace {

using ClientSizeFn = void (*)(HWND window, int* width, int* height);
using GetVideoDeviceFn = void* (*)();
using SetDeviceResetNeededFn = void (*)(void* videoDevice, void* window);

ClientSizeFn g_clientSize = nullptr;
GetVideoDeviceFn g_getVideoDevice = nullptr;
SetDeviceResetNeededFn g_setDeviceResetNeeded = nullptr;
const void* g_dx11Vftable = nullptr;
bool g_active = false;

std::atomic<HWND> g_window{nullptr};
std::atomic<int> g_width{0};
std::atomic<int> g_height{0};

// Thread: game main thread (DeviceReset from PostFlip in the Present path; UpdateWindowResolution from the WndProc).
void HookedClientSize(HWND window, int* width, int* height)
{
    g_clientSize(window, width, height);
    const int w = g_width.load(std::memory_order_relaxed);
    const int h = g_height.load(std::memory_order_relaxed);
    if (w > 0 && h > 0 && window && window == g_window.load(std::memory_order_relaxed) && width && height) {
        *width = w;
        *height = h;
    }
}

} // namespace

bool InstallRenderSizeHook()
{
    static bool s_done = false;
    if (s_done) {
        return g_active;
    }
    s_done = true;
    void* clientSize = SignatureAddress(Sig::WindowClientSize);
    g_getVideoDevice = reinterpret_cast<GetVideoDeviceFn>(SignatureAddress(Sig::GetVideoDevice));
    g_setDeviceResetNeeded = reinterpret_cast<SetDeviceResetNeededFn>(SignatureAddress(Sig::SetDeviceResetNeeded));
    g_dx11Vftable = SignatureAddress(Sig::VideoDeviceDX11WindowsVftable);
    if (!clientSize || !g_getVideoDevice || !g_setDeviceResetNeeded || !g_dx11Vftable) {
        TT2VR_LOG_ERROR("render size: signatures missing; the game renders at its window size");
        return false;
    }
    g_active = core::hooks::Create("VideoDevice client size (FUN_140959f80)", clientSize, &HookedClientSize,
                   &g_clientSize) == core::hooks::Result::Ok;
    if (g_active) {
        TT2VR_LOG_INFO("render size: hook installed");
    }
    return g_active;
}

bool RenderSizeHookActive()
{
    return g_active;
}

void RequestRenderSize(HWND mainWindow, std::uint32_t width, std::uint32_t height)
{
    if (!g_active || !mainWindow) {
        return;
    }
    const int w = static_cast<int>(width & ~1u);
    const int h = static_cast<int>(height & ~1u);
    if (w == g_width.load() && h == g_height.load() && mainWindow == g_window.load()) {
        return;
    }
    g_window = mainWindow;
    g_width = w;
    g_height = h;
    void* device = g_getVideoDevice();
    if (!device || *static_cast<const void* const*>(device) != g_dx11Vftable) {
        // GetVideoDevice returns the ClassRegistry-created device; only call the DX11 method on that class.
        TT2VR_LOG_WARN("render size: video device missing or not VideoDeviceDX11Windows; the size applies at the "
                       "next device reset the game does itself");
        return;
    }
    g_setDeviceResetNeeded(device, nullptr); // VideoDeviceDX11Windows+0x4d8 = 1: DeviceReset at the next PostFlip
    TT2VR_LOG_INFO("render size: forcing {}x{} (device reset requested)", w, h);
}

} // namespace game
