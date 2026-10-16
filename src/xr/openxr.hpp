// OpenXR headers with the D3D11 / Win32 platform types enabled. Include this instead of <openxr/openxr.h>.
#pragma once

#include <windows.h>
#include <d3d11.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <string>

namespace xr {

// "XR_ERROR_SESSION_LOST" etc. for log lines; falls back to the number without an instance.
std::string ResultString(XrInstance instance, XrResult result);

} // namespace xr
