// Engine signatures (CLAUDE.md hard rule 3: no hard-coded addresses).
// Resolution order per entry: the mangled export name in TT2.exe's export table (PlayAll engine code is
// exported), then the IDA-style pattern in .text as a fallback. When both resolve, the pattern is also checked
// at the export address so a drifting pattern shows up in the log before it is ever needed.
// Each entry records our name, export, pattern, how it was found and the exe SHA-256 it was validated on.
#pragma once

#include <cstddef>
#include <iterator>

namespace game {

struct Signature {
    const char* name; // our name, as in docs/re/symbols.md
    const char* exportName; // mangled TT2.exe export, or nullptr
    const char* pattern; // IDA style, see core::ParsePattern; nullptr = export only
    std::ptrdiff_t offset; // added to the pattern match address (e.g. to land on a rel32)
    const char* howFound; // short evidence note
    const char* validatedOn; // exe SHA-256
};

enum class Sig {
    RendererDoUpdate,
    ViewContextInitFromCamera,
    MatrixPerspectiveFovRH,
    TransformMatrixSetViewProjection,
    ViewContextRebuildFrustum,
    Count,
};

inline constexpr const char* kBuild4989319 = "103d4b2e4d86786a7eb1810faa280a41cec6cadce1e8865165c3de8ba87a601a";

// Order must match `Sig`. Patterns were checked unique in .text of build 4989319 (prologue bytes from Ghidra,
// rip-relative displacements and call targets wildcarded).
inline constexpr Signature kSignatures[] = {
    {"Renderer::DoUpdate", "?DoUpdate@Renderer@Game@PlayAll@@IEAAHPEAVGameContext@23@I@Z",
        "48 89 5C 24 18 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 81 EC 80 00 00 00 "
        "48 8B 99 D8 0D 00 00 4C 8B FA 48 8B F1",
        0, "export; 0x1405addd0, builds the ViewContexts from Renderer+0xe48/+0xe50 (docs/re/camera.md)",
        kBuild4989319},
    {"ViewContext::InitFromCamera",
        "?InitFromCamera@ViewContext@Graphics@PlayAll@@QEAAXPEBVCamera@23@AEBUViewport@23@AEBVVector2@Math@3@@Z",
        "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 48 8D 6C 24 A9 48 81 EC F0 00 00 00 48 89 91 50 04 00 00 "
        "48 8B D9 8B 42 7C",
        0, "export; 0x14091aeb0, camera world (pool) -> view, MatrixPerspectiveFovRH + jitter -> proj", kBuild4989319},
    {"Math::MatrixPerspectiveFovRH", "?MatrixPerspectiveFovRH@Math@PlayAll@@YAPEAVMatrix44@12@PEAV312@MMMM@Z",
        "40 53 48 83 EC 40 F3 0F 59 0D ?? ?? ?? ?? 48 8B D9 0F 29 74 24 30 0F 28 F2 0F 29 7C 24 20 0F 28 FB "
        "0F 28 C1 E8 ?? ?? ?? ?? F3 0F 10 2D ?? ?? ?? ?? 33 C0 0F 28 CE 48 89 43 0C F3 0F 5C CF",
        0, "export; 0x140813390, 20 callers; differs from MatrixPerspectiveFovLH 0x1408132f0 at +0x33", kBuild4989319},
    {"TransformMatrix::SetViewProjection",
        "?SetViewProjection@TransformMatrix@Graphics@PlayAll@@QEAAXAEBVMatrix44@Math@3@0@Z",
        "48 89 5C 24 08 57 48 83 EC 20 49 8B F8 48 8B D9 E8 ?? ?? ?? ?? 48 8B D7 48 8B CB 48 8B 5C 24 30 "
        "48 83 C4 20 5F E9",
        0, "export; 0x140915260, RecomputeView(view) then SetProjection(proj)", kBuild4989319},
    {"ViewContext::RebuildFrustum", "?RebuildFrustum@ViewContext@Graphics@PlayAll@@QEAAXXZ",
        "40 53 48 83 EC 40 F3 0F 10 81 40 04 00 00 48 8B D9 F3 0F 10 89 44 04 00 00 BA 04 00 00 00", 0,
        "export; 0x14091b4e0, frustum from ViewProj (slot 0) + clip bounds", kBuild4989319},
};

static_assert(std::size(kSignatures) == static_cast<std::size_t>(Sig::Count), "kSignatures must match Sig");

// Resolves every signature once (logs one line each). Call after the exe version is known, before installing
// engine hooks. Returns the number of unresolved entries.
int ResolveSignatures();
// Address of a resolved signature, or nullptr.
void* SignatureAddress(Sig id);

} // namespace game
