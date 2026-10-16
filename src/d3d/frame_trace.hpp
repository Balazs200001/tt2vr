#pragma once

#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace d3d::trace {

using DoneFn = std::function<void(const std::string& result)>;

// Arms a trace of the next frame (Present to Present). Thread: present thread.
void Request(DoneFn onDone = {});
bool Pending();

// Called by the Present hook (present thread): Begin after Present returns, End before the next Present.
void Begin(std::uint64_t frame, ID3D11DeviceContext* immediate);
void End(std::uint64_t frame);

namespace detail {
inline std::atomic<bool> g_active{false};
}
// Cheap check for the hot path of every hook.
inline bool Active()
{
    return detail::g_active.load(std::memory_order_relaxed);
}

// Recording (any thread that records on a context, while Active()).
void Line(ID3D11DeviceContext* ctx, std::string text);
void FinishList(ID3D11DeviceContext* ctx, ID3D11CommandList* list);
void ExecuteList(ID3D11DeviceContext* immediate, ID3D11CommandList* list, BOOL restore);
void MapBuffer(ID3D11DeviceContext* ctx, ID3D11Resource* resource, D3D11_MAP type, const D3D11_MAPPED_SUBRESOURCE& mapped);
void UnmapBuffer(ID3D11DeviceContext* ctx, ID3D11Resource* resource);

// Short ids ("res7", "rtv12", "dss3"). The first sight of an object records its description
// in the trace's object table, so the object only has to be alive at the call.
std::string Res(ID3D11Resource* resource);
std::string View(ID3D11View* view);
std::string DepthState(ID3D11DepthStencilState* state);
std::string RasterState(ID3D11RasterizerState* state);
// Formats the first N floats of constant-buffer data, N = [capture] cb_floats.
std::string Floats(const void* data, std::size_t bytes);

} // namespace d3d::trace
