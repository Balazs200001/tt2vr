#include "game/render_size.hpp"

#include "core/hooks.hpp"
#include "core/log.hpp"
#include "game/signatures.hpp"

#include <atomic>

namespace game {
namespace {

using ClientSizeFn = void (*)(HWND window, int* width, int* height);
using UpdateWindowResolutionFn = void (*)(void* videoDevice, HWND window);
using GetVideoDeviceFn = void* (*)();
using SetDeviceResetNeededFn = void (*)(void* videoDevice, void* window);

ClientSizeFn g_clientSize = nullptr;
UpdateWindowResolutionFn g_updateWindowResolution = nullptr;
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

using SetWindowPosFn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
SetWindowPosFn g_setWindowPos = nullptr;

// Thread: any (user32 export; the engine's SetSize 0x14095bc20 from SetScreenMode / DeviceReset). A resize of the
// main window to exactly the forced size (+ frame) is the engine applying our render size to its window: keep the
// window's own size (position and z-order changes still apply).
BOOL WINAPI HookedSetWindowPos(HWND window, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    const int w = g_width.load(std::memory_order_relaxed);
    const int h = g_height.load(std::memory_order_relaxed);
    if (w > 0 && h > 0 && window && window == g_window.load(std::memory_order_relaxed) && !(flags & SWP_NOSIZE)) {
        RECT frame{0, 0, w, h};
        AdjustWindowRectEx(&frame, static_cast<DWORD>(GetWindowLongW(window, GWL_STYLE)), FALSE,
            static_cast<DWORD>(GetWindowLongW(window, GWL_EXSTYLE)));
        if ((cx == frame.right - frame.left && cy == frame.bottom - frame.top) || (cx == w && cy == h)) {
            flags |= SWP_NOSIZE;
            static bool s_logged = false;
            if (!s_logged) {
                s_logged = true;
                RECT rc{};
                GetClientRect(window, &rc);
                TT2VR_LOG_INFO("render size: the game's resize of its window to {}x{} blocked (window keeps {}x{})",
                    cx, cy, rc.right, rc.bottom);
            }
        }
    }
    return g_setWindowPos(window, after, x, y, cx, cy, flags);
}

// Thread: game main thread (RenderWindowWin32::DefaultWndProc, WM_SIZE in Windowed mode).
void HookedUpdateWindowResolution(void* videoDevice, HWND window)
{
    if (g_width.load(std::memory_order_relaxed) > 0 && window && window == g_window.load(std::memory_order_relaxed)) {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            TT2VR_LOG_INFO("render size: window resize handling skipped while the render size is forced (the "
                           "window keeps its own size)");
        }
        return;
    }
    g_updateWindowResolution(videoDevice, window);
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
    if (g_active) {
        void* update = SignatureAddress(Sig::VideoDeviceUpdateWindowResolution);
        if (!update || core::hooks::Create("VideoDeviceDX11Windows::UpdateWindowResolution", update,
                           &HookedUpdateWindowResolution, &g_updateWindowResolution) != core::hooks::Result::Ok) {
            TT2VR_LOG_WARN("render size: UpdateWindowResolution not hooked; in Windowed mode the game may resize its "
                           "window to the render size (use Borderless)");
        }
        void* setWindowPos = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowPos"));
        if (!setWindowPos || core::hooks::Create("user32 SetWindowPos (main window, forced size)", setWindowPos,
                                 &HookedSetWindowPos, &g_setWindowPos) != core::hooks::Result::Ok) {
            TT2VR_LOG_WARN("render size: SetWindowPos not hooked; the game may resize its window to the render size");
        }
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
