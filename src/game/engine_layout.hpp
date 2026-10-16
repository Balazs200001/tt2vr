// Engine struct offsets we read or write (build 4989319). Source of truth: docs/re/symbols.md sections 4a, 5a
// and the Renderer table; evidence in docs/re/camera.md. Offsets are data layout, not code addresses, so they
// are not signatures; a new exe build must re-check them (version table in game/version.hpp).
#pragma once

#include <cstddef>
#include <cstring>

namespace game::layout {

// Graphics::Camera (GraphNode-derived)
inline constexpr std::ptrdiff_t kCameraProjType = 0x58; // int, 0 = perspective
inline constexpr std::ptrdiff_t kCameraFov = 0x78; // float, HORIZONTAL fov in radians
inline constexpr std::ptrdiff_t kCameraNear = 0x7c;
inline constexpr std::ptrdiff_t kCameraFar = 0x80;
inline constexpr std::ptrdiff_t kCameraAspectMul = 0x84; // final aspect = vpW / vpH * this
inline constexpr std::ptrdiff_t kCameraFrontFov = 0x8c; // front pass (bike/rider foreground)
inline constexpr std::ptrdiff_t kCameraFrontNear = 0x90;
inline constexpr std::ptrdiff_t kCameraFrontFar = 0x94;
inline constexpr std::ptrdiff_t kCameraFrontPass = 0x98; // byte

// ViewContext (size 0x620)
inline constexpr std::ptrdiff_t kViewCamPos = 0x00; // float3, camera world position
inline constexpr std::ptrdiff_t kViewCullPos = 0x0c; // float3, frozen while g_bLockCullingQuery
inline constexpr std::ptrdiff_t kViewTransform = 0x18; // TransformMatrix (size 0x190)
inline constexpr std::ptrdiff_t kViewViewportW = 0x340; // int
inline constexpr std::ptrdiff_t kViewViewportH = 0x344; // int
inline constexpr std::ptrdiff_t kViewProjOffset = 0x448; // float2, TAA jitter / Ansel offset
inline constexpr std::ptrdiff_t kViewCamera = 0x450; // Camera*

// TransformMatrix slots (4x4, engine layout, stride 0x40)
inline constexpr std::ptrdiff_t kTmView = 0x00;
inline constexpr std::ptrdiff_t kTmWorld = 0x40; // inverse of view = camera world
inline constexpr std::ptrdiff_t kTmProj = 0xc0; // including jitter

// PlayAll::Game::Renderer
inline constexpr std::ptrdiff_t kRendererSplitMode = 0xe40; // cameraDesc.splitMode (0 = one view)
inline constexpr std::ptrdiff_t kRendererSplitCameras = 0xe48; // Camera*[2]
inline constexpr std::ptrdiff_t kRendererCameraMask = 0xe58; // single-camera index = ctz(mask)

template <typename T>
T Read(const void* base, std::ptrdiff_t offset)
{
    T value;
    std::memcpy(&value, static_cast<const char*>(base) + offset, sizeof(T));
    return value;
}

template <typename T>
void Write(void* base, std::ptrdiff_t offset, const T& value)
{
    std::memcpy(static_cast<char*>(base) + offset, &value, sizeof(T));
}

} // namespace game::layout
