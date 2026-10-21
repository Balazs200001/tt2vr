// vtable_check.cpp - compile-time check of d3d/vtable.hpp against the SDK's C interface layout.
// This translation unit only contains static_asserts; CINTERFACE gives us the Vtbl structs.
#define CINTERFACE
#define D3D11_NO_HELPERS
#include <d3d11_1.h>
#include <dxgi.h>

#include <cstddef>

#include "d3d/vtable.hpp"

#define TT2VR_CHECK_SLOT(vtbl, ns, method) \
    static_assert(offsetof(vtbl, method) / sizeof(void*) == d3d::vt::ns::method, #vtbl "::" #method)

TT2VR_CHECK_SLOT(ID3D11DeviceVtbl, device, CreateVertexShader);
TT2VR_CHECK_SLOT(ID3D11DeviceVtbl, device, CreateGeometryShader);
TT2VR_CHECK_SLOT(ID3D11DeviceVtbl, device, CreateGeometryShaderWithStreamOutput);
TT2VR_CHECK_SLOT(ID3D11DeviceVtbl, device, CreatePixelShader);
TT2VR_CHECK_SLOT(ID3D11DeviceVtbl, device, CreateHullShader);
TT2VR_CHECK_SLOT(ID3D11DeviceVtbl, device, CreateDomainShader);
TT2VR_CHECK_SLOT(ID3D11DeviceVtbl, device, CreateComputeShader);

#define TT2VR_CHECK_CTX(method) TT2VR_CHECK_SLOT(ID3D11DeviceContextVtbl, ctx, method)
TT2VR_CHECK_CTX(VSSetConstantBuffers);
TT2VR_CHECK_CTX(PSSetShaderResources);
TT2VR_CHECK_CTX(PSSetShader);
TT2VR_CHECK_CTX(VSSetShader);
TT2VR_CHECK_CTX(DrawIndexed);
TT2VR_CHECK_CTX(Draw);
TT2VR_CHECK_CTX(Map);
TT2VR_CHECK_CTX(Unmap);
TT2VR_CHECK_CTX(PSSetConstantBuffers);
TT2VR_CHECK_CTX(DrawIndexedInstanced);
TT2VR_CHECK_CTX(DrawInstanced);
TT2VR_CHECK_CTX(GSSetConstantBuffers);
TT2VR_CHECK_CTX(GSSetShader);
TT2VR_CHECK_CTX(VSSetShaderResources);
TT2VR_CHECK_CTX(GSSetShaderResources);
TT2VR_CHECK_CTX(OMSetRenderTargets);
TT2VR_CHECK_CTX(OMSetRenderTargetsAndUnorderedAccessViews);
TT2VR_CHECK_CTX(OMSetBlendState);
TT2VR_CHECK_CTX(OMSetDepthStencilState);
TT2VR_CHECK_CTX(DrawAuto);
TT2VR_CHECK_CTX(DrawIndexedInstancedIndirect);
TT2VR_CHECK_CTX(DrawInstancedIndirect);
TT2VR_CHECK_CTX(Dispatch);
TT2VR_CHECK_CTX(DispatchIndirect);
TT2VR_CHECK_CTX(RSSetState);
TT2VR_CHECK_CTX(RSSetViewports);
TT2VR_CHECK_CTX(CopySubresourceRegion);
TT2VR_CHECK_CTX(CopyResource);
TT2VR_CHECK_CTX(UpdateSubresource);
TT2VR_CHECK_CTX(ClearRenderTargetView);
TT2VR_CHECK_CTX(ClearUnorderedAccessViewUint);
TT2VR_CHECK_CTX(ClearUnorderedAccessViewFloat);
TT2VR_CHECK_CTX(ClearDepthStencilView);
TT2VR_CHECK_CTX(GenerateMips);
TT2VR_CHECK_CTX(ResolveSubresource);
TT2VR_CHECK_CTX(ExecuteCommandList);
TT2VR_CHECK_CTX(HSSetShaderResources);
TT2VR_CHECK_CTX(HSSetShader);
TT2VR_CHECK_CTX(HSSetConstantBuffers);
TT2VR_CHECK_CTX(DSSetShaderResources);
TT2VR_CHECK_CTX(DSSetShader);
TT2VR_CHECK_CTX(DSSetConstantBuffers);
TT2VR_CHECK_CTX(CSSetShaderResources);
TT2VR_CHECK_CTX(CSSetUnorderedAccessViews);
TT2VR_CHECK_CTX(CSSetShader);
TT2VR_CHECK_CTX(CSSetConstantBuffers);
TT2VR_CHECK_CTX(ClearState);
TT2VR_CHECK_CTX(FinishCommandList);

#define TT2VR_CHECK_CTX1(method) TT2VR_CHECK_SLOT(ID3D11DeviceContext1Vtbl, ctx, method)
TT2VR_CHECK_CTX1(VSSetConstantBuffers1);
TT2VR_CHECK_CTX1(HSSetConstantBuffers1);
TT2VR_CHECK_CTX1(DSSetConstantBuffers1);
TT2VR_CHECK_CTX1(GSSetConstantBuffers1);
TT2VR_CHECK_CTX1(PSSetConstantBuffers1);
TT2VR_CHECK_CTX1(CSSetConstantBuffers1);

TT2VR_CHECK_SLOT(IDXGISwapChainVtbl, swapchain, Present);
TT2VR_CHECK_SLOT(IDXGISwapChainVtbl, swapchain, ResizeBuffers);
