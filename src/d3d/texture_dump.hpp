#pragma once

#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace d3d::dump {

using DoneFn = std::function<void(const std::string& result)>;

// Arms a texture dump of the next frame. Thread: present thread.
void Request(DoneFn onDone = {});
bool Pending();

// Present hook (present thread): Begin after Present returns, End before the next Present.
void Begin(std::uint64_t frame, ID3D11Device* device, ID3D11DeviceContext* immediate);
void End(std::uint64_t frame, IDXGISwapChain* swapChain);

namespace detail {
inline std::atomic<bool> g_active{false};
}
inline bool Active()
{
    return detail::g_active.load(std::memory_order_relaxed);
}

// Context hooks, while Active(). Binding calls are made before the original call runs.
// numRtvs / numUavs may be D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL / D3D11_KEEP_UNORDERED_ACCESS_VIEWS.
void OnSetTargets(ID3D11DeviceContext* ctx, UINT numRtvs, ID3D11RenderTargetView* const* rtvs,
    ID3D11DepthStencilView* dsv, bool setUavs, UINT uavStart, UINT numUavs, ID3D11UnorderedAccessView* const* uavs);
void OnSetCsUavs(ID3D11DeviceContext* ctx, UINT start, UINT num, ID3D11UnorderedAccessView* const* uavs);
void OnDraw(ID3D11DeviceContext* ctx);
void OnDispatch(ID3D11DeviceContext* ctx);
void OnClear(ID3D11DeviceContext* ctx, ID3D11View* view);
void OnCopy(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src);
void OnClearState(ID3D11DeviceContext* ctx);
void OnFinishList(ID3D11DeviceContext* ctx, ID3D11CommandList* list, BOOL restore); // after the original
void OnExecuteList(ID3D11DeviceContext* immediate, ID3D11CommandList* list, BOOL restore); // after the original

} // namespace d3d::dump
