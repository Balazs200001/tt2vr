#pragma once

// COM vtable slot indices for the interfaces we hook. Checked at compile time against the SDK's
// C interface layout in vtable_check.cpp, so a wrong index fails the build instead of hooking the
// wrong function.
namespace d3d::vt {

namespace device { // ID3D11Device
enum : int {
    CreateVertexShader = 12,
    CreateGeometryShader = 13,
    CreateGeometryShaderWithStreamOutput = 14,
    CreatePixelShader = 15,
    CreateHullShader = 16,
    CreateDomainShader = 17,
    CreateComputeShader = 18,
};
} // namespace device

namespace ctx { // ID3D11DeviceContext / ID3D11DeviceContext1
enum : int {
    VSSetConstantBuffers = 7,
    PSSetShaderResources = 8,
    PSSetShader = 9,
    VSSetShader = 11,
    DrawIndexed = 12,
    Draw = 13,
    Map = 14,
    Unmap = 15,
    PSSetConstantBuffers = 16,
    DrawIndexedInstanced = 20,
    DrawInstanced = 21,
    GSSetConstantBuffers = 22,
    GSSetShader = 23,
    VSSetShaderResources = 25,
    GSSetShaderResources = 31,
    OMSetRenderTargets = 33,
    OMSetRenderTargetsAndUnorderedAccessViews = 34,
    OMSetDepthStencilState = 36,
    DrawAuto = 38,
    DrawIndexedInstancedIndirect = 39,
    DrawInstancedIndirect = 40,
    Dispatch = 41,
    DispatchIndirect = 42,
    RSSetState = 43,
    RSSetViewports = 44,
    CopySubresourceRegion = 46,
    CopyResource = 47,
    UpdateSubresource = 48,
    ClearRenderTargetView = 50,
    ClearUnorderedAccessViewUint = 51,
    ClearUnorderedAccessViewFloat = 52,
    ClearDepthStencilView = 53,
    GenerateMips = 54,
    ResolveSubresource = 57,
    ExecuteCommandList = 58,
    HSSetShaderResources = 59,
    HSSetShader = 60,
    HSSetConstantBuffers = 62,
    DSSetShaderResources = 63,
    DSSetShader = 64,
    DSSetConstantBuffers = 66,
    CSSetShaderResources = 67,
    CSSetUnorderedAccessViews = 68,
    CSSetShader = 69,
    CSSetConstantBuffers = 71,
    ClearState = 110,
    FinishCommandList = 114,
    // ID3D11DeviceContext1
    VSSetConstantBuffers1 = 119,
    HSSetConstantBuffers1 = 120,
    DSSetConstantBuffers1 = 121,
    GSSetConstantBuffers1 = 122,
    PSSetConstantBuffers1 = 123,
    CSSetConstantBuffers1 = 124,
};
} // namespace ctx

namespace swapchain { // IDXGISwapChain
enum : int {
    Present = 8,
    ResizeBuffers = 13,
};
} // namespace swapchain

} // namespace d3d::vt
