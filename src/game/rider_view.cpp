#include "game/rider_view.hpp"

#include "core/log.hpp"
#include "game/engine_layout.hpp"
#include "game/signatures.hpp"

#include <spdlog/fmt/fmt.h>

#include <cstdint>
#include <iterator>

#include <windows.h>

namespace game {
namespace {

using namespace layout;

const void* g_springManage = nullptr;
RiderView g_last;

// SEH: game objects, read only. False if the player camera is not the rendered TTSpringCameraMode.
bool ReadFields(const void* ktuCamera, const void* rendered, RiderView& v)
{
    __try {
        if (!ktuCamera || !g_springManage || Read<const void*>(ktuCamera, kKTUCameraCamera) != rendered ||
            Read<const void*>(ktuCamera, kKTUCameraOverrideMode)) {
            return false;
        }
        const void* mode = Read<const void*>(ktuCamera, kKTUCameraMode);
        const void* const* vtable = mode ? *static_cast<const void* const* const*>(mode) : nullptr;
        if (!vtable || vtable[kModeManageSlot] != g_springManage) {
            return false;
        }
        v.mode = mode;
        v.view = Read<int>(mode, kModeViewEnum);
        v.rear = Read<std::uint8_t>(mode, kModeRearView) != 0;
        if (const void* views = Read<const void*>(mode, kModeCameraViews)) {
            const int index = Read<int>(mode, kModeCameraDataIndex);
            const auto* data = Read<const char*>(views, kCameraViewsData);
            if (data && index >= 0 && index < Read<int>(views, kCameraViewsCount)) {
                const char* cd = data + index * kCameraDataSize;
                v.type = Read<int>(cd, kCameraDataType);
                if (v.type == 0 || v.type == 2) {
                    v.settings = Read<const void*>(cd, kCameraDataSettings);
                }
            }
        }
        if (const void* vehicle = Read<const void*>(mode, kModeVehicle)) {
            if (const void* controller = Read<const void*>(vehicle, kVehicleController)) {
                v.crashed = Read<std::uint8_t>(controller, kControllerCrashed) != 0;
            }
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

void InitRiderView()
{
    g_springManage = SignatureAddress(Sig::SpringCameraModeManage);
    if (!g_springManage) {
        TT2VR_LOG_WARN("rider view: TTSpringCameraMode::Manage missing; riding views are not told apart");
    }
}

RiderView ReadRiderView(const void* ktuCamera, const void* rendered)
{
    RiderView v;
    v.valid = ReadFields(ktuCamera, rendered, v);
    if (!v.valid) {
        v = RiderView{};
    }
    if (v.valid != g_last.valid || v.view != g_last.view || v.type != g_last.type || v.crashed != g_last.crashed) {
        TT2VR_LOG_INFO("rider view: {}", DescribeRiderView(v));
    }
    g_last = v;
    return v;
}

RiderView LastRiderView()
{
    return g_last;
}

const char* ViewName(int view)
{
    static const char* const kNames[] = {"FollowFar", "FollowNear", "Helmet", "HelmetNoMesh", "Dash", "Fork",
        "LeftWheel", "Saddle", "Cockpit_01", "Bubble", "PitStop"};
    return view >= 0 && view < static_cast<int>(std::size(kNames)) ? kNames[view] : "none";
}

std::string DescribeRiderView(const RiderView& v)
{
    if (!v.valid) {
        return "not riding (no player TTSpringCameraMode rendered)";
    }
    static const char* const kTypes[] = {"Fixed", "Spring", "Cockpit", "Helicopter", "Photo"};
    const char* type = v.type >= 0 && v.type < static_cast<int>(std::size(kTypes)) ? kTypes[v.type] : "?";
    return fmt::format("view {} {} (camera type {} {}){}{} -> {}", v.view, ViewName(v.view), v.type, type,
        v.rear ? ", rear view" : "", v.crashed ? ", crashed" : "", v.Cockpit() ? "cockpit (look removal)" : "no look removal");
}

} // namespace game
