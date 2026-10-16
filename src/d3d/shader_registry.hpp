#pragma once

#include <d3d11.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace d3d::shaders {

enum class Stage : std::uint8_t { VS, PS, CS, GS, HS, DS };
const char* StageName(Stage stage); // "vs", "ps", ...

// Hooks ID3D11Device::Create*Shader. Must run before the game creates its first shader.
void InstallHooks(ID3D11Device* device);

// xxh64 of the bytecode the shader was created from (stored on the object as private data).
std::optional<std::uint64_t> HashOf(ID3D11DeviceChild* shader);

// "ps:taa_resolve" for named shaders, "ps:1a2b3c4d5e6f7788" otherwise, "<stage>:null" for nullptr.
std::string Label(ID3D11DeviceChild* shader, Stage stage);

std::size_t Count();

// One line per unique bytecode hash: hash, stage, bytecode size, creation count, our name.
bool WriteRegistry(const std::filesystem::path& file);

} // namespace d3d::shaders
