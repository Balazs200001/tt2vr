// game::SharedExposure (PLAN Phase 4c): both eyes use one auto-exposure state, the left eye's.
//
// Why: the tonemapper keeps per-camera-index adaptation history (1x1 luminance RTs rotating through
// tm+0x130 / tm+0x138+idx*8), so two views adapt independently and can drift apart in brightness, which causes
// binocular rivalry (PLAN "Temporal design": exposure is view-independent state, shared, never disabled).
// docs/re/temporal.md P8 and section 9.4.
//
// How: hook the tonemap entry FUN_140914680 (tm, sceneHdr, dst, ctx, camIdx, vc), called per view at record time
// (DeferredRenderLoop::Render 0x140848768), view 0 then view 1. View 0 runs unchanged; its tail swaps its new result
// into tm+0x138. For view 1, around the call: tm+0x130 (the RT the compose reads) = tm+0x138 (view 0's result this
// frame), the luminance-chain gate tm+0x150 = 0 (no adaptation step for slot 1), and afterwards tm+0x130,
// tm+0x138+8 and the gate are put back, so slot 1's own history stays untouched. Passes execute later in submission
// order (view 0 first), so view 1's HDR_FinalCompose samples view 0's same-frame exposure.
#pragma once

namespace game {

// Thread: game main thread, after ResolveSignatures. Optional: a missing signature leaves per-eye exposure.
void InstallSharedExposure();
// Thread: game main thread, Renderer::DoUpdate pre-hook (start of a frame's recording).
void OnSharedExposureFrame();

} // namespace game
