// context_hooks.cpp - ID3D11DeviceContext hooks feeding the frame trace and the texture dump.
//
// Threads: the immediate context is used by the game main thread; deferred contexts by enkiTS
// workers (docs/re/frame-anatomy.md 1.5/1.6). Every detour can run on any of those threads.
// Variant V = 0 hooks the immediate context's vtable, V = 1 a deferred context's vtable; when both
// vtables point at the same function, only the V = 0 hook exists and serves both.
#include "d3d/context_hooks.hpp"

#include "core/hooks.hpp"
#include "core/log.hpp"
#include "d3d/frame_trace.hpp"
#include "d3d/internal.hpp"
#include "d3d/shader_registry.hpp"
#include "d3d/texture_dump.hpp"
#include "d3d/vtable.hpp"

#include <d3d11_1.h>
#include <spdlog/fmt/fmt.h>
#include <wrl/client.h>

#include <atomic>
#include <string>

namespace d3d {
namespace {

using Ctx = ID3D11DeviceContext;
using shaders::Stage;

std::atomic<PreDrawCallback> g_preDraw[kPreDrawSlots]{};

void PreDraw(Ctx* c)
{
    for (auto& slot : g_preDraw) {
        if (const PreDrawCallback pre = slot.load(std::memory_order_acquire); pre && !InInternalCall()) {
            pre(c);
        }
    }
}

bool Tracing()
{
    return trace::Active() && !InInternalCall();
}

bool Dumping()
{
    return dump::Active() && !InInternalCall();
}

const char* Prefix(Stage s)
{
    static constexpr const char* kNames[] = {"VS", "PS", "CS", "GS", "HS", "DS"};
    return kNames[static_cast<int>(s)];
}

template <typename View>
std::string ViewList(View* const* views, UINT n)
{
    std::string s = "[";
    for (UINT i = 0; i < n; ++i) {
        s += (i ? " " : "");
        s += views ? trace::View(views[i]) : "null";
    }
    return s + "]";
}

std::string BufferList(ID3D11Buffer* const* buffers, UINT n)
{
    std::string s = "[";
    for (UINT i = 0; i < n; ++i) {
        s += (i ? " " : "");
        s += buffers ? trace::Res(buffers[i]) : "null";
    }
    return s + "]";
}

std::string Box(const D3D11_BOX* box)
{
    return box ? fmt::format("box=({},{},{})-({},{},{})", box->left, box->top, box->front, box->right, box->bottom,
                     box->back)
               : "box=all";
}

bool IsConstantBuffer(ID3D11Resource* resource)
{
    D3D11_RESOURCE_DIMENSION dim{};
    resource->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) {
        return false;
    }
    D3D11_BUFFER_DESC d{};
    static_cast<ID3D11Buffer*>(resource)->GetDesc(&d);
    return (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
}

template <int V>
struct H {
    static inline void* o[136] = {}; // trampolines, indexed by vtable slot

    template <int Slot, typename Fn>
    static Fn Orig()
    {
        return reinterpret_cast<Fn>(o[Slot]);
    }

    // ---- per-stage binds ----
    template <int Slot, typename Shader, Stage S>
    static void STDMETHODCALLTYPE SetShader(Ctx* c, Shader* shader, ID3D11ClassInstance* const* ci, UINT n)
    {
        if (Tracing()) {
            trace::Line(c, fmt::format("{}SetShader {}", Prefix(S), shaders::Label(shader, S)));
        }
        Orig<Slot, void(STDMETHODCALLTYPE*)(Ctx*, Shader*, ID3D11ClassInstance* const*, UINT)>()(c, shader, ci, n);
    }

    template <int Slot, Stage S>
    static void STDMETHODCALLTYPE SetSRVs(Ctx* c, UINT start, UINT n, ID3D11ShaderResourceView* const* views)
    {
        if (Tracing()) {
            trace::Line(c, fmt::format("{}SetShaderResources {} {}", Prefix(S), start, ViewList(views, n)));
        }
        Orig<Slot, void(STDMETHODCALLTYPE*)(Ctx*, UINT, UINT, ID3D11ShaderResourceView* const*)>()(c, start, n, views);
    }

    template <int Slot, Stage S>
    static void STDMETHODCALLTYPE SetCBs(Ctx* c, UINT start, UINT n, ID3D11Buffer* const* buffers)
    {
        if (Tracing()) {
            trace::Line(c, fmt::format("{}SetConstantBuffers {} {}", Prefix(S), start, BufferList(buffers, n)));
        }
        Orig<Slot, void(STDMETHODCALLTYPE*)(Ctx*, UINT, UINT, ID3D11Buffer* const*)>()(c, start, n, buffers);
    }

    template <int Slot, Stage S>
    static void STDMETHODCALLTYPE SetCBs1(ID3D11DeviceContext1* c, UINT start, UINT n, ID3D11Buffer* const* buffers,
        const UINT* first, const UINT* num)
    {
        if (Tracing()) {
            std::string s = "[";
            for (UINT i = 0; i < n; ++i) {
                s += (i ? " " : "");
                s += buffers ? trace::Res(buffers[i]) : "null";
                if (first && num) {
                    s += fmt::format("+{}x{}", first[i], num[i]); // in 16-byte constants
                }
            }
            trace::Line(c, fmt::format("{}SetConstantBuffers1 {} {}]", Prefix(S), start, s));
        }
        Orig<Slot, void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, UINT, UINT, ID3D11Buffer* const*, const UINT*,
            const UINT*)>()(c, start, n, buffers, first, num);
    }

    // ---- output merger / rasterizer ----
    static void STDMETHODCALLTYPE OMSetRenderTargets(Ctx* c, UINT n, ID3D11RenderTargetView* const* rtvs,
        ID3D11DepthStencilView* dsv)
    {
        if (Tracing()) {
            trace::Line(c, fmt::format("OMSetRenderTargets {} dsv={}", ViewList(rtvs, n), trace::View(dsv)));
        }
        if (Dumping()) {
            dump::OnSetTargets(c, n, rtvs, dsv, false, 0, 0, nullptr);
        }
        Orig<vt::ctx::OMSetRenderTargets, decltype(&OMSetRenderTargets)>()(c, n, rtvs, dsv);
    }

    static void STDMETHODCALLTYPE OMSetRenderTargetsAndUAVs(Ctx* c, UINT nRtv, ID3D11RenderTargetView* const* rtvs,
        ID3D11DepthStencilView* dsv, UINT uavStart, UINT nUav, ID3D11UnorderedAccessView* const* uavs,
        const UINT* counts)
    {
        if (Tracing()) {
            const std::string rt = nRtv == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL
                                       ? std::string("keep")
                                       : fmt::format("{} dsv={}", ViewList(rtvs, nRtv), trace::View(dsv));
            const std::string ua = nUav == D3D11_KEEP_UNORDERED_ACCESS_VIEWS
                                       ? std::string("keep")
                                       : fmt::format("{} {}", uavStart, ViewList(uavs, nUav));
            trace::Line(c, fmt::format("OMSetRenderTargetsAndUnorderedAccessViews rt={} uav={}", rt, ua));
        }
        if (Dumping()) {
            dump::OnSetTargets(c, nRtv, rtvs, dsv, true, uavStart, nUav, uavs);
        }
        Orig<vt::ctx::OMSetRenderTargetsAndUnorderedAccessViews, decltype(&OMSetRenderTargetsAndUAVs)>()(
            c, nRtv, rtvs, dsv, uavStart, nUav, uavs, counts);
    }

    static void STDMETHODCALLTYPE OMSetDepthStencilState(Ctx* c, ID3D11DepthStencilState* state, UINT ref)
    {
        if (Tracing()) {
            trace::Line(c, fmt::format("OMSetDepthStencilState {} ref={}", trace::DepthState(state), ref));
        }
        Orig<vt::ctx::OMSetDepthStencilState, decltype(&OMSetDepthStencilState)>()(c, state, ref);
    }

    static void STDMETHODCALLTYPE RSSetState(Ctx* c, ID3D11RasterizerState* state)
    {
        if (Tracing()) {
            trace::Line(c, fmt::format("RSSetState {}", trace::RasterState(state)));
        }
        Orig<vt::ctx::RSSetState, decltype(&RSSetState)>()(c, state);
    }

    static void STDMETHODCALLTYPE RSSetViewports(Ctx* c, UINT n, const D3D11_VIEWPORT* vps)
    {
        if (Tracing()) {
            std::string s;
            for (UINT i = 0; vps && i < n; ++i) {
                s += fmt::format(" [{}: {},{} {}x{} z{}..{}]", i, vps[i].TopLeftX, vps[i].TopLeftY, vps[i].Width,
                    vps[i].Height, vps[i].MinDepth, vps[i].MaxDepth);
            }
            trace::Line(c, "RSSetViewports" + s);
        }
        Orig<vt::ctx::RSSetViewports, decltype(&RSSetViewports)>()(c, n, vps);
    }

    static void STDMETHODCALLTYPE CSSetUAVs(Ctx* c, UINT start, UINT n, ID3D11UnorderedAccessView* const* uavs,
        const UINT* counts)
    {
        if (Tracing()) {
            trace::Line(c, fmt::format("CSSetUnorderedAccessViews {} {}", start, ViewList(uavs, n)));
        }
        if (Dumping()) {
            dump::OnSetCsUavs(c, start, n, uavs);
        }
        Orig<vt::ctx::CSSetUnorderedAccessViews, decltype(&CSSetUAVs)>()(c, start, n, uavs, counts);
    }

    // ---- draws / dispatches ----
    static void Drew(Ctx* c, std::string text)
    {
        if (Tracing()) {
            trace::Line(c, std::move(text));
        }
        if (Dumping()) {
            dump::OnDraw(c);
        }
    }

    static void STDMETHODCALLTYPE DrawIndexed(Ctx* c, UINT count, UINT start, INT base)
    {
        PreDraw(c);
        if (Tracing() || Dumping()) {
            Drew(c, fmt::format("DrawIndexed idx={} start={} base={}", count, start, base));
        }
        Orig<vt::ctx::DrawIndexed, decltype(&DrawIndexed)>()(c, count, start, base);
    }

    static void STDMETHODCALLTYPE Draw(Ctx* c, UINT count, UINT start)
    {
        PreDraw(c);
        if (Tracing() || Dumping()) {
            Drew(c, fmt::format("Draw v={} start={}", count, start));
        }
        Orig<vt::ctx::Draw, decltype(&Draw)>()(c, count, start);
    }

    static void STDMETHODCALLTYPE DrawIndexedInstanced(Ctx* c, UINT count, UINT inst, UINT start, INT base, UINT startInst)
    {
        PreDraw(c);
        if (Tracing() || Dumping()) {
            Drew(c, fmt::format("DrawIndexedInstanced idx={} inst={} start={} base={} startInst={}", count, inst,
                start, base, startInst));
        }
        Orig<vt::ctx::DrawIndexedInstanced, decltype(&DrawIndexedInstanced)>()(c, count, inst, start, base, startInst);
    }

    static void STDMETHODCALLTYPE DrawInstanced(Ctx* c, UINT count, UINT inst, UINT start, UINT startInst)
    {
        PreDraw(c);
        if (Tracing() || Dumping()) {
            Drew(c, fmt::format("DrawInstanced v={} inst={} start={} startInst={}", count, inst, start, startInst));
        }
        Orig<vt::ctx::DrawInstanced, decltype(&DrawInstanced)>()(c, count, inst, start, startInst);
    }

    static void STDMETHODCALLTYPE DrawAuto(Ctx* c)
    {
        PreDraw(c);
        if (Tracing() || Dumping()) {
            Drew(c, "DrawAuto");
        }
        Orig<vt::ctx::DrawAuto, decltype(&DrawAuto)>()(c);
    }

    static void STDMETHODCALLTYPE DrawIndexedInstancedIndirect(Ctx* c, ID3D11Buffer* args, UINT offset)
    {
        PreDraw(c);
        if (Tracing() || Dumping()) {
            Drew(c, fmt::format("DrawIndexedInstancedIndirect args={}+{}", Tracing() ? trace::Res(args) : "", offset));
        }
        Orig<vt::ctx::DrawIndexedInstancedIndirect, decltype(&DrawIndexedInstancedIndirect)>()(c, args, offset);
    }

    static void STDMETHODCALLTYPE DrawInstancedIndirect(Ctx* c, ID3D11Buffer* args, UINT offset)
    {
        PreDraw(c);
        if (Tracing() || Dumping()) {
            Drew(c, fmt::format("DrawInstancedIndirect args={}+{}", Tracing() ? trace::Res(args) : "", offset));
        }
        Orig<vt::ctx::DrawInstancedIndirect, decltype(&DrawInstancedIndirect)>()(c, args, offset);
    }

    static void STDMETHODCALLTYPE Dispatch(Ctx* c, UINT x, UINT y, UINT z)
    {
        if (Tracing()) {
            trace::Line(c, fmt::format("Dispatch {}x{}x{}", x, y, z));
        }
        if (Dumping()) {
            dump::OnDispatch(c);
        }
        Orig<vt::ctx::Dispatch, decltype(&Dispatch)>()(c, x, y, z);
    }

    static void STDMETHODCALLTYPE DispatchIndirect(Ctx* c, ID3D11Buffer* args, UINT offset)
    {
        if (Tracing()) {
            trace::Line(c, fmt::format("DispatchIndirect args={}+{}", trace::Res(args), offset));
        }
        if (Dumping()) {
            dump::OnDispatch(c);
        }
        Orig<vt::ctx::DispatchIndirect, decltype(&DispatchIndirect)>()(c, args, offset);
    }

    // ---- copies / updates / clears ----
    static void STDMETHODCALLTYPE CopySubresourceRegion(Ctx* c, ID3D11Resource* dst, UINT dstSub, UINT x, UINT y,
        UINT z, ID3D11Resource* src, UINT srcSub, const D3D11_BOX* box)
    {
        if (Tracing()) {
            trace::Line(c, fmt::format("CopySubresourceRegion dst={}[{}] at ({},{},{}) src={}[{}] {}", trace::Res(dst),
                dstSub, x, y, z, trace::Res(src), srcSub, Box(box)));
        }
        if (Dumping()) {
            dump::OnCopy(c, dst, src);
        }
        Orig<vt::ctx::CopySubresourceRegion, decltype(&CopySubresourceRegion)>()(c, dst, dstSub, x, y, z, src, srcSub, box);
    }

    static void STDMETHODCALLTYPE CopyResource(Ctx* c, ID3D11Resource* dst, ID3D11Resource* src)
    {
        if (Tracing()) {
            trace::Line(c, fmt::format("CopyResource dst={} src={}", trace::Res(dst), trace::Res(src)));
        }
        if (Dumping()) {
            dump::OnCopy(c, dst, src);
        }
        Orig<vt::ctx::CopyResource, decltype(&CopyResource)>()(c, dst, src);
    }

    static void STDMETHODCALLTYPE ResolveSubresource(Ctx* c, ID3D11Resource* dst, UINT dstSub, ID3D11Resource* src,
        UINT srcSub, DXGI_FORMAT format)
    {
        if (Tracing()) {
            trace::Line(c, fmt::format("ResolveSubresource dst={}[{}] src={}[{}] fmt={}", trace::Res(dst), dstSub,
                trace::Res(src), srcSub, static_cast<int>(format)));
        }
        if (Dumping()) {
            dump::OnCopy(c, dst, src);
        }
        Orig<vt::ctx::ResolveSubresource, decltype(&ResolveSubresource)>()(c, dst, dstSub, src, srcSub, format);
    }

    static void STDMETHODCALLTYPE UpdateSubresource(Ctx* c, ID3D11Resource* dst, UINT sub, const D3D11_BOX* box,
        const void* data, UINT rowPitch, UINT depthPitch)
    {
        if (Tracing()) {
            std::string text = fmt::format("UpdateSubresource dst={}[{}] {} pitch={}/{}", trace::Res(dst), sub, Box(box),
                rowPitch, depthPitch);
            if (dst && IsConstantBuffer(dst)) {
                D3D11_BUFFER_DESC d{};
                static_cast<ID3D11Buffer*>(dst)->GetDesc(&d);
                const UINT bytes = box ? box->right - box->left : d.ByteWidth;
                text += fmt::format("\n    cb data {} B: {}", bytes, trace::Floats(data, bytes));
            }
            trace::Line(c, std::move(text));
        }
        Orig<vt::ctx::UpdateSubresource, decltype(&UpdateSubresource)>()(c, dst, sub, box, data, rowPitch, depthPitch);
    }

    static void STDMETHODCALLTYPE ClearRenderTargetView(Ctx* c, ID3D11RenderTargetView* rtv, const FLOAT color[4])
    {
        if (Tracing()) {
            trace::Line(c, fmt::format("ClearRenderTargetView {} ({} {} {} {})", trace::View(rtv), color[0], color[1],
                color[2], color[3]));
        }
        if (Dumping()) {
            dump::OnClear(c, rtv);
        }
        Orig<vt::ctx::ClearRenderTargetView, decltype(&ClearRenderTargetView)>()(c, rtv, color);
    }

    static void STDMETHODCALLTYPE ClearUAVUint(Ctx* c, ID3D11UnorderedAccessView* uav, const UINT values[4])
    {
        if (Tracing()) {
            trace::Line(c, fmt::format("ClearUnorderedAccessViewUint {} ({} {} {} {})", trace::View(uav), values[0],
                values[1], values[2], values[3]));
        }
        if (Dumping()) {
            dump::OnClear(c, uav);
        }
        Orig<vt::ctx::ClearUnorderedAccessViewUint, decltype(&ClearUAVUint)>()(c, uav, values);
    }

    static void STDMETHODCALLTYPE ClearUAVFloat(Ctx* c, ID3D11UnorderedAccessView* uav, const FLOAT values[4])
    {
        if (Tracing()) {
            trace::Line(c, fmt::format("ClearUnorderedAccessViewFloat {} ({} {} {} {})", trace::View(uav), values[0],
                values[1], values[2], values[3]));
        }
        if (Dumping()) {
            dump::OnClear(c, uav);
        }
        Orig<vt::ctx::ClearUnorderedAccessViewFloat, decltype(&ClearUAVFloat)>()(c, uav, values);
    }

    static void STDMETHODCALLTYPE ClearDepthStencilView(Ctx* c, ID3D11DepthStencilView* dsv, UINT flags, FLOAT depth,
        UINT8 stencil)
    {
        if (Tracing()) {
            trace::Line(c, fmt::format("ClearDepthStencilView {} flags={} depth={} stencil={}", trace::View(dsv), flags,
                depth, stencil));
        }
        if (Dumping()) {
            dump::OnClear(c, dsv);
        }
        Orig<vt::ctx::ClearDepthStencilView, decltype(&ClearDepthStencilView)>()(c, dsv, flags, depth, stencil);
    }

    static void STDMETHODCALLTYPE GenerateMips(Ctx* c, ID3D11ShaderResourceView* srv)
    {
        if (Tracing()) {
            trace::Line(c, fmt::format("GenerateMips {}", trace::View(srv)));
        }
        Orig<vt::ctx::GenerateMips, decltype(&GenerateMips)>()(c, srv);
    }

    static HRESULT STDMETHODCALLTYPE Map(Ctx* c, ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags,
        D3D11_MAPPED_SUBRESOURCE* mapped)
    {
        const HRESULT hr = Orig<vt::ctx::Map, decltype(&Map)>()(c, res, sub, type, flags, mapped);
        if (Tracing() && res) {
            trace::Line(c, fmt::format("Map {}[{}] type={} flags={} hr=0x{:08x}", trace::Res(res), sub,
                static_cast<int>(type), flags, static_cast<unsigned>(hr)));
            if (SUCCEEDED(hr) && mapped) {
                trace::MapBuffer(c, res, type, *mapped);
            }
        }
        return hr;
    }

    static void STDMETHODCALLTYPE Unmap(Ctx* c, ID3D11Resource* res, UINT sub)
    {
        if (Tracing() && res) {
            trace::UnmapBuffer(c, res); // logs the constant-buffer contents the game just wrote
        }
        Orig<vt::ctx::Unmap, decltype(&Unmap)>()(c, res, sub);
    }

    // ---- command lists / state ----
    static void STDMETHODCALLTYPE ExecuteCommandList(Ctx* c, ID3D11CommandList* list, BOOL restore)
    {
        if (Tracing()) {
            trace::ExecuteList(c, list, restore);
        }
        Orig<vt::ctx::ExecuteCommandList, decltype(&ExecuteCommandList)>()(c, list, restore);
        if (Dumping()) {
            dump::OnExecuteList(c, list, restore);
        }
    }

    static HRESULT STDMETHODCALLTYPE FinishCommandList(Ctx* c, BOOL restore, ID3D11CommandList** list)
    {
        const HRESULT hr = Orig<vt::ctx::FinishCommandList, decltype(&FinishCommandList)>()(c, restore, list);
        if (SUCCEEDED(hr) && list && *list) {
            if (Tracing()) {
                trace::FinishList(c, *list);
            }
            if (Dumping()) {
                dump::OnFinishList(c, *list, restore);
            }
        }
        return hr;
    }

    static void STDMETHODCALLTYPE ClearState(Ctx* c)
    {
        if (Tracing()) {
            trace::Line(c, "ClearState");
        }
        if (Dumping()) {
            dump::OnClearState(c);
        }
        Orig<vt::ctx::ClearState, decltype(&ClearState)>()(c);
    }

    // ---- installation ----
    template <typename Fn>
    static void Hook(void** vtbl, int slot, Fn detour, const char* method, int& created, int& shared)
    {
        const std::string name = fmt::format("{}::{}", V == 0 ? "ImmediateContext" : "DeferredContext", method);
        switch (core::hooks::Create(name.c_str(), vtbl[slot], reinterpret_cast<void*>(detour), &o[slot])) {
        case core::hooks::Result::Ok: ++created; break;
        case core::hooks::Result::AlreadyHooked: ++shared; break;
        case core::hooks::Result::Failed: break;
        }
    }

    static void Install(void** vtbl, bool hasContext1)
    {
        namespace c = vt::ctx;
        int created = 0;
        int shared = 0;
#define TT2VR_HOOK(slot, fn) Hook(vtbl, slot, fn, #slot + 9, created, shared) // "+ 9" strips "vt::ctx::"
        TT2VR_HOOK(vt::ctx::VSSetShader, (&SetShader<c::VSSetShader, ID3D11VertexShader, Stage::VS>));
        TT2VR_HOOK(vt::ctx::PSSetShader, (&SetShader<c::PSSetShader, ID3D11PixelShader, Stage::PS>));
        TT2VR_HOOK(vt::ctx::CSSetShader, (&SetShader<c::CSSetShader, ID3D11ComputeShader, Stage::CS>));
        TT2VR_HOOK(vt::ctx::GSSetShader, (&SetShader<c::GSSetShader, ID3D11GeometryShader, Stage::GS>));
        TT2VR_HOOK(vt::ctx::HSSetShader, (&SetShader<c::HSSetShader, ID3D11HullShader, Stage::HS>));
        TT2VR_HOOK(vt::ctx::DSSetShader, (&SetShader<c::DSSetShader, ID3D11DomainShader, Stage::DS>));
        TT2VR_HOOK(vt::ctx::VSSetShaderResources, (&SetSRVs<c::VSSetShaderResources, Stage::VS>));
        TT2VR_HOOK(vt::ctx::PSSetShaderResources, (&SetSRVs<c::PSSetShaderResources, Stage::PS>));
        TT2VR_HOOK(vt::ctx::CSSetShaderResources, (&SetSRVs<c::CSSetShaderResources, Stage::CS>));
        TT2VR_HOOK(vt::ctx::GSSetShaderResources, (&SetSRVs<c::GSSetShaderResources, Stage::GS>));
        TT2VR_HOOK(vt::ctx::HSSetShaderResources, (&SetSRVs<c::HSSetShaderResources, Stage::HS>));
        TT2VR_HOOK(vt::ctx::DSSetShaderResources, (&SetSRVs<c::DSSetShaderResources, Stage::DS>));
        TT2VR_HOOK(vt::ctx::VSSetConstantBuffers, (&SetCBs<c::VSSetConstantBuffers, Stage::VS>));
        TT2VR_HOOK(vt::ctx::PSSetConstantBuffers, (&SetCBs<c::PSSetConstantBuffers, Stage::PS>));
        TT2VR_HOOK(vt::ctx::CSSetConstantBuffers, (&SetCBs<c::CSSetConstantBuffers, Stage::CS>));
        TT2VR_HOOK(vt::ctx::GSSetConstantBuffers, (&SetCBs<c::GSSetConstantBuffers, Stage::GS>));
        TT2VR_HOOK(vt::ctx::HSSetConstantBuffers, (&SetCBs<c::HSSetConstantBuffers, Stage::HS>));
        TT2VR_HOOK(vt::ctx::DSSetConstantBuffers, (&SetCBs<c::DSSetConstantBuffers, Stage::DS>));
        TT2VR_HOOK(vt::ctx::OMSetRenderTargets, &OMSetRenderTargets);
        TT2VR_HOOK(vt::ctx::OMSetRenderTargetsAndUnorderedAccessViews, &OMSetRenderTargetsAndUAVs);
        TT2VR_HOOK(vt::ctx::OMSetDepthStencilState, &OMSetDepthStencilState);
        TT2VR_HOOK(vt::ctx::RSSetState, &RSSetState);
        TT2VR_HOOK(vt::ctx::RSSetViewports, &RSSetViewports);
        TT2VR_HOOK(vt::ctx::CSSetUnorderedAccessViews, &CSSetUAVs);
        TT2VR_HOOK(vt::ctx::DrawIndexed, &DrawIndexed);
        TT2VR_HOOK(vt::ctx::Draw, &Draw);
        TT2VR_HOOK(vt::ctx::DrawIndexedInstanced, &DrawIndexedInstanced);
        TT2VR_HOOK(vt::ctx::DrawInstanced, &DrawInstanced);
        TT2VR_HOOK(vt::ctx::DrawAuto, &DrawAuto);
        TT2VR_HOOK(vt::ctx::DrawIndexedInstancedIndirect, &DrawIndexedInstancedIndirect);
        TT2VR_HOOK(vt::ctx::DrawInstancedIndirect, &DrawInstancedIndirect);
        TT2VR_HOOK(vt::ctx::Dispatch, &Dispatch);
        TT2VR_HOOK(vt::ctx::DispatchIndirect, &DispatchIndirect);
        TT2VR_HOOK(vt::ctx::CopySubresourceRegion, &CopySubresourceRegion);
        TT2VR_HOOK(vt::ctx::CopyResource, &CopyResource);
        TT2VR_HOOK(vt::ctx::ResolveSubresource, &ResolveSubresource);
        TT2VR_HOOK(vt::ctx::UpdateSubresource, &UpdateSubresource);
        TT2VR_HOOK(vt::ctx::ClearRenderTargetView, &ClearRenderTargetView);
        TT2VR_HOOK(vt::ctx::ClearUnorderedAccessViewUint, &ClearUAVUint);
        TT2VR_HOOK(vt::ctx::ClearUnorderedAccessViewFloat, &ClearUAVFloat);
        TT2VR_HOOK(vt::ctx::ClearDepthStencilView, &ClearDepthStencilView);
        TT2VR_HOOK(vt::ctx::GenerateMips, &GenerateMips);
        TT2VR_HOOK(vt::ctx::Map, &Map);
        TT2VR_HOOK(vt::ctx::Unmap, &Unmap);
        TT2VR_HOOK(vt::ctx::ExecuteCommandList, &ExecuteCommandList);
        TT2VR_HOOK(vt::ctx::FinishCommandList, &FinishCommandList);
        TT2VR_HOOK(vt::ctx::ClearState, &ClearState);
        if (hasContext1) {
            TT2VR_HOOK(vt::ctx::VSSetConstantBuffers1, (&SetCBs1<c::VSSetConstantBuffers1, Stage::VS>));
            TT2VR_HOOK(vt::ctx::PSSetConstantBuffers1, (&SetCBs1<c::PSSetConstantBuffers1, Stage::PS>));
            TT2VR_HOOK(vt::ctx::CSSetConstantBuffers1, (&SetCBs1<c::CSSetConstantBuffers1, Stage::CS>));
            TT2VR_HOOK(vt::ctx::GSSetConstantBuffers1, (&SetCBs1<c::GSSetConstantBuffers1, Stage::GS>));
            TT2VR_HOOK(vt::ctx::HSSetConstantBuffers1, (&SetCBs1<c::HSSetConstantBuffers1, Stage::HS>));
            TT2VR_HOOK(vt::ctx::DSSetConstantBuffers1, (&SetCBs1<c::DSSetConstantBuffers1, Stage::DS>));
        }
#undef TT2VR_HOOK
        TT2VR_LOG_INFO("{} context hooks: {} installed, {} shared with the immediate context's vtable",
            V == 0 ? "immediate" : "deferred", created, shared);
    }
};

bool HasContext1(ID3D11DeviceContext* ctx)
{
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> ctx1;
    return SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)));
}

} // namespace

void SetPreDrawCallback(PreDrawCallback callback, int slot)
{
    if (slot >= 0 && slot < kPreDrawSlots) {
        g_preDraw[slot].store(callback, std::memory_order_release);
    }
}

void InstallContextHooks(ID3D11Device* device, ID3D11DeviceContext* immediate)
{
    if (!device || !immediate) {
        TT2VR_LOG_ERROR("context hooks: no device/context; frame trace and texture dump disabled");
        return;
    }
    H<0>::Install(*reinterpret_cast<void***>(immediate), HasContext1(immediate));
    // A throwaway deferred context gives us the deferred vtable (the game creates its own right after).
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> deferred;
    const HRESULT hr = device->CreateDeferredContext(0, &deferred);
    if (FAILED(hr)) {
        TT2VR_LOG_WARN("CreateDeferredContext failed (0x{:08x}); deferred-context calls may be missing from traces",
            static_cast<unsigned>(hr));
        return;
    }
    H<1>::Install(*reinterpret_cast<void***>(deferred.Get()), HasContext1(deferred.Get()));
}

} // namespace d3d
