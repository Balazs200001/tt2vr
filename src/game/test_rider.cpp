#include "game/test_rider.hpp"

#include "core/log.hpp"
#include "core/test_mode.hpp"
#include "game/camera.hpp"
#include "game/engine_layout.hpp"
#include "game/signatures.hpp"

#include <spdlog/fmt/fmt.h>

#include <cstdint>

#include <windows.h>

namespace game {
namespace {

using namespace layout;

// TTVehicle (the rider; TTCamera+0x38), delegated RE 2026-10-22:
//   +0x6e0  GenericStringEnum "Control" (0x20 bytes: vptr, value +8, enumeration +0x10, count +0x18)
//   vtable +0x2c0  SetControl(const GenericStringEnum&): TTVehicle's override re-points the physics input source
//                  (human / AI) and sets the AI or human settings up
//   +0x78   the AI virtual controller (Rc2VirtualController); its vtable +0x1a0 = enable(bool)
constexpr std::ptrdiff_t kRiderControl = 0x6e0;
constexpr std::ptrdiff_t kRiderControlValue = kRiderControl + 8;
constexpr std::ptrdiff_t kRiderVirtualController = 0x78;
constexpr std::ptrdiff_t kVtSetControl = 0x2c0;
constexpr std::ptrdiff_t kVtEnable = 0x1a0;
constexpr int kControlHuman = 0, kControlAi = 1;

using CopyEnumFn = void* (*)(void* out, const void* from);
using SetControlFn = void (*)(void* rider, const void* control);
using EnableFn = void (*)(void* controller, bool on);

struct alignas(16) EnumBuffer {
    unsigned char b[0x40];
};

void* LocalRider()
{
    const void* camera = LocalRiderCamera();
    if (!camera) {
        return nullptr;
    }
    void* rider = Read<void*>(camera, kTTCameraPlayer);
    return rider && Read<int>(rider, kPlayerKind) == 0 ? rider : nullptr;
}

// One engine call under SEH: 0, or the exception code.
template <typename F>
unsigned TryCall(F& f)
{
    __try {
        f();
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return static_cast<unsigned>(GetExceptionCode());
    }
}

// `step` names the call in the error.
template <typename F>
bool Guarded(const char* step, std::string& error, F&& f)
{
    const unsigned code = TryCall(f);
    if (code != 0) {
        error = fmt::format("exception 0x{:08x} in {}", code, step);
    }
    return code == 0;
}

std::uintptr_t ExeOffset(const void* p)
{
    return reinterpret_cast<std::uintptr_t>(p) - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
}

bool SetControl(void* rider, int value, CopyEnumFn copyEnum, std::string& error)
{
    EnumBuffer control{};
    if (!Guarded("copying the Control enum", error, [&] { copyEnum(&control, static_cast<char*>(rider) + kRiderControl); })) {
        return false;
    }
    Write<int>(&control, 8, value);
    void* controller = nullptr;
    if (!Guarded("reading the virtual controller", error, [&] { controller = Read<void*>(rider, kRiderVirtualController); })) {
        return false;
    }
    if (value == kControlAi && controller) {
        if (!Guarded("enabling the virtual controller", error, [&] {
                reinterpret_cast<EnableFn>((*static_cast<void* const* const*>(controller))[kVtEnable / 8])(controller, true);
            })) {
            return false;
        }
    }
    if (!Guarded("SetControl", error, [&] {
            reinterpret_cast<SetControlFn>((*static_cast<void* const* const*>(rider))[kVtSetControl / 8])(rider, &control);
        })) {
        return false;
    }
    if (value == kControlHuman && controller) {
        return Guarded("disabling the virtual controller", error, [&] {
            reinterpret_cast<EnableFn>((*static_cast<void* const* const*>(controller))[kVtEnable / 8])(controller, false);
        });
    }
    return true;
}

} // namespace

std::string AiRiderCommand(const std::string& arg)
{
    if (!core::TestMode()) {
        return "ai FAILED: test mode only (-tt2vr-test)";
    }
    if (arg != "on" && arg != "off") {
        return "ai FAILED: use ai on|off";
    }
    const auto copyEnum = reinterpret_cast<CopyEnumFn>(SignatureAddress(Sig::GenericStringEnumCopy));
    if (!copyEnum) {
        return "ai FAILED: GenericStringEnum copy constructor not found";
    }
    void* rider = LocalRider();
    if (!rider) {
        return "ai FAILED: no local rider (start riding first)";
    }
    const void* vtable = Read<const void*>(rider, 0);
    const int before = Read<int>(rider, kRiderControlValue);
    const int value = arg == "on" ? kControlAi : kControlHuman;
    std::string error;
    if (!SetControl(rider, value, copyEnum, error)) {
        return fmt::format("ai FAILED: {} (rider {} vtable TT2+{:#x}, control {})", error, rider, ExeOffset(vtable),
            before);
    }
    const int after = Read<int>(rider, kRiderControlValue);
    const std::string r = fmt::format("ai {}: rider {} control {} -> {}", arg, rider, before, after);
    TT2VR_LOG_INFO("test rider: {}", r);
    return after == value ? r : "ai FAILED: " + r;
}

} // namespace game
