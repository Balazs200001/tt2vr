// shader_registry.cpp - every shader the game creates, identified by the xxh64 of its bytecode.
//
// Shader bytecode is only visible at Create*Shader time (CLAUDE.md, graphics gotchas), so these hooks
// are installed from the device-creation hook, before the game loads any shader.
#include "d3d/shader_registry.hpp"

#include "core/hooks.hpp"
#include "core/log.hpp"
#include "d3d/vtable.hpp"

#include <spdlog/fmt/fmt.h>
#include <xxhash.h>

#include <algorithm>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace d3d::shaders {
namespace {

// Private-data key under which we store the bytecode hash on each shader object.
constexpr GUID kHashGuid = {0x6a1e3c52, 0x8f0d, 0x4b7a, {0x9c, 0x21, 0x3e, 0x5d, 0x7f, 0x4a, 0x0b, 0x19}};

struct Entry {
    Stage stage;
    std::size_t size;
    int created;
};

std::mutex g_mutex;
std::unordered_map<std::uint64_t, Entry> g_entries;

const std::unordered_map<std::uint64_t, const char*>& Names()
{
    static const std::unordered_map<std::uint64_t, const char*> names = {
#define TT2VR_SHADER_NAME(hash, name) {hash, name},
#include "d3d/shader_names.inc"
#undef TT2VR_SHADER_NAME
    };
    return names;
}

const char* NameOf(std::uint64_t hash)
{
    const auto& names = Names();
    const auto it = names.find(hash);
    return it == names.end() ? nullptr : it->second;
}

void Register(ID3D11DeviceChild* shader, const void* bytecode, SIZE_T length, Stage stage)
{
    if (!shader || !bytecode || length == 0) {
        return;
    }
    const std::uint64_t hash = XXH64(bytecode, length, 0);
    shader->SetPrivateData(kHashGuid, sizeof(hash), &hash);
    std::lock_guard lock(g_mutex);
    auto [it, inserted] = g_entries.try_emplace(hash, Entry{stage, length, 0});
    ++it->second.created;
    if (inserted && core::GetVerbose()) {
        TT2VR_LOG_DEBUG("shader {}:{:016x} {} B", StageName(stage), hash, length);
    }
}

// Thread: any game thread that creates shaders (main thread and level-loader threads).
using CreateVSFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
using CreatePSFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
using CreateCSFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**);
using CreateGSFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11GeometryShader**);
using CreateGSSOFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, const D3D11_SO_DECLARATION_ENTRY*,
    UINT, const UINT*, UINT, UINT, ID3D11ClassLinkage*, ID3D11GeometryShader**);
using CreateHSFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11HullShader**);
using CreateDSFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11DomainShader**);

CreateVSFn g_createVS = nullptr;
CreatePSFn g_createPS = nullptr;
CreateCSFn g_createCS = nullptr;
CreateGSFn g_createGS = nullptr;
CreateGSSOFn g_createGSSO = nullptr;
CreateHSFn g_createHS = nullptr;
CreateDSFn g_createDS = nullptr;

template <typename Shader, typename Fn, Fn* Original, Stage S>
HRESULT STDMETHODCALLTYPE CreateHook(ID3D11Device* self, const void* bytecode, SIZE_T length,
    ID3D11ClassLinkage* linkage, Shader** out)
{
    const HRESULT hr = (*Original)(self, bytecode, length, linkage, out);
    if (SUCCEEDED(hr) && out) {
        Register(*out, bytecode, length, S);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateGSSOHook(ID3D11Device* self, const void* bytecode, SIZE_T length,
    const D3D11_SO_DECLARATION_ENTRY* decl, UINT numEntries, const UINT* strides, UINT numStrides,
    UINT rasterizedStream, ID3D11ClassLinkage* linkage, ID3D11GeometryShader** out)
{
    const HRESULT hr = g_createGSSO(self, bytecode, length, decl, numEntries, strides, numStrides,
        rasterizedStream, linkage, out);
    if (SUCCEEDED(hr) && out) {
        Register(*out, bytecode, length, Stage::GS);
    }
    return hr;
}

} // namespace

const char* StageName(Stage stage)
{
    static constexpr const char* kNames[] = {"vs", "ps", "cs", "gs", "hs", "ds"};
    return kNames[static_cast<int>(stage)];
}

void InstallHooks(ID3D11Device* device)
{
    void** vtbl = *reinterpret_cast<void***>(device);
    namespace dv = vt::device;
    core::hooks::Create("ID3D11Device::CreateVertexShader", vtbl[dv::CreateVertexShader],
        &CreateHook<ID3D11VertexShader, CreateVSFn, &g_createVS, Stage::VS>, &g_createVS);
    core::hooks::Create("ID3D11Device::CreatePixelShader", vtbl[dv::CreatePixelShader],
        &CreateHook<ID3D11PixelShader, CreatePSFn, &g_createPS, Stage::PS>, &g_createPS);
    core::hooks::Create("ID3D11Device::CreateComputeShader", vtbl[dv::CreateComputeShader],
        &CreateHook<ID3D11ComputeShader, CreateCSFn, &g_createCS, Stage::CS>, &g_createCS);
    core::hooks::Create("ID3D11Device::CreateGeometryShader", vtbl[dv::CreateGeometryShader],
        &CreateHook<ID3D11GeometryShader, CreateGSFn, &g_createGS, Stage::GS>, &g_createGS);
    core::hooks::Create("ID3D11Device::CreateGeometryShaderWithStreamOutput",
        vtbl[dv::CreateGeometryShaderWithStreamOutput], &CreateGSSOHook, &g_createGSSO);
    core::hooks::Create("ID3D11Device::CreateHullShader", vtbl[dv::CreateHullShader],
        &CreateHook<ID3D11HullShader, CreateHSFn, &g_createHS, Stage::HS>, &g_createHS);
    core::hooks::Create("ID3D11Device::CreateDomainShader", vtbl[dv::CreateDomainShader],
        &CreateHook<ID3D11DomainShader, CreateDSFn, &g_createDS, Stage::DS>, &g_createDS);
    TT2VR_LOG_INFO("shader registry hooks installed ({} names known)", Names().size());
}

std::optional<std::uint64_t> HashOf(ID3D11DeviceChild* shader)
{
    if (!shader) {
        return std::nullopt;
    }
    std::uint64_t hash = 0;
    UINT size = sizeof(hash);
    if (FAILED(shader->GetPrivateData(kHashGuid, &size, &hash)) || size != sizeof(hash)) {
        return std::nullopt;
    }
    return hash;
}

std::string Label(ID3D11DeviceChild* shader, Stage stage)
{
    if (!shader) {
        return fmt::format("{}:null", StageName(stage));
    }
    const auto hash = HashOf(shader);
    if (!hash) {
        return fmt::format("{}:unregistered@{}", StageName(stage), static_cast<const void*>(shader));
    }
    if (const char* name = NameOf(*hash)) {
        return fmt::format("{}:{}", StageName(stage), name);
    }
    return fmt::format("{}:{:016x}", StageName(stage), *hash);
}

std::size_t Count()
{
    std::lock_guard lock(g_mutex);
    return g_entries.size();
}

bool WriteRegistry(const std::filesystem::path& file)
{
    std::vector<std::pair<std::uint64_t, Entry>> entries;
    {
        std::lock_guard lock(g_mutex);
        entries.assign(g_entries.begin(), g_entries.end());
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return a.second.stage != b.second.stage ? a.second.stage < b.second.stage : a.first < b.first;
    });
    std::ofstream out(file, std::ios::trunc);
    if (!out) {
        return false;
    }
    out << "# tt2vr shader registry: xxh64(bytecode) stage bytes created name\n";
    out << fmt::format("# {} unique shaders\n", entries.size());
    for (const auto& [hash, e] : entries) {
        const char* name = NameOf(hash);
        out << fmt::format("{:016x} {} {} {} {}\n", hash, StageName(e.stage), e.size, e.created, name ? name : "-");
    }
    return static_cast<bool>(out);
}

} // namespace d3d::shaders
