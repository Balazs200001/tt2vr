#include "game/lens_mask.hpp"

#include "core/config.hpp"
#include "core/hooks.hpp"
#include "core/log.hpp"
#include "d3d/internal.hpp"
#include "game/camera.hpp"
#include "game/engine_layout.hpp"
#include "game/signatures.hpp"
#include "game/split_view.hpp"
#include "stereo/math/lens_mask.hpp"
#include "xr/session.hpp"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <windows.h>

namespace game {
namespace {

using namespace layout;
using Microsoft::WRL::ComPtr;

// RenderLayerDX11 pass executor FUN_14094aab0 (this, ContextWrapper*, DrawPass*): sets the pass up (viewport, the
// clear, render targets, states), then replays its draw commands. ContextWrapper+0 = the ID3D11DeviceContext the job
// records on (renderer.md 13).
using ExecutePassFn = void (*)(void* layer, void* wrapper, void* pass);
ExecutePassFn g_execute = nullptr;
bool g_installed = false;

constexpr const char* kClearPassName = "discard framebuffer & clear stencil & depth"; // RenderLayer::ClearRT
constexpr const char* kClearGBufferName = "Clear GBuffer";
constexpr const char* kOceanName = "Ocean"; // draws after 'Clear GBuffer'; SSR would trace its water in masked pixels
constexpr std::ptrdiff_t kPassName = 0x18; // DrawPassDesc embedded at pass+0x18, name first
constexpr std::ptrdiff_t kPassClearFlags = 0x68; // 1 depth, 2 colour, 4 stencil, 8 discard colour, 0x10 discard depth
constexpr std::ptrdiff_t kPassTarget = 0x70; // RenderTarget*
constexpr std::uint32_t kDepthClearFlags = 0xd; // RenderGBuffer's clear: discard colour, clear stencil and depth
constexpr std::ptrdiff_t kTargetDsv = 0x118; // RenderTarget: writable ID3D11DepthStencilView*
constexpr std::ptrdiff_t kWrapperContext = 0x0;

// The mesh at depth 1 (after the clear) / 0 (before Clear GBuffer) / 1 before and 0 after the ocean pass.
enum Step : int { kBlock = 0, kSky = 1, kAround = 2 };

// A recorded pass to mask around. Each keeps its own depth view: a view can clear more than one target with the
// same flags. A block entry (a depth clear) only draws once that eye's 'Clear GBuffer' was recorded right after it
// (confirmed): only RenderGBuffer's clear is masked, never another target's.
struct Entry {
    const void* pass;
    int eye;
    int step;
    ID3D11DepthStencilView* dsv;
    UINT width, height;
    std::atomic<bool> confirmed;
};
constexpr int kMaxEntries = 12;
Entry g_entries[kMaxEntries];
std::atomic<int> g_entryCount{0}; // entries are written on the main thread before the count publishes them

struct EyeMesh {
    ComPtr<ID3D11Buffer> vb[2]; // per step: NDC x, y and the step's depth
    UINT vertices = 0;
    unsigned version = ~0u;
    stereo::FovTangents fov{};
    int lastClear = -1; // entry index of the eye's latest recorded depth clear this frame
    int gbuffer = -1; // entry index of the eye's confirmed GBuffer clear this frame
};
EyeMesh g_eye[2];
ID3D11Device* g_device = nullptr;
ComPtr<ID3D11VertexShader> g_vs;
ComPtr<ID3D11InputLayout> g_layout;
ComPtr<ID3D11RasterizerState> g_raster;
ComPtr<ID3D11DepthStencilState> g_depthWrite;
bool g_resourcesFailed = false;
std::atomic<int> g_drawn[2] = {}; // this frame, per depth (block / sky)
std::atomic<int> g_matched{0}, g_executed{0}, g_skipped{0}; // this frame: entries found / passes run / draws skipped
std::uint64_t g_frames = 0;
int g_lastDrawn[2] = {};
bool g_logged = false;
int g_clearLogs = 0;

constexpr char kVsSource[] = "float4 VS(float3 p : POSITION) : SV_Position { return float4(p.xy, p.z, 1); }";

bool CreateResources(ID3D11Device* device)
{
    if (g_vs) {
        return true;
    }
    if (g_resourcesFailed || !device) {
        return false;
    }
    ComPtr<ID3DBlob> code, errors;
    D3D11_INPUT_ELEMENT_DESC element{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE; // the runtime's winding is unspecified
    rd.DepthClipEnable = TRUE;
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (FAILED(D3DCompile(kVsSource, sizeof(kVsSource) - 1, "tt2vr_lens_mask", nullptr, nullptr, "VS", "vs_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)) ||
        FAILED(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_vs)) ||
        FAILED(device->CreateInputLayout(&element, 1, code->GetBufferPointer(), code->GetBufferSize(), &g_layout)) ||
        FAILED(device->CreateRasterizerState(&rd, &g_raster)) ||
        FAILED(device->CreateDepthStencilState(&dd, &g_depthWrite))) {
        TT2VR_LOG_ERROR("lens mask: shader or states failed; off");
        g_vs.Reset();
        g_layout.Reset();
        g_raster.Reset();
        g_depthWrite.Reset();
        g_resourcesFailed = true;
        return false;
    }
    return true;
}

bool SameFov(const stereo::FovTangents& a, const stereo::FovTangents& b)
{
    return std::fabs(a.left - b.left) < 1e-5f && std::fabs(a.right - b.right) < 1e-5f && std::fabs(a.up - b.up) < 1e-5f &&
           std::fabs(a.down - b.down) < 1e-5f;
}

// Main thread (recording): the eye's vertex buffers for the current mesh and projection.
bool UpdateMesh(int eye)
{
    EyeMesh& m = g_eye[eye];
    stereo::FovTangents fov{};
    if (!EyeTangents(eye, fov)) {
        return false;
    }
    const unsigned version = xr::HiddenMeshVersion();
    if (m.vb[0] && m.version == version && SameFov(m.fov, fov)) {
        return true;
    }
    const std::vector<float> ndc = stereo::MaskToNdc(xr::HiddenMesh(eye), fov);
    m.version = version;
    m.fov = fov;
    m.vertices = 0;
    m.vb[0].Reset();
    m.vb[1].Reset();
    if (ndc.size() < 6) {
        return false;
    }
    const UINT count = static_cast<UINT>(ndc.size() / 2);
    for (int step = 0; step < 2; ++step) {
        std::vector<float> data(static_cast<std::size_t>(count) * 3);
        for (UINT i = 0; i < count; ++i) {
            data[i * 3] = ndc[i * 2];
            data[i * 3 + 1] = ndc[i * 2 + 1];
            data[i * 3 + 2] = step == kBlock ? 1.0f : 0.0f;
        }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = static_cast<UINT>(data.size() * sizeof(float));
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        const D3D11_SUBRESOURCE_DATA init{data.data(), 0, 0};
        if (FAILED(g_device->CreateBuffer(&bd, &init, &m.vb[step]))) {
            m.vb[0].Reset();
            m.vb[1].Reset();
            return false;
        }
    }
    m.vertices = count;
    TT2VR_LOG_INFO("lens mask: eye {} mesh {} triangles (frustum {:.3f} {:.3f} {:.3f} {:.3f})", eye, count / 3, fov.left,
        fov.right, fov.up, fov.down);
    return true;
}

// The engine's writable depth view of the cleared target, and its size. SEH: engine data from static RE.
bool TargetDepth(void* pass, ID3D11DepthStencilView*& dsv, UINT& width, UINT& height)
{
    __try {
        if (*reinterpret_cast<const std::uint32_t*>(static_cast<char*>(pass) + kPassClearFlags) != kDepthClearFlags) {
            return false;
        }
        void* target = *reinterpret_cast<void**>(static_cast<char*>(pass) + kPassTarget);
        dsv = target ? *reinterpret_cast<ID3D11DepthStencilView**>(static_cast<char*>(target) + kTargetDsv) : nullptr;
        if (!dsv) {
            return false;
        }
        ID3D11Resource* resource = nullptr;
        dsv->GetResource(&resource);
        if (!resource) {
            return false;
        }
        ID3D11Texture2D* texture = nullptr;
        const bool ok = SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture)));
        resource->Release();
        if (!ok) {
            return false;
        }
        D3D11_TEXTURE2D_DESC d{};
        texture->GetDesc(&d);
        texture->Release();
        width = d.Width;
        height = d.Height;
        return width > 0 && height > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int AddEntry(const void* pass, int eye, int step, ID3D11DepthStencilView* dsv, UINT width, UINT height)
{
    const int n = g_entryCount.load(std::memory_order_relaxed);
    if (n >= kMaxEntries) {
        return -1;
    }
    Entry& e = g_entries[n];
    e.pass = pass;
    e.eye = eye;
    e.step = step;
    e.dsv = dsv;
    e.width = width;
    e.height = height;
    e.confirmed.store(step != kBlock, std::memory_order_relaxed);
    g_entryCount.store(n + 1, std::memory_order_release);
    return n;
}

const Entry* FindEntry(const void* pass)
{
    const int n = g_entryCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        if (g_entries[i].pass == pass) {
            return &g_entries[i];
        }
    }
    return nullptr;
}

// Draws eye `eye`'s mesh at the step's depth into its depth view on `context` (the executing job's context), with
// every piece of state it touches put back (the engine caches its bindings per context).
void DrawMask(ID3D11DeviceContext* context, const Entry& e, bool before)
{
    const EyeMesh& m = g_eye[e.eye];
    const int step = e.step == kAround ? (before ? kBlock : kSky) : e.step;
    if (!context || !e.dsv || !e.confirmed.load(std::memory_order_acquire) || !m.vb[step] || m.vertices == 0) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    d3d::InternalScope internal;
    // Save.
    ComPtr<ID3D11InputLayout> layout;
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, offset = 0;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11HullShader> hs;
    ComPtr<ID3D11DomainShader> ds;
    ComPtr<ID3D11RasterizerState> raster;
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ComPtr<ID3D11DepthStencilView> savedDsv;
    ComPtr<ID3D11DepthStencilState> depthState;
    UINT stencilRef = 0;
    context->IAGetInputLayout(&layout);
    context->IAGetPrimitiveTopology(&topology);
    context->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    context->VSGetShader(&vs, nullptr, nullptr);
    context->PSGetShader(&ps, nullptr, nullptr);
    context->GSGetShader(&gs, nullptr, nullptr);
    context->HSGetShader(&hs, nullptr, nullptr);
    context->DSGetShader(&ds, nullptr, nullptr);
    context->RSGetState(&raster);
    context->RSGetViewports(&viewportCount, viewports);
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &savedDsv);
    context->OMGetDepthStencilState(&depthState, &stencilRef);
    // Draw.
    const UINT ourStride = 3 * sizeof(float), ourOffset = 0;
    ID3D11Buffer* ourVb = m.vb[step].Get();
    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(e.width), static_cast<float>(e.height), 0.0f, 1.0f};
    context->IASetInputLayout(g_layout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetVertexBuffers(0, 1, &ourVb, &ourStride, &ourOffset);
    context->VSSetShader(g_vs.Get(), nullptr, 0);
    context->PSSetShader(nullptr, nullptr, 0); // depth only
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->RSSetState(g_raster.Get());
    context->RSSetViewports(1, &vp);
    context->OMSetRenderTargets(0, nullptr, e.dsv);
    context->OMSetDepthStencilState(g_depthWrite.Get(), 0);
    context->Draw(m.vertices, 0);
    // Restore.
    context->IASetInputLayout(layout.Get());
    context->IASetPrimitiveTopology(topology);
    context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    context->GSSetShader(gs.Get(), nullptr, 0);
    context->HSSetShader(hs.Get(), nullptr, 0);
    context->DSSetShader(ds.Get(), nullptr, 0);
    context->RSSetState(raster.Get());
    context->RSSetViewports(viewportCount, viewports);
    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, savedDsv.Get());
    context->OMSetDepthStencilState(depthState.Get(), stencilRef);
    if (vb) {
        vb->Release();
    }
    for (auto* rtv : rtvs) {
        if (rtv) {
            rtv->Release();
        }
    }
    g_drawn[step].fetch_add(1, std::memory_order_relaxed);
}

ID3D11DeviceContext* ContextOf(void* wrapper)
{
    __try {
        return wrapper ? *reinterpret_cast<ID3D11DeviceContext**>(static_cast<char*>(wrapper) + kWrapperContext) : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Thread: the job thread executing the pass (or the main thread for immediate passes).
void HookedExecute(void* layer, void* wrapper, void* pass)
{
    g_executed.fetch_add(1, std::memory_order_relaxed);
    const Entry* e = g_entryCount.load(std::memory_order_relaxed) > 0 ? FindEntry(pass) : nullptr;
    if (e) {
        g_matched.fetch_add(1, std::memory_order_relaxed);
    }
    if (e && (e->step == kSky || e->step == kAround)) {
        DrawMask(ContextOf(wrapper), *e, true);
    }
    g_execute(layer, wrapper, pass);
    if (e && (e->step == kBlock || e->step == kAround)) {
        DrawMask(ContextOf(wrapper), *e, false);
    }
}

} // namespace

void InstallLensMask()
{
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;
    void* execute = SignatureAddress(Sig::RenderLayerExecuteDrawPass);
    if (!execute || core::hooks::Create("RenderLayerDX11 DrawPass executor (FUN_14094aab0)", execute, &HookedExecute,
                        &g_execute) != core::hooks::Result::Ok) {
        TT2VR_LOG_WARN("lens mask: pass executor not hooked; the visibility mask is off");
        return;
    }
    g_installed = true;
    TT2VR_LOG_INFO("lens mask: hooks installed ([xr] visibility_mask)");
}

void LensMaskLoopBegin(void* renderLoop)
{
    (void)renderLoop;
    if (g_installed && (g_frames++ % 6000) == 1 && g_entryCount.load() > 0) {
        TT2VR_LOG_INFO("lens mask: last frame {} passes recorded, {} executed through the hook, {} matched, drawn {} / {}, "
                       "skipped {}",
            g_entryCount.load(), g_executed.load(), g_matched.load(), g_drawn[0].load(), g_drawn[1].load(),
            g_skipped.load());
    }
    g_executed = 0;
    g_matched = 0;
    g_skipped = 0;
    g_entryCount.store(0, std::memory_order_release);
    for (int step = 0; step < 2; ++step) {
        g_lastDrawn[step] = g_drawn[step].exchange(0, std::memory_order_relaxed);
    }
    g_eye[0].lastClear = g_eye[1].lastClear = -1;
    g_eye[0].gbuffer = g_eye[1].gbuffer = -1;
}

void LensMaskOnRecord(void* pass, int view)
{
    if (!g_installed || !pass || view < 0 || view > 1 || !core::GetConfig().xr.visibility_mask || !SplitViewActive() ||
        !xr::SessionRunning() || xr::HiddenMesh(view).empty()) {
        return;
    }
    const char* name = Read<const char*>(pass, kPassName);
    if (!name) {
        return;
    }
    const int eye = view; // camera index k = eye k (game::SplitView)
    if (std::strcmp(name, kClearPassName) == 0) {
        if (!g_device) {
            ComPtr<ID3D11Device> device;
            ID3D11DepthStencilView* probe = nullptr;
            UINT w = 0, h = 0;
            if (TargetDepth(pass, probe, w, h)) {
                probe->GetDevice(&device);
                g_device = device.Get(); // the game's device; it outlives us
            }
        }
        EyeMesh& m = g_eye[eye];
        ID3D11DepthStencilView* dsv = nullptr;
        UINT w = 0, h = 0;
        if (!CreateResources(g_device) || !TargetDepth(pass, dsv, w, h) || !UpdateMesh(eye)) {
            return;
        }
        m.lastClear = AddEntry(pass, eye, kBlock, dsv, w, h);
        if (g_clearLogs < 6) {
            ++g_clearLogs;
            TT2VR_LOG_INFO("lens mask: eye {} depth clear recorded (target {} {}x{}, entry {})", eye,
                Read<void*>(pass, kPassTarget), w, h, m.lastClear);
        }
    }
    else if (std::strcmp(name, kClearGBufferName) == 0 && g_eye[eye].lastClear >= 0) {
        Entry& clear = g_entries[g_eye[eye].lastClear];
        clear.confirmed.store(true, std::memory_order_release);
        AddEntry(pass, eye, kSky, clear.dsv, clear.width, clear.height);
        g_eye[eye].gbuffer = g_eye[eye].lastClear;
        g_eye[eye].lastClear = -1;
        if (!g_logged) {
            g_logged = true;
            TT2VR_LOG_INFO("lens mask: on (eye depth target {}x{}; hidden area drawn after the GBuffer depth clear and "
                           "reset to sky before 'Clear GBuffer')",
                clear.width, clear.height);
        }
    }
    else if (std::strcmp(name, kOceanName) == 0 && g_eye[eye].gbuffer >= 0) {
        const Entry& clear = g_entries[g_eye[eye].gbuffer];
        AddEntry(pass, eye, kAround, clear.dsv, clear.width, clear.height);
    }
}

} // namespace game
