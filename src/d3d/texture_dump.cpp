// texture_dump.cpp - per-pass snapshots of the textures one frame writes, saved to captures/.
//
// Rules (one dump = the frame between Present N and Present N+1):
// - A target becomes "dirty" when it is written: draws write the bound RTVs/DSV/OM UAVs, dispatches the
//   bound CS UAVs, clears their view, copies/resolves their destination (only when the source is a GPU
//   target, so streaming uploads are ignored). Writes recorded on a deferred context become dirty when
//   their command list is executed on the immediate context.
// - A dirty target is snapshotted (CopyResource into a private texture, on the immediate context, so in
//   GPU order) when the immediate context explicitly binds a new OM/CS output set that no longer contains
//   it, and at the end of the frame. Each snapshot is therefore the content at the end of one pass.
// - The back buffer is snapshotted last, before our overlay is drawn.
// - Only 2D textures are saved. 8-bit UNORM colour, R10G10B10A2, R8 and R16 go to PNG; everything else to
//   DDS plus a .preview.png (HDR colour: Reinhard + gamma 2.2; other formats: per-channel min..max).
#include "d3d/texture_dump.hpp"

#include "core/config.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "d3d/describe.hpp"
#include "d3d/frame_trace.hpp"
#include "d3d/internal.hpp"

#include <DirectXPackedVector.h>
#include <ScreenGrab.h>
#include <spdlog/fmt/fmt.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace d3d::dump {
namespace {

using Microsoft::WRL::ComPtr;

struct Target {
    ComPtr<ID3D11Resource> resource;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // typed, readable
    const char* kind = "rtv";
};

struct Bindings {
    ComPtr<ID3D11RenderTargetView> rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11UnorderedAccessView> omUavs[D3D11_1_UAV_SLOT_COUNT];
    ComPtr<ID3D11UnorderedAccessView> csUavs[D3D11_1_UAV_SLOT_COUNT];
    std::vector<Target> written; // deferred contexts only: writes not yet attached to a command list
};

struct Snapshot {
    int seq = 0;
    ComPtr<ID3D11Texture2D> copy;
    std::string kind;
    std::string trigger;
    std::string description;
    const void* source = nullptr;
};

struct State {
    std::mutex mutex;
    std::uint64_t frame = 0;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* immediate = nullptr;
    std::unordered_map<ID3D11DeviceContext*, Bindings> contexts;
    std::unordered_map<ID3D11CommandList*, std::vector<Target>> lists;
    std::vector<Target> dirty;
    std::vector<Snapshot> snapshots;
    int limit = 64;
    int skipped = 0;
    std::vector<std::string> notes;
};

State g_state;
bool g_requested = false; // present thread only
std::vector<DoneFn> g_done; // present thread only

bool IsTexture2D(ID3D11Resource* resource)
{
    D3D11_RESOURCE_DIMENSION dim{};
    resource->GetType(&dim);
    return dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D;
}

bool IsGpuTarget(ID3D11Resource* resource)
{
    if (!resource || !IsTexture2D(resource)) {
        return false;
    }
    D3D11_TEXTURE2D_DESC d{};
    static_cast<ID3D11Texture2D*>(resource)->GetDesc(&d);
    return (d.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS)) != 0;
}

// Typed twin of a typeless format (best guess for copies whose destination has no view).
DXGI_FORMAT TypedFormat(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
    default: return ReadableFormat(f);
    }
}

template <typename View>
DXGI_FORMAT ViewFormat(View* view)
{
    typename std::conditional_t<std::is_same_v<View, ID3D11RenderTargetView>, D3D11_RENDER_TARGET_VIEW_DESC,
        std::conditional_t<std::is_same_v<View, ID3D11DepthStencilView>, D3D11_DEPTH_STENCIL_VIEW_DESC,
            D3D11_UNORDERED_ACCESS_VIEW_DESC>> desc{};
    view->GetDesc(&desc);
    return ReadableFormat(desc.Format);
}

void AddUnique(std::vector<Target>& set, Target t)
{
    if (!t.resource || !IsTexture2D(t.resource.Get())) {
        return;
    }
    for (const auto& existing : set) {
        if (existing.resource.Get() == t.resource.Get()) {
            return;
        }
    }
    set.push_back(std::move(t));
}

template <typename View>
Target TargetOf(View* view, const char* kind)
{
    Target t;
    t.kind = kind;
    view->GetResource(&t.resource);
    t.format = ViewFormat(view);
    if (t.format == DXGI_FORMAT_UNKNOWN && t.resource && IsTexture2D(t.resource.Get())) {
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(t.resource.Get())->GetDesc(&d);
        t.format = TypedFormat(d.Format);
    }
    return t;
}

// Caller holds the mutex. Writes on the immediate context are dirty at once; deferred ones wait for Execute.
void MarkWritten(ID3D11DeviceContext* ctx, Target t)
{
    if (ctx == g_state.immediate) {
        AddUnique(g_state.dirty, std::move(t));
    }
    else {
        AddUnique(g_state.contexts[ctx].written, std::move(t));
    }
}

bool BoundOnImmediate(ID3D11Resource* resource)
{
    const auto it = g_state.contexts.find(g_state.immediate);
    if (it == g_state.contexts.end()) {
        return false;
    }
    const Bindings& b = it->second;
    auto same = [&](ID3D11View* v) {
        if (!v) {
            return false;
        }
        ComPtr<ID3D11Resource> r;
        v->GetResource(&r);
        return r.Get() == resource;
    };
    for (const auto& v : b.rtvs) {
        if (same(v.Get())) {
            return true;
        }
    }
    if (same(b.dsv.Get())) {
        return true;
    }
    for (const auto& v : b.omUavs) {
        if (same(v.Get())) {
            return true;
        }
    }
    for (const auto& v : b.csUavs) {
        if (same(v.Get())) {
            return true;
        }
    }
    return false;
}

// Caller holds the mutex; immediate context / present thread only.
void TakeSnapshot(const Target& t, const std::string& trigger)
{
    if (static_cast<int>(g_state.snapshots.size()) >= g_state.limit) {
        ++g_state.skipped;
        return;
    }
    ComPtr<ID3D11Texture2D> src;
    if (FAILED(t.resource.As(&src))) {
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    src->GetDesc(&desc);
    const bool msaa = desc.SampleDesc.Count > 1;
    D3D11_TEXTURE2D_DESC copyDesc = desc;
    copyDesc.Format = t.format != DXGI_FORMAT_UNKNOWN ? t.format : desc.Format;
    copyDesc.Usage = D3D11_USAGE_DEFAULT;
    copyDesc.BindFlags = 0;
    copyDesc.CPUAccessFlags = 0;
    copyDesc.MiscFlags = 0;
    if (msaa) {
        // Resolve subresource 0 into a single-sample copy (multisampled textures need a bind flag).
        copyDesc.SampleDesc = {1, 0};
        copyDesc.MipLevels = 1;
        copyDesc.ArraySize = 1;
    }
    ComPtr<ID3D11Texture2D> copy;
    HRESULT hr = g_state.device->CreateTexture2D(&copyDesc, nullptr, &copy);
    if (FAILED(hr) && !msaa && copyDesc.Format != desc.Format) {
        copyDesc.Format = desc.Format; // e.g. R24_UNORM_X8_TYPELESS not creatable: keep the raw family
        hr = g_state.device->CreateTexture2D(&copyDesc, nullptr, &copy);
    }
    if (FAILED(hr)) {
        g_state.notes.push_back(fmt::format("snapshot of {} failed: CreateTexture2D 0x{:08x}",
            DescribeResource(src.Get()), static_cast<unsigned>(hr)));
        ++g_state.skipped;
        return;
    }
    {
        InternalScope internal;
        if (msaa) {
            g_state.immediate->ResolveSubresource(copy.Get(), 0, src.Get(), 0, copyDesc.Format);
        }
        else {
            g_state.immediate->CopyResource(copy.Get(), src.Get());
        }
    }
    Snapshot s;
    s.seq = static_cast<int>(g_state.snapshots.size());
    s.copy = copy;
    s.kind = t.kind;
    s.trigger = trigger;
    s.description = DescribeResource(src.Get());
    s.source = src.Get();
    if (trace::Active()) {
        trace::Line(g_state.immediate, fmt::format("[dump] snapshot {:03} {} {} ({})", s.seq, t.kind,
            trace::Res(src.Get()), trigger));
    }
    g_state.snapshots.push_back(std::move(s));
}

// Caller holds the mutex. Snapshots every dirty target the immediate context no longer outputs to.
void FlushUnbound(const char* trigger)
{
    std::vector<Target> keep;
    for (auto& t : g_state.dirty) {
        if (BoundOnImmediate(t.resource.Get())) {
            keep.push_back(std::move(t));
        }
        else {
            TakeSnapshot(t, trigger);
        }
    }
    g_state.dirty = std::move(keep);
}

std::string Sanitize(std::string s)
{
    for (char& c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) {
            c = '_';
        }
    }
    return s.substr(0, 48);
}

bool IsPngFormat(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_A8_UNORM:
    case DXGI_FORMAT_R16_UNORM:
        return true;
    default:
        return false;
    }
}

bool IsGrayFormat(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R8_UNORM || f == DXGI_FORMAT_A8_UNORM || f == DXGI_FORMAT_R16_UNORM;
}

} // namespace

bool WritePngBgr(const std::filesystem::path& path, UINT width, UINT height, std::vector<std::uint8_t>& bgr)
{
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(width, height))) {
        return false;
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat24bppBGR) {
        return false;
    }
    return SUCCEEDED(frame->WritePixels(height, width * 3, static_cast<UINT>(bgr.size()), bgr.data())) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

namespace {

// Decodes texel channels to floats. Returns the channel count, 0 if the format has no preview.
int DecodeTexel(DXGI_FORMAT f, const std::uint8_t* p, float out[4], bool& hdrColour)
{
    using namespace DirectX::PackedVector;
    hdrColour = false;
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        const auto* h = reinterpret_cast<const HALF*>(p);
        for (int i = 0; i < 3; ++i) {
            out[i] = XMConvertHalfToFloat(h[i]);
        }
        hdrColour = true;
        return 3;
    }
    case DXGI_FORMAT_R32G32B32A32_FLOAT: {
        std::memcpy(out, p, 3 * sizeof(float));
        hdrColour = true;
        return 3;
    }
    case DXGI_FORMAT_R11G11B10_FLOAT: {
        XMFLOAT3PK pk;
        std::memcpy(&pk.v, p, 4);
        DirectX::XMFLOAT3 v;
        DirectX::XMStoreFloat3(&v, XMLoadFloat3PK(&pk));
        out[0] = v.x;
        out[1] = v.y;
        out[2] = v.z;
        hdrColour = true;
        return 3;
    }
    case DXGI_FORMAT_R16G16_FLOAT: {
        const auto* h = reinterpret_cast<const HALF*>(p);
        out[0] = XMConvertHalfToFloat(h[0]);
        out[1] = XMConvertHalfToFloat(h[1]);
        return 2;
    }
    case DXGI_FORMAT_R32G32_FLOAT:
        std::memcpy(out, p, 2 * sizeof(float));
        return 2;
    case DXGI_FORMAT_R16G16_UNORM: {
        const auto* u = reinterpret_cast<const std::uint16_t*>(p);
        out[0] = u[0] / 65535.0f;
        out[1] = u[1] / 65535.0f;
        return 2;
    }
    case DXGI_FORMAT_R16G16_SNORM: {
        const auto* s = reinterpret_cast<const std::int16_t*>(p);
        out[0] = s[0] / 32767.0f;
        out[1] = s[1] / 32767.0f;
        return 2;
    }
    case DXGI_FORMAT_R8G8_UNORM:
        out[0] = p[0] / 255.0f;
        out[1] = p[1] / 255.0f;
        return 2;
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        std::memcpy(out, p, sizeof(float));
        return 1;
    case DXGI_FORMAT_R16_FLOAT:
        out[0] = XMConvertHalfToFloat(*reinterpret_cast<const HALF*>(p));
        return 1;
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS: {
        std::uint32_t v;
        std::memcpy(&v, p, 4);
        out[0] = (v & 0xffffff) / 16777215.0f;
        return 1;
    }
    case DXGI_FORMAT_R32_UINT: {
        std::uint32_t v;
        std::memcpy(&v, p, 4);
        out[0] = static_cast<float>(v);
        return 1;
    }
    default:
        return 0;
    }
}

std::size_t TexelBytes(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return 16;
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R8G8_UNORM:
        return 2;
    default:
        return 4;
    }
}

// Viewable 8-bit preview of a float/depth/two-channel texture (mip 0, slice 0).
bool WritePreview(ID3D11Texture2D* tex, const std::filesystem::path& path, std::string& range)
{
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    float probe[4] = {};
    bool hdr = false;
    std::uint8_t zero[16] = {};
    if (desc.SampleDesc.Count > 1 || DecodeTexel(desc.Format, zero, probe, hdr) == 0) {
        return false;
    }
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(g_state.device->CreateTexture2D(&sd, nullptr, &staging))) {
        return false;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    {
        InternalScope internal;
        g_state.immediate->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex, 0, nullptr);
        if (FAILED(g_state.immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            return false;
        }
    }
    const UINT w = desc.Width;
    const UINT h = desc.Height;
    const std::size_t texel = TexelBytes(desc.Format);
    std::vector<float> values(static_cast<std::size_t>(w) * h * 3, 0.0f);
    int channels = 0;
    for (UINT y = 0; y < h; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
        for (UINT x = 0; x < w; ++x) {
            float v[4] = {};
            channels = DecodeTexel(desc.Format, row + x * texel, v, hdr);
            float* dst = &values[(static_cast<std::size_t>(y) * w + x) * 3];
            for (int c = 0; c < 3; ++c) {
                dst[c] = c < channels ? v[c] : 0.0f;
            }
        }
    }
    {
        InternalScope internal;
        g_state.immediate->Unmap(staging.Get(), 0);
    }
    float lo[3] = {INFINITY, INFINITY, INFINITY};
    float hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (std::size_t i = 0; i < values.size(); ++i) {
        const float v = values[i];
        if (std::isfinite(v)) {
            lo[i % 3] = std::min(lo[i % 3], v);
            hi[i % 3] = std::max(hi[i % 3], v);
        }
    }
    range.clear();
    for (int c = 0; c < channels; ++c) {
        range += fmt::format("{}{}=[{:.6g}, {:.6g}]", c ? " " : "", "rgb"[c], lo[c], hi[c]);
    }
    std::vector<std::uint8_t> bgr(static_cast<std::size_t>(w) * h * 3);
    for (std::size_t px = 0; px < static_cast<std::size_t>(w) * h; ++px) {
        std::uint8_t rgb[3] = {};
        for (int c = 0; c < 3; ++c) {
            float v = values[px * 3 + (channels == 1 ? 0 : c)];
            if (!std::isfinite(v) || (channels == 2 && c == 2)) {
                v = 0.0f;
            }
            else if (hdr) {
                v = std::pow(std::max(v, 0.0f) / (1.0f + std::max(v, 0.0f)), 1.0f / 2.2f);
            }
            else {
                const int ch = channels == 1 ? 0 : c;
                v = hi[ch] > lo[ch] ? (v - lo[ch]) / (hi[ch] - lo[ch]) : 0.0f;
            }
            rgb[c] = static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        bgr[px * 3 + 0] = rgb[2];
        bgr[px * 3 + 1] = rgb[1];
        bgr[px * 3 + 2] = rgb[0];
    }
    return WritePngBgr(path, w, h, bgr);
}

std::string SaveAll(std::uint64_t frame)
{
    const auto dir = core::CaptureDir();
    const auto indexPath = dir / fmt::format("{}_dump.txt", frame);
    std::ofstream index(indexPath, std::ios::trunc);
    index << fmt::format("# tt2vr texture dump, frame {}: {} snapshots, {} skipped (limit {})\n", frame,
        g_state.snapshots.size(), g_state.skipped, g_state.limit);
    index << "# seq kind trigger | source resource | files\n";
    int saved = 0;
    InternalScope internal; // ScreenGrab's copies go through our hooks
    for (const auto& s : g_state.snapshots) {
        D3D11_TEXTURE2D_DESC d{};
        s.copy->GetDesc(&d);
        std::string base = fmt::format("{}_mono_{:03}_{}_{}x{}_{}", frame, s.seq, s.kind, d.Width, d.Height,
            FormatName(d.Format));
        const auto srcName = s.description.find('"');
        if (srcName != std::string::npos) {
            base += "_" + Sanitize(s.description.substr(srcName + 1, s.description.rfind('"') - srcName - 1));
        }
        std::string files;
        HRESULT hr = E_FAIL;
        if (IsPngFormat(d.Format)) {
            const auto file = dir / (base + ".png");
            hr = DirectX::SaveWICTextureToFile(g_state.immediate, s.copy.Get(), GUID_ContainerFormatPng,
                file.c_str(), IsGrayFormat(d.Format) ? nullptr : &GUID_WICPixelFormat24bppBGR);
            files = file.filename().string();
        }
        else {
            const auto file = dir / (base + ".dds");
            hr = DirectX::SaveDDSTextureToFile(g_state.immediate, s.copy.Get(), file.c_str());
            files = file.filename().string();
            std::string range;
            const auto preview = dir / (base + ".preview.png");
            if (WritePreview(s.copy.Get(), preview, range)) {
                files += " " + preview.filename().string() + " " + range;
            }
        }
        if (SUCCEEDED(hr)) {
            ++saved;
        }
        else {
            files += fmt::format(" (save FAILED 0x{:08x})", static_cast<unsigned>(hr));
        }
        index << fmt::format("{:03} {} {} | @{} {} | {}\n", s.seq, s.kind, s.trigger, s.source, s.description, files);
    }
    for (const auto& note : g_state.notes) {
        index << "# " << note << "\n";
    }
    return fmt::format("dump ok {} ({} of {} snapshots saved, {} skipped)", indexPath.string(), saved,
        g_state.snapshots.size(), g_state.skipped);
}

void ResetState()
{
    g_state.contexts.clear();
    g_state.lists.clear();
    g_state.dirty.clear();
    g_state.snapshots.clear();
    g_state.notes.clear();
    g_state.skipped = 0;
}

void ClearBindings(Bindings& b)
{
    for (auto& v : b.rtvs) {
        v.Reset();
    }
    b.dsv.Reset();
    for (auto& v : b.omUavs) {
        v.Reset();
    }
    for (auto& v : b.csUavs) {
        v.Reset();
    }
}

} // namespace

void Request(DoneFn onDone)
{
    g_requested = true;
    g_done.push_back(std::move(onDone));
}

bool Pending()
{
    return g_requested || Active();
}

void Begin(std::uint64_t frame, ID3D11Device* device, ID3D11DeviceContext* immediate)
{
    if (!g_requested || Active()) {
        return;
    }
    g_requested = false;
    {
        std::lock_guard lock(g_state.mutex);
        ResetState();
        g_state.frame = frame;
        g_state.device = device;
        g_state.immediate = immediate;
        g_state.limit = core::GetConfig().capture.max_textures;
    }
    detail::g_active.store(true);
    TT2VR_LOG_INFO("dump: recording frame {}", frame);
}

void End(std::uint64_t frame, IDXGISwapChain* swapChain)
{
    if (!Active()) {
        return;
    }
    detail::g_active.store(false);
    std::string result;
    {
        std::lock_guard lock(g_state.mutex);
        try {
            for (auto& t : g_state.dirty) {
                TakeSnapshot(t, "end of frame");
            }
            g_state.dirty.clear();
            ComPtr<ID3D11Texture2D> back;
            if (swapChain && SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
                ++g_state.limit; // the back buffer never counts against the limit
                D3D11_TEXTURE2D_DESC d{};
                back->GetDesc(&d);
                TakeSnapshot(Target{back, TypedFormat(d.Format), "backbuffer"}, "before Present");
            }
            HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            result = SaveAll(frame);
            if (SUCCEEDED(co)) {
                CoUninitialize();
            }
        }
        catch (...) {
            result = "dump FAILED: exception";
        }
        ResetState();
    }
    TT2VR_LOG_INFO("{}", result);
    for (auto& done : g_done) {
        if (done) {
            done(result);
        }
    }
    g_done.clear();
}

void OnSetTargets(ID3D11DeviceContext* ctx, UINT numRtvs, ID3D11RenderTargetView* const* rtvs,
    ID3D11DepthStencilView* dsv, bool setUavs, UINT uavStart, UINT numUavs, ID3D11UnorderedAccessView* const* uavs)
{
    std::lock_guard lock(g_state.mutex);
    Bindings& b = g_state.contexts[ctx];
    if (numRtvs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            b.rtvs[i] = (rtvs && i < numRtvs) ? rtvs[i] : nullptr;
        }
        b.dsv = dsv;
    }
    if (setUavs && numUavs != D3D11_KEEP_UNORDERED_ACCESS_VIEWS) {
        for (UINT i = 0; i < D3D11_1_UAV_SLOT_COUNT; ++i) {
            b.omUavs[i] = (uavs && i >= uavStart && i - uavStart < numUavs) ? uavs[i - uavStart] : nullptr;
        }
    }
    if (ctx == g_state.immediate) {
        FlushUnbound("pass end: OM rebind");
    }
}

void OnSetCsUavs(ID3D11DeviceContext* ctx, UINT start, UINT num, ID3D11UnorderedAccessView* const* uavs)
{
    std::lock_guard lock(g_state.mutex);
    Bindings& b = g_state.contexts[ctx];
    for (UINT i = 0; i < num && start + i < D3D11_1_UAV_SLOT_COUNT; ++i) {
        b.csUavs[start + i] = uavs ? uavs[i] : nullptr;
    }
    if (ctx == g_state.immediate) {
        FlushUnbound("pass end: CS UAV rebind");
    }
}

void OnDraw(ID3D11DeviceContext* ctx)
{
    std::lock_guard lock(g_state.mutex);
    Bindings& b = g_state.contexts[ctx];
    for (const auto& v : b.rtvs) {
        if (v) {
            MarkWritten(ctx, TargetOf(v.Get(), "rtv"));
        }
    }
    if (b.dsv && core::GetConfig().capture.include_depth) {
        MarkWritten(ctx, TargetOf(b.dsv.Get(), "dsv"));
    }
    for (const auto& v : b.omUavs) {
        if (v) {
            MarkWritten(ctx, TargetOf(v.Get(), "uav"));
        }
    }
}

void OnDispatch(ID3D11DeviceContext* ctx)
{
    std::lock_guard lock(g_state.mutex);
    Bindings& b = g_state.contexts[ctx];
    for (const auto& v : b.csUavs) {
        if (v) {
            MarkWritten(ctx, TargetOf(v.Get(), "uav"));
        }
    }
}

void OnClear(ID3D11DeviceContext* ctx, ID3D11View* view)
{
    if (!view) {
        return;
    }
    std::lock_guard lock(g_state.mutex);
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&rtv)))) {
        MarkWritten(ctx, TargetOf(rtv.Get(), "rtv"));
    }
    else if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&dsv)))) {
        if (core::GetConfig().capture.include_depth) {
            MarkWritten(ctx, TargetOf(dsv.Get(), "dsv"));
        }
    }
    else if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&uav)))) {
        MarkWritten(ctx, TargetOf(uav.Get(), "uav"));
    }
}

void OnCopy(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src)
{
    if (!dst || !IsTexture2D(dst) || !IsGpuTarget(src)) {
        return;
    }
    D3D11_TEXTURE2D_DESC d{};
    static_cast<ID3D11Texture2D*>(dst)->GetDesc(&d);
    std::lock_guard lock(g_state.mutex);
    MarkWritten(ctx, Target{dst, TypedFormat(d.Format), "copy"});
}

void OnClearState(ID3D11DeviceContext* ctx)
{
    std::lock_guard lock(g_state.mutex);
    ClearBindings(g_state.contexts[ctx]);
}

void OnFinishList(ID3D11DeviceContext* ctx, ID3D11CommandList* list, BOOL restore)
{
    std::lock_guard lock(g_state.mutex);
    Bindings& b = g_state.contexts[ctx];
    if (list) {
        g_state.lists[list] = std::move(b.written);
    }
    b.written.clear();
    if (!restore) {
        ClearBindings(b);
    }
}

void OnExecuteList(ID3D11DeviceContext* immediate, ID3D11CommandList* list, BOOL restore)
{
    std::lock_guard lock(g_state.mutex);
    if (auto it = g_state.lists.find(list); it != g_state.lists.end()) {
        for (auto& t : it->second) {
            AddUnique(g_state.dirty, std::move(t));
        }
        g_state.lists.erase(it);
    }
    if (!restore) {
        ClearBindings(g_state.contexts[immediate]); // state reset, not an explicit rebind: no snapshot here
    }
}

} // namespace d3d::dump
