#include "game/engine_textures.hpp"

#include "game/engine_layout.hpp"

#include <windows.h>

namespace game {
namespace {

using namespace layout;

using GetTextureFn = void* (*)(void* renderTarget, int index); // RenderTarget vtable slot 8 (+0x40): BaseTexture*

} // namespace

ID3D11Texture2D* RenderTargetTexture(void* renderTarget)
{
    __try {
        if (!renderTarget) {
            return nullptr;
        }
        const auto get = reinterpret_cast<GetTextureFn>(
            (*static_cast<void* const* const*>(renderTarget))[kRenderTargetTextureSlot]);
        void* texture = get(renderTarget, 0);
        return texture ? Read<ID3D11Texture2D*>(texture, kTexture2DDeviceTexture) : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

ID3D11Texture2D* RenderTargetTextureAt(void* base, std::ptrdiff_t offset)
{
    __try {
        return base ? RenderTargetTexture(Read<void*>(base, offset)) : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

ID3D11Resource* Texture3DResource(void* texture3d)
{
    __try {
        return texture3d ? Read<ID3D11Resource*>(texture3d, kTexture3DDeviceTexture) : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

} // namespace game
