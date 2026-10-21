#pragma once

#include <d3d11.h>

#include <string>

namespace d3d {

// "R16G16B16A16_FLOAT"; "fmt<n>" for values outside the table.
const char* FormatName(DXGI_FORMAT format);

// Typed format for reading back a view's data (depth formats map to their colour twin,
// e.g. D32_FLOAT -> R32_FLOAT). Returns `format` unchanged when no mapping is needed.
DXGI_FORMAT ReadableFormat(DXGI_FORMAT format);

// The game's debug name (WKPDID_D3DDebugObjectName), or "" if none.
std::string DebugName(ID3D11DeviceChild* object);

// One-line descriptions; must be called while the object is alive.
std::string DescribeResource(ID3D11Resource* resource);
std::string DescribeView(ID3D11View* view); // view type, view format, subresource range (not the resource)
std::string DescribeBlendState(ID3D11BlendState* state); // render target 0 only, plus the independent-blend flag
std::string DescribeDepthStencilState(ID3D11DepthStencilState* state);
std::string DescribeRasterizerState(ID3D11RasterizerState* state);

} // namespace d3d
