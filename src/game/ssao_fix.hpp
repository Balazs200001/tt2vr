// game::SsaoFix (PLAN Phase 4): the AO pass (Intel ASSAO) for off-centre eye projections.
//
// Why: IntelSSAO::UpdateConstants 0x140927e20 (this, pass, const Matrix44& proj) writes the ASSAO constants straight
// into a constant buffer it allocates from the RenderLayer (vtable slot +0x70). It reconstructs view positions with
// NDCToViewMul = (2/m00, -2/m11) and NDCToViewAdd = (-1/m00, 1/m11): a symmetric frustum. The headset's eye
// projections are off-centre (Quest 3 left eye: m02 = -0.24, m12 = -0.19 in RenderDoc shadows_rider_frame3561), so
// every pixel's view position was skewed and AO darkened the wrong places, moving with the head.
//
// Fix: hook UpdateConstants; while it runs (thread-local flag) a pass-through hook on the RenderLayer allocator
// remembers the buffer it returns; after it returns, NDCToViewAdd (floats 10, 11) becomes
// ((m02 - 1) / m00, (1 + m12) / m11), the exact inverse of x_ndc = m00 * x / -z - m02 (docs/conventions.md layout:
// m02 at +0x8, m12 at +0x18). The buffer is uploaded when the recorded passes execute, later in the frame.
#pragma once

namespace game {

// Thread: game main thread, after ResolveSignatures and once the RenderLayer exists (first Present). Optional:
// missing signatures leave the AO pass as the game made it.
void InstallSsaoFix();

} // namespace game
