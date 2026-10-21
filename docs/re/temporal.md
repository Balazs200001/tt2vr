# Temporal state: every resource and constant, and how it becomes per-eye

Build 4989319 (see `README.md`). Rules come from `CLAUDE.md` (hard rule 1: never disable or bypass a temporal effect to fix a stereo artifact) and `PLAN.md` -> "Temporal design" (invariants T1 to T4, mechanisms A, B1, B2).

**Status of this document: mostly static; the single-view TAA, motion-vector, exposure and last-frame resources are confirmed at runtime** by RenderDoc capture `tt2_snaefell_helmet_moving_taa_f8323.rdc` (frame 8323, helmet camera, moving, AA = FXAA_TAA, 1920x1080, dynamic resolution off, one view; details in section 9). Everything else is from decompilation of the snapshot exe. Confidence tags (`confirmed` / `likely` / `guess`) keep what the source analysis gave: they describe how well the *static reading* is established, not whether a mechanism works in the mod. **Every per-eye mechanism status in this file is `candidate`**; rows whose resource, format, binding or constant the capture observed are tagged `confirmed (runtime, single view)` on the *observation* only (the capture has one view, so per-eye behaviour is untested). PLAN's inventory needs `confirmed` for each row in Phase 6; nothing below qualifies until the listed runtime check passes. Rows that depend on a RenderDoc or debugger observation say so.

**Phase 6 (2026-10-21):** the per-row results (two-view runtime evidence) are the Status column of section 6 and section 11; the row tables in section 3 keep their earlier static text and status lines.

Companion: `frame-anatomy.md` (pass order, split-screen path, threading). Names: `symbols.md`.

---

## 1. Headline

1. **The engine already keeps every view-dependent temporal resource in fixed 2-slot arrays indexed by a "camera index"** (`ushort` at `ViewContext+0x464`, copied to `RenderContext+0x594`). It is the split-screen player slot, maximum 2. `DeferredRenderLoop::Render` 0x1408469c0 runs the full pipeline (GBuffer -> SSR -> lighting -> tonemap -> FXAA -> TAA -> motion blur -> latch) once per `ViewContext` inside one call. `confirmed`, `runtime-check`.
2. **Hypothesis C (preferred candidate):** map eye k to engine camera index k by using the split-screen two-view path (`Renderer::DoUpdate` modes 1 or 2, `Renderer+0xe48/+0xe50` cameras, `ViewContext` at `Renderer+0x30` and `+0x650`). The engine then keeps per-slot history for free: TAA RT pair, last-frame colour/depth, last-frame VP and camera position, froxel history, exposure history, lens-flare queries, skinned-mesh validity, CSM caches. Status: candidate.
3. **Mechanism A (state swap, fallback):** render each eye as slot 0 in a normal single-view frame and swap the slot-0 contents of the field list in section 4 around each eye render. Status: candidate.
4. **Mechanism B1 (texture copy, narrowest):** valid only for TAA and SSR colour/depth history; it leaves last-VP, froxel and exposure shared, so it cannot be the whole answer. Status: candidate.
5. **Three hazards decide whether C works** (section 5): exposure is per-slot but PLAN requires it shared; the TAA reset flag is a single shared byte; dynamic resolution must be fixed. Plus: no camera-cut detection exists in the engine.

---

## 2. Camera-index slot map (the arrays hypothesis C relies on)

All arrays hold **2 slots** (bounded by adjacent fields). Slot 1 history only exists after `RenderLoop::OnSplitChanged` 0x1408dc2b0 -> `InitializeTAA(true, p)` + `InitializeLastFrameBuffers(p)` (`RenderLoop::Create` allocates slot 0 only: `InitializeTAA(false,false)`, `InitializeLastFrameBuffers(false)`). `OnSplitChanged` is called from the `Renderer::OnRender` resize block when `Renderer+0xe39` is set. `confirmed`.

| Object | Field | Offset (base + idx * stride) | Created / written by | Confidence |
|---|---|---|---|---|
| `RenderLoop` | TAA history A (read as `LastFrameBuffer`) | `+0xce8 + idx*0x10` | `InitializeTAA` 0x1408dbd40; swapped in `RenderTAA` | confirmed |
| `RenderLoop` | TAA output B (draw target) | `+0xcf0 + idx*0x10` | same | confirmed |
| `RenderLoop` | TAA effect `+0xcd8`, technique ids `+0xce0` (TAA), `+0xce4` (NoMotionVectorFix) | single | `InitializeTAA` | confirmed |
| `RenderLoop` | TAA reset flag (byte) | `+0xd08` **single** | ctor 0x1408d9120, `ClearTAA` 0x1404f5b10; cleared at end of `RenderTAA` | confirmed |
| `RenderLoop` | motion-vector RT | `+0xca8` **single, shared** | `InitializeMotionBlur` 0x1408db950 | confirmed |
| `RenderLoop` | lastVP (unjittered, camera-relative) | `+0xed8 + idx*0x40` | `PostUpdateLastFrameCamera` 0x1408dc2f0 | confirmed |
| `RenderLoop` | lastCamWorldPos (float3) | `+0xf58 + idx*0xc` | same; rebased by `PreUpdateLastFrameCamera` 0x1408dc6b0 | confirmed |
| `RenderLoop` | lastJitter (jitter x (0.5, -0.5)) | `+0xf70 + idx*8` | same | confirmed |
| `RenderLoop` | front-pass set: VP / cam pos / jitter | `+0xf80 + idx*0x40`, `+0x1000 + idx*0xc`, `+0x1018 + idx*8` | same, for the front-pass ViewContext at `RenderLoop+0x1590` | confirmed |
| `RenderLoop` | RenderTAA stored copies (used under `s_bMotionPause`): VP / cam pos / jitter / res scale | `+0x1028 + idx*0x40`, `+0x10a8 + idx*0xc`, `+0x10c0 + idx*8`, `+0x10d0 + idx*8` | `RenderTAA` | likely |
| `RenderLoop` | previous resolution scale (`s_vResolutionScaling`) | `+0x10d0 + idx*8` | `RenderTAA`, `CopyBuffersForNextFrame` | confirmed |
| `RenderLoop` | 'Last Frame Depth' (R32F) | `+0x10e0 + idx*8` | `InitializeLastFrameBuffers` 0x1408db790; `CopyBuffersForNextFrame` 0x1408d9ee0 | confirmed |
| `RenderLoop` | 'Last Frame Buffer' (HDR, 8 mips) | `+0x10f0 + idx*8` | same | confirmed |
| `DeferredLighting` | froxel ping-pong index (-1 = no history) | `+0x780 + idx*4` | `LightFroxel` 0x140839120 | confirmed |
| `DeferredLighting` | froxel history volumes (4 x Texture3D 'LightedFroxel') | `+0x760 + (pp*2 + idx)*8` | same | confirmed |
| `DeferredLighting` | froxel noise time | `+0x920 + idx*4` | `GetFroxelNoiseTime` 0x1408390a0 | confirmed |
| Tonemap object (`RenderLoop+0x1ed0`, entry `FUN_140914680`) | previous-luminance RT (ping-pong with `+0x130`) | `tm+0x138 + idx*8` | `FUN_1409120b0` (HDR_LuminanceFinal) | confirmed |
| tonemap | first-frame flag | `tm+0x14e + idx` | same | confirmed |
| tonemap | per-slot params (key, bias `+0x34`, speed `+0x38`, mode `+0x50`) | `tm+0x30 + idx*0x60` | init `FUN_1409126c0` | confirmed |
| tonemap | CPU readback of 1x1 luminance | `tm+0x148` **single float** | mapped staging RT `tm+0x128` | confirmed |
| `LensFlareInstance` | fade / queries / flag | `+0x64 + idx*4`, `+0x90 + idx*8` (queries), `[idx+0x75]` | `Update` 0x140898df0 | likely |
| `SkinMesh` | last frameId / LOD validity | `+0x158 + idx*4`, `+0x160 + idx*2` | `UpdateRenderData` 0x140906b90 | confirmed |
| `CascadedShadowMapRenderer` | per-camera cascades / static shadow cache | `+0x25b0 + idx`, stride 0x11a0 per idx | `SetupRenderContext` 0x140828320 | likely |
| `WaterDrop` | per-slot state | camIdx-keyed (`FUN_140914e20`) | | likely |

Slot-selection facts:

- `ViewContext+0x464` = camera index; per-view culling "view index" = `idx*13 + ViewContext+0x460`.
- With split mode 0, `DoUpdate` initialises one view at slot `ctz(mask)`. Whether `+0x464` follows the slot or the camera, and thus whether a single-view render could target slot 1, is **unverified** (`runtime-check`: "force `ViewContext+0x464 = 1` for a single-view render and check TAA/SSR/froxel/exposure use the second slot without corrupting slot 0").
- Views run **serially** inside the loop, each completing its full chain before the next, so shared transients (GBuffer, HDR RTs, MV RT, `RenderContext` at `DeferredRenderLoop+0x1f00`) are safe to reuse per view. `likely`.
- Engine native stereo (`ViewContext` eye block +0x1a8, `InitFromCameraStereo`) is **dead code and keyed by eye, not by camera index**; the temporal arrays never looked at it. It would have shared one history across both eyes. Not a mechanism for us (see `frame-anatomy.md` section 7). `confirmed`.

---

## 3. The inventory

Columns: `Row` maps to the PLAN inventory (P1..P11 in PLAN order) or is new (N1..N8). `Scope` follows `CLAUDE.md` rule 1 (per-eye = `stereo::ViewState`; shared = updated once per frame, not disabled). `Mechanism` lists candidates in preference order; all are `candidate`. "Engine today" says what the engine does on its own for slot idx.

### 3.1 Rows matching PLAN's inventory

#### P1. TAA history colour, including engine ping-pong

| Item | Value |
|---|---|
| Engine location | Pair of RTs per slot: A = `RenderLoop+0xce8+idx*0x10` (history, shader name `LastFrameBuffer`), B = `+0xcf0+idx*0x10` (draw target). Names 'TAA render target 0/1', size `RenderLoop+0xc60`, scale 1.0, format code 0x41424752 ('RGBA'). **Runtime correction (f8323, event 36731): the textures are B8G8R8A8_SRGB, not R8G8B8A8, and three distinct textures are involved, not a pair** (section 9.1); TAA runs on LDR after tonemap and film grain (confirmed runtime, single view). `RenderTAA` 0x1408ddf70 (call 0x140848c03) draws into B, then `RenderCopyTexture` copies B into the PostProcessManager current RT, then **A and B pointers are swapped** (ping-pong by pointer swap, no index). `NoMotionVectorFixTAA` 0x1408dc0e0 (call 0x1408de874, likely) then overwrites history RT A (post-swap = the frame just produced) for pixels without motion vectors, stencil-gated. |
| Gates | Active iff `s_eAntiAliasingMode` 0x141407dc8 (CVar `Engine.Renderer.AntiAliasing`: 0 none, 1 FXAA, 2 FXAA_TAA, 3 TAA) is 2 or 3 **and** `RenderLoop+0x20` (frame dt ms, `RenderLoop::SetTime`) != 0. Under `ViewContext::s_bMotionPause` 0x14140bb30 (dt == 0) `RenderTAA` swaps A/B on entry and uses the stored matrices at `+0x1028..`. Position in chain: after tonemap and FXAA, before dynamic-res upscale. `IsUsingTAA` 0x1404fd660. |
| Shader inputs | `g_TAAParams` = (prevResScale.x, prevResScale.y, reset flag as float, 0) (StringId 0x141408270, @0x1408de51a); `g_viewProjLastFrame` = `lastVP[idx]` (0x141408288, @0x1408de592); last camera pos; `g_jitter` = (cur.x*0.5, cur.y*-0.5, prev.x*0.5, prev.y*-0.5) (0x141408268, @0x1408de641) with cur = `RenderContext+0x578/0x57c` (= `ViewContext+0x448/0x44c`) and prev = `RenderLoop+0xf70+idx*8`; inverse current camera-relative VP; textures `DepthBuffer`, `FrameBuffer`, `LastFrameBuffer`, `MotionVectors` (`RenderLoop+0xca8`). After the draw: reset flag cleared; copies stored at `+0x1028/+0x10a8/+0x10c0/+0x10d0`. |
| Scope | **per-eye** |
| Candidate mechanism | **C** (eye k -> slot k): history pair already per slot. **A**: swap the pair pointers `+0xce8/+0xcf0` (and the other fields in section 4) around each eye. **B1**: before `RenderTAA`'s draw, `CopyResource(ViewState[E].history -> texture bound as LastFrameBuffer)`, after it copy the output back; works with the engine ping-pong because it copies whatever is bound. Two full-res copies per eye. |
| Runtime (f8323) | Event 36731: SRVs t0 `DepthBuffer`, t1 `FrameBuffer` (`3105`), t2 `LastFrameBuffer` (history, `3276`), t3 `MotionVectors` (`3399`); two RTs `3268` and `3272`, all B8G8R8A8_SRGB, 1920x1080; result `3272` copied to `3101` at 36758. Whether `3268`/`3272`/`3276` rotate across frames is **unverified** (one frame captured). Full detail in section 9.1. |
| Hazards | Slot-1 RTs do not exist until `OnSplitChanged(true)` runs, and `InitializeTAA(true,..)` recreates slot 0 as well (reset of both). Reset flag is shared (see N6). Size `RenderLoop+0xc60` is the full-window size, not the split size: in split mode each view samples history with the resolution-scale constants (`likely`, open question what `+0xc60` is in split mode). Jitter disabled in split mode (P2). RT format must match what we copy under B1. |
| Status | confirmed (runtime, single view) for the observed resource, binding and constants; per-eye behaviour and mechanism: candidate |
| Runtime check | Done (f8323): 'TAA resolve' bound SRVs, formats (B8G8R8A8_SRGB) and size (1920x1080), output copied at 36758. Still open: role rotation across frames (multi-frame capture), the second slot's textures in split mode. The 'RGBA' = R8G8B8A8 assumption is refuted. |
| Confidence | confirmed (layout and flow, decompile); confirmed (runtime, single view) for bindings, format and size; `NoMotionVectorFix` role likely (event 36786 is only a candidate); per-eye behaviour and rotation across frames not verified |

#### P2. TAA jitter index / offset

| Item | Value |
|---|---|
| Producer | `Renderer::DoUpdate` 0x1405addd0. **Not Halton.** A global parity counter `DAT_1413a88c4` selects: even frames offset (1/viewportW, 0), odd frames (0, 1/viewportH) (half a pixel in NDC after the x0.5 in `g_jitter`). The Vector2 is the 4th parameter (counting `this`; the 3rd by other counts) of `ViewContext::InitFromCamera(ViewContext*, Camera*, Viewport&, Vector2&)` 0x14091aeb0, which adds it to projection elements `[2][0]` and `[2][1]` (byte offsets +8 and +0x18) and stores it at `ViewContext+0x448/+0x44c`. `GetProjectionNoOffset` 0x14091adb0 subtracts it. `ViewContext::ResetProjectionOffset` 0x14091b570 (call 0x140848c72, after `PostUpdateLastFrameCamera`, on the `ViewContext` at `DeferredRenderLoop+0x2030`) removes it at the end of each view. The counter increments once per `DoUpdate` and holds when game dt (`param_1+0x60`) == 0. |
| Gate | TAA mode 2 or 3 **and** physics not paused **and** `Renderer+0x640 == 0` **and** (`Renderer+0xc90 == 0` or `Renderer+0xc60 == 0`), where `+0x640` / `+0xc60` are the view-type ints (`ViewContext+0x610`) of views 0 and 1. **Corrected 2026-10-16 (disasm 0x1405adfc6, `frame-anatomy.md` 5.1): jitter stays on in split mode** with an ordinary view 1; both views get the same vector. Ansel replaces it with `GameEngine::GetAnselProjectionOffset` (single-view branch, `Renderer[0x19]`). All other `InitFromCamera` callers (`UpdateShift`, `ProbeRenderer`, external-scene views, `Pick3dInList`) pass `V_VECTOR2_0`. `RenderContext::Prepare(ViewContext const&)` 0x1408cfad0 copies the whole `ViewContext` including the offset. |
| Previous jitter | `lastJitter[idx]` = `RenderLoop+0xf70+idx*8` (latched x0.5 in `PostUpdateLastFrameCamera`); front-pass set `+0x1018+idx*8`. |
| Scope | **per-eye** (index and previous value per eye) |
| Candidate mechanism | **C**: if we keep the split path, the gate must be bypassed in a way that still produces jitter (see hazards). Our hook on `InitFromCamera` can add the jitter itself: it knows the counter `DAT_1413a88c4`, the viewport, and the mode. **A**: slot 0's `lastJitter` (+0xf70) is swapped with the rest, the counter is advanced once per eye render or once per frame (decision needed, T1). In both cases the offset must be written back to `ViewContext+0x448/+0x44c` so `GetProjectionNoOffset` and `g_jitter.xy` stay consistent. |
| Hazards | (1) ~~Disabled in split mode~~: refuted 2026-10-16 (gate re-read from disasm; see Producer). Still `runtime-check`: `g_jitter` non-zero in both views' 'TAA resolve'. (2) Both views share one counter and get the same phase; that is fine for T1 (each eye's previous value is its own N-1 value) provided the counter advances once per frame, not once per eye. In alternate-eye mode the previous value is N-2 for that eye, and the parity then alternates the *wrong* way (an eye may always get the same phase); decide and record. (3) **Do not reuse `ViewContext+0x448` for persistent per-eye lens shift** (camera note): the OpenXR asymmetric projection's off-axis terms live in the same `[2][0]/[2][1]` elements; they must be set in the projection and the jitter added on top, and only the jitter stored at `+0x448` so `GetProjectionNoOffset` removes the jitter and keeps the lens shift. (4) Jitter is in projection units, not pixels: it depends on the viewport size per eye. (5) The engine's matrices elsewhere (front-pass `ViewContext`, CSM `SetupRenderContext` "adds +0x448") also use the offset. |
| Status | confirmed (runtime, single view) for the observed resource, binding and constants; per-eye behaviour and mechanism: candidate |
| Runtime (f8323) | Event 36731 `g_jitter` = (0, -0.00046296295, 0.00026041668, 0): cur = (0, +1/1080), prev = (+1/1920, 0), i.e. (cur.x*0.5, cur.y*-0.5, prev.x*0.5, prev.y*-0.5) as in the formula; cbCameraStd `projectionOffset` = (0, 1/1080) with proj[1][2] = +offset.y baked into viewProjection (event 36833). Only one frame was captured, so the full alternation over 4 frames is not seen. |
| Runtime check | Breakpoint or log on `InitFromCamera` for a frame: count calls, dump the 4th argument over several frames (expect alternating (+-1/w, 0) and (0, +-1/h)). RenderDoc: `g_jitter` in the 'TAA resolve' constant buffer for 4 frames (expect (+-0.5/w, 0) / (0, +-0.5/h) with previous in zw) and `cbCameraStd` projection [2][0]/[2][1]. Watch `DAT_1413a88c4` across a pause (dt == 0) and a World Shift. |
| Confidence | confirmed (producer and gate); likely (Ansel branch, held counter); formula and the two phases confirmed (runtime, single view) |

#### P3. Previous view / projection / viewProj (jittered and unjittered)

| Item | Value |
|---|---|
| Engine location | **Only the unjittered, camera-relative previous VP is kept, plus the previous jitter separately**; no jittered previous matrix exists. `PostUpdateLastFrameCamera` 0x1408dc2f0 (call 0x140848c58; end of each view's chain, after TAA and motion blur; skipped under `s_bMotionPause`) latches per slot: `lastVP[idx]` = `GetProjectionNoOffset` x (view with translation zeroed) at `RenderLoop+0xed8+idx*0x40`; `lastCamWorldPos[idx]` at `+0xf58+idx*0xc`; `lastJitter[idx]` at `+0xf70+idx*8`. Second set for the front-pass `ViewContext` (`RenderLoop+0x1590/+0x15a8`): `+0xf80+idx*0x40`, `+0x1000+idx*0xc`, `+0x1018+idx*8`, gated by `this[0x1e70]` or (`this[0x111c]` and the camera has byte `+0x98`). Upload: `UpdateLastFrameCameraCB` 0x1408df3c0 writes 0x50 bytes (matrix + cam pos) into the **shared** constant buffer `cbVSCameraLastFrameStd` (StringId 0x1414075b0) for the MV passes; flag selects the front-pass set. Accessors: `GetLastFrameViewProj` 0x1404f8890 (no callers). |
| Consumers | `RenderTAA` (`g_viewProjLastFrame`), `RenderMotionBlur` (`g_viewProjectionLastFrame` 0x141408018, `g_cameraPositionLastFrame` 0x1414080c8), MV passes 'Motion Vectors' / 'Motion Vectors front pass', `RenderSSR` (last VP + last cam pos), froxel (see N1). |
| Scope | **per-eye** |
| Candidate mechanism | **C**: the latch runs per view and reads that view's `ViewContext`, so eye k's `ViewContext` (with its own view matrix and position) fills slot k automatically. **A**: swap the whole block `+0xed8..+0x10d8` (and front-pass set). **B1**: not applicable (leaves matrices shared). |
| Hazards | (1) **`lastCamWorldPos` comes from the ViewContext's camera position** (`InitFromCamera` stores the camera position at `ViewContext+0` from the world pool and a culling position at `+0xc`); if we replace only the view matrix and not the position fields, latched positions are wrong for the eye (`guess`: needs a check that all three agree per eye). (2) Matrices are camera-relative, so head translation between frames is carried by `lastCamWorldPos` differences; both need the per-eye value. (3) Latching happens **after** TAA and motion blur, so within a view they see the previous frame's value. (4) World Shift: only camera positions are rebased (N7). (5) Pause: `s_bMotionPause` freezes latching. (6) Under alternate-eye, eye E's previous value is N-2 (T1) and this is intended. |
| Status | confirmed (runtime, single view) for the observed resource, binding and constants; per-eye behaviour and mechanism: candidate |
| Runtime (f8323) | Event 36731: `g_viewProjLastFrame` @80 is jitter-free (row0 . R2 and row1 . R2 ~ 0); `g_cameraPosition` (-211.20682, -14.257798, 50.707253) and `g_cameraPositionLastFrame` (-210.17304, -13.224817, 50.830555) differ by (1.034, 1.033, -0.123) m per frame. `cbVSCameraLastFrameStd` itself was not read. |
| Runtime check | RenderDoc: 'Motion Vectors' and 'Motion Vectors front pass': `cbVSCameraLastFrameStd` content against the previous frame's `cbCameraStd` unjittered VP; confirm the camera-relative convention. Debugger: watch `RenderLoop+0xed8..` across a frame in split mode to see slot 0 and slot 1 filled by distinct views. |
| Confidence | confirmed (flow, offsets); runtime-check |

#### P4. Previous depth / Hi-Z (disocclusion, occlusion culling)

| Item | Value |
|---|---|
| Engine location | 'Last Frame Depth' (format 'FL32' = R32F), `RenderLoop+0x10e0+idx*8`, size `RenderLoop+0xc60`, filled by `CopyBuffersForNextFrame` 0x1408d9ee0 (call 0x14084867f), **conditionally** (see N2). Only reader found: `DeferredRenderer::RenderSSR` 0x140844de0 (call sites 0x140844e17 / 0x140844e36; `GetLastFrameDepth` 0x1408db2a0). TAA reads only the *current* `DepthBuffer`. |
| Hi-Z / occlusion | **No GPU Hi-Z history and no GPU occlusion culling from previous depth was found.** Visibility culling is CPU (`VisibilityCuller`, enki jobs; occluders are static terrain occluders loaded by the world streamer, log line 'Loading 80 Terrain Occluders from World Streamer'; `ClearOccluder` is called every scene). 'Tile Min Max Depth CS' (froxel/tile culling) reads the current depth and is not history (`guess`, from the name only). |
| Scope | **per-eye** (last-frame depth); nothing to do for Hi-Z |
| Candidate mechanism | **C**: array per slot. **A**: swap `+0x10e0` pointer. **B1**: `CopyResource` into/out of the bound last-frame depth at the SSR pass. |
| Hazards | Only filled when a consumer needs it (N2); if the conditional gate is off for slot 1 (e.g. the per-view flag `[0x61e]` is set per ViewContext), slot 1's history is never refreshed. Format R32F must match under B1. |
| Status | confirmed (runtime, single view) for the observed resource, binding and constants; per-eye behaviour and mechanism: candidate |
| Runtime (f8323) | 'Last Frame Depth' copy at event 35759: depth 339 (SRV 340) -> `3284` R32_FLOAT 1920x1080, after Colour Pass #7 (transparency) and before bloom, as static says. The copy ran in this capture, so its gate was true. No compute pass writing a Hi-Z pyramid was seen (consistent with no Hi-Z). |
| Runtime check | RenderDoc: 'Generate last frame mips' present only when SSR or water drops are active; SRVs of 'SSR pass'. Search a capture for any compute pass writing a Hi-Z pyramid from previous depth (expected: none). |
| Confidence | confirmed (last-frame depth); likely (no Hi-Z history; absence from decompile only) |

#### P5. Temporal AO / SSR / volumetric accumulation

| Item | Value |
|---|---|
| SSAO | `IntelSSAO::Render` 0x140927c50, `UpdateConstants` 0x140927e20: no history buffers or frame counters seen; **likely stateless / spatial only**. Reads `s_vResolutionScaling`. |
| SSR | `DeferredRenderer::RenderSSR` 0x140844de0 -> `DeferredLighting::RenderSSR` with last-frame depth + colour + `lastVP[idx]` + `lastCamPos[idx]` + prev resolution scale (`RenderLoop+0x10d0+idx*8`). The history is the N2 pair, not a separate accumulation buffer. |
| Volumetric | see N1 (froxel history). The 'Volumetric accumulation CS' output (`DeferredLighting+0x890`, `GetFroxelScatteringAccumulation`) is a single **shared accumulation volume produced and consumed inside the same view pass**, i.e. a transient, not history (`likely`). |
| Scope | AO: nothing to do if stateless. SSR/volumetric: **per-eye** (via N1, N2) |
| Candidate mechanism | As N1 and N2. |
| Hazards | The accumulation volume at `+0x890` is shared across views; safe because views run serially and it is rewritten each view. If a mechanism interleaves eyes at a finer granularity than "whole view chain per eye", this breaks (`guess`). |
| Status | candidate |
| Runtime check | RenderDoc: IntelSSAO passes have no history SRV; 'SSR pass' SRV list; accumulation volume written then read within one view. |
| Confidence | likely (AO stateless); confirmed (SSR inputs) |

#### P6. Motion blur camera matrices

| Item | Value |
|---|---|
| Engine location | `RenderLoop::RenderMotionBlur` 0x1408dd5e0, per view after TAA, only if `DAT_141407830[idx]` (per-slot enable from CVar `Engine.Renderer.MotionBlur.Enable`, `s_bMotionBlurEnable`) and `RenderLoop+0x20` (dt) != 0. Uses the **same** `lastVP[idx]` (`+0xed8+idx*0x40`) and `lastCamPos[idx]` (`+0xf58+idx*0xc`) as `g_viewProjectionLastFrame` / `g_cameraPositionLastFrame`, the shared MV RT (`+0xca8`), dt (`+0x20`) and the 60-frame average dt (`+0x24`, x1000). Otherwise stateless. Latching happens after it, so it sees the previous frame's VP. |
| Scope | **per-eye** for camera matrices (shared with P3); object motion (per-object previous transforms, P7) is shared |
| Candidate mechanism | Same as P3. |
| Hazards | The MV RT is shared and reused per view; with serial per-view execution it holds only the current view's vectors. Motion blur strength depends on dt: if our render path changes the apparent frame time (e.g. render at the headset rate, not the game rate), `RenderLoop::SetTime` must still see the game dt. |
| Status | confirmed (runtime, single view) for the observed resource, binding and constants; per-eye behaviour and mechanism: candidate |
| Runtime (f8323) | Event 36833: SRVs `DepthBuffer_tex`, `FrameBuffer_tex`, `MotionVectors_tex` (`3399`); cbCameraStd (resource 113) plus `g_viewProjectionLastFrame`, `g_motionBlurParam1/2`; output `3105`. Runs after TAA (36731) and the copy (36758), as static order says. |
| Runtime check | RenderDoc: 'Render MotionBlur' constant buffer vs P3 values; check blur in both eyes during head rotation. |
| Confidence | confirmed (inputs); runtime-check |

#### P7. Per-object previous world transforms

| Item | Value |
|---|---|
| Engine location | `Mesh::UpdateRenderData` 0x1408a3f00 (ViewContext, frameId, dbIdx): advances previous <- current (`Mesh+0xf4..0x124 <- +0xc4..`) **only when `frameId != Mesh+0x70`** (last frame the object was updated) **and `s_bMotionPause` is false**, then loads current from the global transform array `DAT_1413cda88[node+0x50]*0x30`. If `frameId != last+1` it sets previous = current (no motion; skip detection). Result is packed into a 0x70-byte `DoubleBuffer` record consumed by `cbVSObjectInstancedMotionVector` / `cbVSObjectSkinnedLastFrame`. Called through `RenderEntity::FillRenderInstances` 0x1408d06c0 (virtual +0x60), which writes `Mesh+0x70 = frameId` after a successful update and adds instances to the per-view-index `RenderInstanceManager`. Runs on render-phase enki jobs, not in sim update. `SkinMesh::UpdateRenderData` 0x140906b90 keeps per-slot last frameId / LOD (`SkinMesh+0x158+idx*4`, `+0x160+idx*2`) for motion-valid flags (`param_2 == last+1`). |
| Scope | **shared** (once per frame) |
| Candidate mechanism | **C**: one `Render` call; `FillRenderInstances` runs once per scene (culling is the union of views), so the advance happens once and is idempotent per `frameId`. No work. **A / per-eye `Render` calls**: the guard makes a second call with the *same* `frameId` harmless; a second call with a *different* `frameId` advances previous <- current a second time and the second eye sees **zero object motion** (the exact concern in PLAN). Our per-eye call must not change what the engine uses as `frameId`. |
| Hazards | **The `frameId` source is not traced.** Candidate source: `RenderLayer+0x30` (render id, `GetFrameID` 0x1403d3df0) which increments on every `BeginRender`; the external-scene loop and any per-eye `BeginRender`/`EndRender` cycle would bump it twice per presented frame. `SkinMesh` per-slot validity (`param_2 == last+1`) breaks the same way. World Shift: `Mesh+0xf4` is not rebased (N7). |
| Status | candidate. **Phase 5 (2026-10-21, `renderer.md` 4):** frameId = `RenderLayer+0x30`, read by the fill job `FUN_1408d3370`, which calls `FillRenderInstances` once per visible view with the same frameId: `Mesh` advances once per frame (confirmed). **`SkinMesh` is wrong in two-view frames:** one EntityRenderData and one cur/prev palette pair per mesh; view 1's call latches prev <- cur a second time, so both views draw skinned meshes with prev == cur (zero object motion). Trace evidence: skinned MV CB pairs differ in flat trace 10866 (5/5), identical in two-view traces 12770/9191/13970/4400 (41/44). Fixed by `game::SkinMotion` (PLAN 5a; trace 12270: prev != cur in both views, identical between views). |
| Runtime check | Debugger: breakpoint `Mesh::UpdateRenderData` 0x1408a3f00 and log (this, frameId) in a two-view (split) frame to confirm the second view does not advance `Mesh+0xf4`; find where `frameId` comes from; log `RenderLayer+0x30` per frame. |
| Confidence | confirmed (mechanism); open (frameId source) |

#### P8. Auto-exposure / eye adaptation

| Item | Value |
|---|---|
| Engine location | Tonemap object `RenderLoop+0x1ed0`, entry `FUN_140914680` (call 0x140848768 in the view loop, passing the current post-process RT, `camIdx` as `param_5`, `ViewContext*` as `param_6`): 'Downscale', then `FUN_1409120b0` ('HDR_LuminanceInit' / 'HDR_LuminanceDownscale' / 'HDR_LuminanceFinal', if flag `tm+0x150`), then `FUN_1409131d0` ('HDR_FinalCompose'). **History is per slot**: the output 1x1 RT `tm+0x130` is ping-pong swapped with previous-luminance `tm+0x138+idx*8` (bound as texture slot 3 in HDR_LuminanceFinal); first-frame flag `tm+0x14e+idx` (when set the pass gets blend 1.0 = snap); params `tm+0x30+idx*0x60` (key, bias `+0x34`, speed `+0x38`, mode `+0x50`). Final factor ~ speed[idx]*dt (`param_2+0x2c`). A staging copy of the 1x1 luminance is mapped to CPU (half to float if FL16) into **`tm+0x148`, a single shared float** (debug text 'Scene Current Luminance: %.4f' and the clamp `FUN_140914500`). 'Reset luminance' string belongs to init `FUN_1409126c0` (called from `RenderLoop::Create`, 0x1408dad36). `FUN_140914e20` (takes camIdx) is separate (water-drop state). |
| Scope | **shared** (PLAN, `CLAUDE.md`): one adaptation state for the head, both eyes read the same value |
| Candidate mechanism | **C**: the engine gives each slot its own adaptation, so left and right will drift. To share: (a) after slot 0's `HDR_LuminanceFinal` write, copy its 1x1 result into slot 1's previous-luminance RT (and pin slot 1's first-frame flag to 0), or point `tm+0x138+1*8` at the same texture as slot 0 so the ping-pong swap is common (careful: the swap exchanges `tm+0x130` with `tm+0x138+idx*8`); (b) or substitute the 1x1 binding of eye 1's `HDR_FinalCompose` with eye 0's luminance. **A**: with idx fixed at 0 there is one state; nothing extra, but then eye 1 must **not** run the adaptation step a second time (guard: skip `HDR_LuminanceInit/Downscale/Final` for the second eye, or restore the 1x1 RTs after eye 1). |
| Hazards | **Contradiction between source notes, resolved here:** the render-loop note said exposure is a single object that would double-step; the later temporal decompile shows history IS per camera index, so no double step but **independent adaptation per eye**. Either way PLAN's rule (one adaptation state per head) is violated unless we share it. Luminance is measured on the current view's HDR (eye-specific), so even a shared value should be computed from one source (eye 0) or the average of both, decide and log. The CPU readback `tm+0x148` always holds the last view's value. Unknown: params block `tm+0x30` semantics and whether a pre-exposure is applied before the last-frame colour copy (N2). |
| Status | confirmed (runtime, single view) for the observed resource, binding and constants; per-eye behaviour and mechanism: candidate |
| Runtime (f8323) | Chain: event 36541 -> `3372` R16_FLOAT 16x16 (reads `3368` R16F); 36570 probably 4x4 (guess); 36606 -> `3385` R16_FLOAT 1x1 (reads `3376` and `3380`, both R16_FLOAT, consistent with a blend against previous luminance); tonemap `HDR_FinalCompose` (36652) reads `3385` as `LuminanceTexture_tex` (SRV t2). Ping-pong across frames and the first-frame flag were not observed (one frame). |
| Runtime check | RenderDoc: step 'HDR_Luminance*' and the 1x1 RT ping-pong across frames; verify the slot-3 previous-luminance binding and the first-frame flag. Watch `tm+0x148`, `tm+0x130`, `tm+0x138+idx*8` across two views in a frame (split mode) to see independent adaptation. |
| Confidence | confirmed (per-slot layout); likely (shared readback semantics); the choice of sharing mechanism is a guess |

#### P9. GPU particles / simulations

| Item | Value |
|---|---|
| Engine location | PopcornFX (`HHParticlesManager`). Sim: `UpdatePhysicalSpawn`/`UpdatePhysicalEvolve` in `RenderOneFrame` step 2, `UpdateEvolve` in `Renderer::OnRender` (once per frame), `UpdateFence` and `SceneManager::UpdatePopCornBound` only when camera idx == 0 inside `DeferredRenderLoop::Render` (and also in `OnRender`), `UpdatePkFxSceneCB` once per frame. Per view: `StartRenderParticles(scene, view, 1)`, `KickRenderParticles(view, 1)`, `StartRenderParticles(., 0)`, later `StartRenderParticles(., 2)`, `AddParticleLights`. `CullParticles` runs after `Render`, on view 0. `HHParticlesManager::OnShift` rebases particles at World Shift. PopcornFX has its own thread pool on top of enki. `HHParticlesManager::SetViewContext` is fed `Renderer::GetViewContext(i)` (0x14048c44a) before `DoUpdate` rebuilds them (0x14048c536): PopcornFX may use last frame's view (`guess`, pointer-vs-copy unknown). |
| Scope | **shared** (once per frame); render-side particle draw is per view and has no history |
| Candidate mechanism | **C**: engine already does this (`idx == 0` guards). **A**: ensure the sim steps run once per frame, not once per eye (they are in `OnRender`/`RenderOneFrame`, outside `RenderLoop::Render`; a per-eye call of only `Render` does not repeat them, but `UpdateFence`/`UpdatePopCornBound` are inside `Render` guarded by idx == 0, so they would run **twice** if slot 0 is reused for both eyes: guard needed). |
| Hazards | PopcornFX GPU-sim internals not decompiled; "view-independent" is inferred. Particle sort/cull depends on the view (`CullParticles` on view 0 only); eye 1 reuses eye 0's cull (small error for particle visibility at the periphery; likely acceptable). |
| Status | candidate. **Phase 5 (2026-10-21, `renderer.md` 1, 6):** `CullParticles` 0x1409a2a90 culls with the **union** of both views (not view 0; the hazard above is void); `UpdateFence`/`UpdatePopCornBound` run once (idx 0); `SetViewContext` copies last frame's views (engine behaviour). Open: medium 2 (per-camera effect pairs) stays disabled without `GameEngine+0x184` (`renderer.md` 6 A). |
| Runtime check | Break on `UpdateFence` / `UpdatePopCornBound` per frame in a two-view frame (expect once); RenderDoc: particle compute dispatches appear once per frame. |
| Confidence | confirmed (call placement); likely (view-independence) |

#### P10. Shadow maps / cascades

| Item | Value |
|---|---|
| Engine location | `CascadedShadowMapRenderer` (`RenderDesc+0xc40`), set up per scene in `DeferredLighting::SetupLights` 0x14083d2c0 -> `SetupRenderContext` 0x140828320 (call 0x14083d427) with the **whole view array**; per-camera-index state at `CSM+0x25b0+idx`, stride 0x11a0, 'Cascaded %d' render contexts, `ViewContext+0x470` `CascadedShadowRenderDesc`; 'Static Shadow' cache re-shifted at World Shift (`ShiftStaticShadowMap` 0x140829380, called from `RenderStaticShadow` 0x140827240), `CreateStaticShadowMap` on resize. Log: 2048 x 3 cascades. `DeferredRenderer::RenderDirectionalCascadedShadowMap` 0x140841d60 and `RenderShadowMaps` 0x140844f00 are 12-byte stubs; the cascade draws are issued elsewhere, via the CSM contexts. |
| Scope | **shared** (PLAN: rendered once, cascades fitted to the combined eye frustum) |
| Candidate mechanism | **C**: the engine already culls on the union of views and sets lights up once per scene, but keeps per-slot cascade state, so it **may fit and render two cascade sets** (one per camera index). If both views are the two eyes of one head, two sets cost double and can differ by sub-texel amounts between eyes (shadow shimmer rivalry). To share: force slot 1's cascade matrices to slot 0's after `SetupRenderContext`. **A**: slot 0 only; shadows rendered once per frame if `SetupLights` runs once. |
| Hazards | **Whether cascades are fitted per view or merged is unproven** (`guess`). The static shadow cache is shifted at World Shift per slot. Under split mode the engine may treat the second view as a different player (probes/shadow assumptions, `guess`). |
| Status | candidate. **Phase 5 (2026-10-21, `renderer.md` 3):** fitted **per camera index** from each eye's ViewContext with the union projection (no seam from head rotation) and rendered **per view** (atlas clear, static map, 3 cascades; trace 4400). Shared since PLAN 5c (`game::SharedShadows`): index 1 reuses index 0's `CascadedShadowContext` (world-space matrices, confirmed), fitted from the head centre; one shadow render per frame (trace 12870). |
| Runtime check | Read `CascadedShadowMapRenderer` state after `SetupLights` with two views: distinct cascade matrices per camera index, or shared? RenderDoc: count cascade draws per frame. |
| Confidence | likely (per-idx state exists); guess (fitting policy) |

#### P11. Reflection / environment probes

| Item | Value |
|---|---|
| Engine location | `ProbeRenderer::Update` 0x14056b3a0, once per frame in `OnRender`; `RenderLoop::RenderProbe` 0x1408ddeb0 (`DeferredLighting::SetupProbeLights` + `DeferredLighting::RenderProbe`) before `Render`, receiving the view array (`RenderLoop+0x1ea8`) and `RenderLoop+0x10d0`. `ProbeRenderer::RenderAt` 0x140568410 is the offline/editor bake (`Textures/Probes/*_unfiltered.dds`). Probe relight cadence not decompiled. |
| Scope | **shared** (once per frame) |
| Candidate mechanism | **C**: runs once per `OnRender` before the view loop (confirmed placement). **A**: if `OnRender` is not re-run per eye, nothing to do. |
| Hazards | `RenderProbe` receives the whole view array, so probe relight may depend on view positions (which view? union?): unverified. Probe state is also externally re-run per external scene. |
| Status | candidate. **Phase 5 (2026-10-21, `renderer.md` 1):** `RenderProbe` runs once per frame and picks the probe nearest to any view (one face per frame, 7-frame relight cycle); probe filtering appears once in trace 4400. |
| Runtime check | Count `RenderProbe` calls per frame (expect 1); RenderDoc: probe passes appear once. |
| Confidence | confirmed (placement); runtime-check |

### 3.2 New rows (not in PLAN's inventory yet)

#### N1. Froxel (volumetric lighting) temporal history

| Item | Value |
|---|---|
| Engine location | `DeferredLighting::LightFroxel` 0x140839120, per slot: ping-pong index `DeferredLighting+0x780+idx*4` (**-1 = no history yet**, uses non-temporal technique ids `+0x5d8/+0x5dc` 'csLightFroxel[NoShadow]'; otherwise temporal `+0x5e0/+0x5e4` 'csLightFroxelTemporal[NoShadow]', index toggles 0/1 each frame); four Texture3D 'LightedFroxel' at `+0x760+(pp*2+idx)*8`; compute slot 4 (RO) = previous volume, slot 0 (RW) = current. `GetFroxelLightedVolume(idx)` 0x1404f8550 returns the current one. Noise time accumulates per slot at `+0x920+idx*4` (`+= Wind::GetStrength(idx) * a * b`). `FillCSLightCB` receives the previous-frame resolution scaling Vector2 (`RenderLoop+0x10d0+idx*8`) but **no previous-matrix input was found**: the reprojection method (same-cell blend vs matrix) is unestablished. The technique ids are created in `DeferredLighting::Create` 0x140833fa0 (string xref 0x140fdb570 at 0x140835338). |
| Scope | **per-eye** |
| Candidate mechanism | **C**: already per slot. **A**: swap `+0x780` index plus the four volume pointers (`+0x760..+0x778`) and `+0x920`. **B1**: not suited (a 3D texture pair plus a ping-pong index in CPU state). Resetting a slot: writing -1 to `+0x780+idx*4` should select the non-temporal technique for one frame (`guess`, from the branch). |
| Hazards | If froxel history is reprojected with a matrix, the previous matrix source is unknown (`runtime-check`); if it is a same-cell blend, a per-eye history keyed to the wrong eye would blend the wrong eye's volume (bad, but different from TAA ghosting). Volumes are sized by the full window or split size (open). |
| Status | candidate |
| Runtime check | RenderDoc: `csLightFroxelTemporal` dispatch: SRV slot 4 and `cbCSLightData` for any previous-frame matrix; scrub camera motion to see whether history is reprojected. Dump ping-pong index per slot in split mode. |
| Confidence | likely (layout confirmed; reprojection method unknown) |

#### N2. Last-frame colour and depth for SSR and water drops

| Item | Value |
|---|---|
| Engine location | `InitializeLastFrameBuffers` 0x1408db790 creates per slot 'Last Frame Depth' (R32F, `RenderLoop+0x10e0+idx*8`) and 'Last Frame Buffer' (format code 0x464c4834, likely R16G16B16A16F, 8 mips, `+0x10f0+idx*8`), size `RenderLoop+0xc60`. `CopyBuffersForNextFrame` 0x1408d9ee0 (call 0x14084867f, per view): copies scene depth and HDR scene colour (**before** tonemap, **after** transparency and the HUD flag-8 pass, so including 3D HUD pixels) and runs 7 'Generate last frame mips' passes (texture 'LinearInput'). It also latches the prev resolution scale to `+0x10d0` when TAA is off. **Conditional**: `DAT_1413cbda0` && a per-context flag, or `WaterDrop::HasAliveParticles(idx)`, or view flag `[0x61e]`. Getters: `GetLastFrameBuffer` 0x1408db290, `GetLastFrameDepth` 0x1408db2a0 (only caller `RenderSSR`). Consumers: `RenderSSR` (0x140844de0) and `WaterDrop` (screen-space visor drops, per-slot state). |
| Scope | **per-eye** |
| Candidate mechanism | As P4 (**C**, **A** swap `+0x10e0/+0x10f0`, **B1** copy at the SSR draw). |
| Hazards | Conditional copy: if the gate is false for a frame, history is stale and the consumer must also be gated off; a mechanism that forces copies must also keep the consumers consistent. HUD pixels in the history (flag-8 HUD) are an engine behaviour, not a bug. Format unconfirmed. Mips: 8 per slot (cost under B1). |
| Status | confirmed (runtime, single view) for the observed resource, binding and constants; per-eye behaviour and mechanism: candidate |
| Runtime (f8323) | 'Last Frame Depth' event 35759: `3284` R32_FLOAT. 'Last Frame Buffer' events 35786, 35819, 35842, 35865, 35888, 35911, 35934, 35957 (8 draws): texture `3288` RGBA16F with 8 mips (mip0 1920x1080, mip7 15x8), source HDR scene `3591`. Both after Colour Pass #7, before bloom/tonemap. The copies ran in this capture. The SRV list of the SSR pass was not verified (SSR CS 20770 reads `SSRTempRead`). |
| Runtime check | RenderDoc: 'Generate last frame mips' appears only with SSR or water drops; formats and mip count of 'Last Frame Buffer'; SRV list of 'SSR pass'. |
| Confidence | confirmed (layout, flow); likely (formats) |

#### N3. Lens-flare occlusion queries

| Item | Value |
|---|---|
| Engine location | `LensFlareInstance::Update` 0x140898df0: per-slot fade value (`+0x64+idx*4`), per-slot occlusion queries (`+0x90+idx*8`, `GetData`-style results via `_GetQueryResults`), flag `[idx+0x75]`. Query latency means the result arrives frames later. Passes: 'Lens flares occlusions query', 'Lens flares render', `LensFlareManager::Render`, after motion blur and `PostUpdateLastFrameCamera`. The stereo flag `ViewContext+0x466` would disable lens flares (not used by us). |
| Scope | **per-eye** (queries are view-dependent) |
| Candidate mechanism | **C**: per slot already. **A**: swap per-instance slot-0 fields for every `LensFlareInstance` (many instances, awkward: prefer C). |
| Hazards | Query results lag, so a camera cut shows a wrong flare for a few frames; no reset hook found. Occlusion test is a GPU query on an eye-specific depth buffer, so sharing it between eyes is wrong. |
| Status | candidate |
| Runtime check | RenderDoc: query begin/end per eye; log slot index used in `LensFlareInstance::Update`. |
| Confidence | likely |

#### N4. Dynamic resolution

| Item | Value |
|---|---|
| Engine location | Global scale `DrawPass::s_vResolutionScaling` (vec2 at 0x1413602d8/0x1413602dc), `s_bDynamicResolutionEnabled`. Written once per frame by `FUN_1405b1270` (writes at 0x1405b16b1/0x1405b16c7): 10-sample GPU-time ring buffer fed by the `Renderer` timestamp queries (`FUN_140865330`, read in `OnFlip` by `FUN_1408651f0`), targets a frame budget, clamps to [`DAT_1413a80e0`, `DAT_1413a8010`], modes in `DAT_1413a7e08` / `DAT_1413a8568`. Not per view. Readers: TAA, `FillCSLightCB`, `LightFroxel`, `RenderSSR`, `SkyRenderer`, `IntelSSAO::UpdateConstants`, `FUN_1409120b0`. `Renderer::EnableDynamicResolution` 0x1403de740, `SetDynamicResolutionRange` 0x1405b03d0. `UpscaleDynamicResolution` 0x1408df750 ('Dynamic resolution upscale', 'upscale_simple') runs after TAA. Settings parsing logs 'Invalid dynamic resolution setting in settings.cfg : resetting to off' (`DynamicResolution::Create`), so the CVar is `Engine.Renderer.DynamicResolution`. History RTs are allocated at full size (`RenderLoop+0xc60`) and sampled with the scale; previous scale per slot at `+0x10d0+idx*8` is fed to TAA/SSR/froxel so scale changes are absorbed by constants (`likely`). |
| Scope | **shared** setting; previous scale is per-eye state (stored per slot) |
| Candidate mechanism | **Keep it off / fixed at 1.0** (`must be off`): the game must render at the exact per-eye swapchain size (`CLAUDE.md`), and a scale below 1 makes the engine render into sub-rects and upscale, which breaks our fixed eye RT size. This is a resolution-control choice, not a bypass of a temporal effect (history stays; the previous scale per slot stays correct at 1.0). Record in PLAN's Decisions log. Our own dynamic scale, if wanted, belongs in Phase 9 (the headset-side resolution). |
| Hazards | If a user leaves it on, `s_vResolutionScaling` changes frame to frame and the two eyes read the same global value (consistent per frame, fine for T1). The controller is driven by GPU timestamps and spins in `OnFlip`. Also: a first-time-on dynamic resolution flips TAA history sampling. |
| Status | candidate |
| Runtime check | Read 0x1413602d8 per frame with the CVar off (expect 1.0, 1.0); RenderDoc: no 'Dynamic resolution upscale' draw, or an identity one. |
| Confidence | likely |

#### N5. World Shift fix-up

| Item | Value |
|---|---|
| Engine location | `GameEngine::UpdateShift` 0x1404d5630 (floating-origin grid 2048 from `GameEngine+0x84..0x8c`, logs 'World Shift (%f %f)'; 65 shifts in the sample log): `PhysicsManager::ShiftOrigin`, `Game::SceneManager::ShiftOrigin` 0x1405dad20 -> `Graphics::SceneManager::ShiftOrigin` 0x1408ffec0 (`GraphNode::ShiftOrigin` 0x14086eee0 translates **both** world-matrix arrays `_s_oInstance` and `DAT_1413cda88` by -shift; 'StaticEntityShift' jobs), `Terrain::OnShift`, `GenericInstanceSpawner::OnShift`, `HHParticlesManager::OnShift`, `PublicManager::OnShift`, `World::OnShift`; then re-inits each ViewContext via `InitFromCamera` with **zero jitter** (loop over `this_00[0x450]`, `this_00+0x338`, `V_VECTOR2_0`), and records the shift in `Renderer+0xd1c/+0xd20/+0xd24` + flag `+0xd28` (later `RenderDesc+0xcec`, flag `+0xcf8`, `ShiftInfo`). Render-side temporal fix-up is **only** `PreUpdateLastFrameCamera` 0x1408dc6b0 (call 0x140848bf4), which subtracts the `ShiftInfo` vec3 (if the byte `+0xc` is set) from the stored camera positions `+0xf58+idx*0xc` and the front-pass set `+0x1000+idx*0xc`. Matrices are camera-relative, so nothing else is needed for them. Static shadow cache is shifted by `ShiftStaticShadowMap`. TAA, SSR and froxel **history textures are not touched** (their content is in screen space, so no fix-up needed). Per-object previous transforms `Mesh+0xf4` are not rebased: possibly one stale frame of motion vectors at a shift (`guess`). |
| Scope | **shared** event; the stored camera positions are per-eye state |
| Candidate mechanism | **C**: both slots are rebased by the `idx`-wise loop in `PreUpdateLastFrameCamera` automatically **if `ShiftInfo` is passed to every view of the frame** (it is part of `RenderDesc`). **A**: the saved per-eye copies of `lastCamPos` (and anything we store outside the engine arrays) must be rebased by the same vector at the same frame, or the eye whose state is parked will carry the old origin. |
| Hazards | **A parked eye state that misses a shift event is wrong by 2048 units.** In alternate-eye mode an eye is rendered every second frame: apply `ShiftInfo` to the parked copy when the event occurs, not when it is next swapped in. Our own cache of the world pose (e.g. `stereo::ViewState` previous matrices, if kept outside the engine) needs the same rebase. Treat a shift as **not** a discontinuity for history textures (the engine does not reset them) but as a rebase for positions. Also `UpdateShift` re-inits ViewContexts with zero jitter: our `InitFromCamera` hook must not re-add jitter for those calls (compare call site). |
| Status | candidate |
| Runtime check | Watch `RenderLoop+0xd08`, `+0xed8..`, `+0xf58..` and `DAT_1413a88c4` across a 'World Shift' log line and across a pause (dt == 0); observe the MV buffer on a shift frame for a one-frame glitch. |
| Confidence | confirmed (flow); guess (stale `Mesh+0xf4` glitch) |

#### N6. TAA reset flag and camera-cut handling

| Item | Value |
|---|---|
| Engine location | One shared byte `RenderLoop+0xd08`: set in the ctor 0x1408d9120 and by `RenderLoop::ClearTAA` 0x1404f5b10 (which has **no in-binary callers**: not called from `.text` we analysed, possibly Lua/vtable or never). Delivered to the resolve pass as float in `g_TAAParams.z` and cleared at the end of `RenderTAA` (`this[0xd08] = 0`). RT recreation (`InitializeTAA`, on resize or split change via `OnSplitChanged`, triggered by `Renderer+0xe39`) is the only other reset. **No camera-cut or teleport detection exists** anywhere found; under pause (dt == 0) TAA is skipped. |
| Scope | **shared flag** in the engine, but the reset must apply to **both eyes** (T4) |
| Candidate mechanism | **Mod owns the reset.** On every trigger in `CLAUDE.md` (camera cut, race restart, recenter, resolution/format change, IPD/world-scale change, stereo-mode toggle): set the flag **before each eye's `RenderTAA`** (the flag is cleared after the first slot's resolve, so a single write resets only the first slot to run; write it per eye, or call `InitializeTAA(true, 2)` to recreate both pairs, `likely`). Also clear the other per-slot histories at the same time: froxel `+0x780+idx*4 = -1` (N1), exposure first-frame flag **only if** exposure is to snap (P8), last-frame buffers via recreation (N2). A (swapped) state must also carry a per-eye reset bit. |
| Hazards | The engine never resets on a bike respawn or teleport: ghosting for several frames unless the mod detects the discontinuity (the race-restart / replay / cutscene states in `frame-anatomy.md` 8.4 are the anchors). Resetting only one slot leaves the other eye ghosting (binocular mismatch). The `ClearTAA` address `0x1404f5b10` is a safe call target only after the signature entry exists (rule 3, no hard-coded addresses). |
| Status | implemented (`src/game/taa_control.*`): the flag is written before each view's `RenderTAA` while a reset is pending. Triggers: recenter, the `taareset` command, and a camera-cut detector (view 0's game camera moves more than 15 m or turns more than 45 deg in one frame; resets this frame and the next). |
| Runtime check | Done 2026-10-21: recenter resets showed no smear in either eye (Balazs). Race-restart cut: pending. |
| Confidence | confirmed (flag and absence of callers); confirmed (runtime, recenter) for the per-eye write |

#### N7. Motion vectors as a shared transient

| Item | Value |
|---|---|
| Engine location | One MV RT (`RenderLoop+0xca8`, `GetMotionVectorsRenderTarget` 0x1404f89e0), written in `DeferredRenderer::RenderGBuffer` 0x140842040 by 'Motion Vectors' (`UpdateLastFrameCameraCB(idx,false)`, `SetupZBackViewContext`) then 'Motion Vectors front pass' (`UpdateLastFrameCameraCB(idx,true)`, `SetupZFrontViewContext` 0x1408df070), only when the per-slot motion-blur flag or TAA is on. Read by 'TAA resolve' and 'Render MotionBlur'. Format RG16 (`InitializeMotionBlur` 0x1408db950). Front pass uses its own `ViewContext` (`RenderLoop+0x1590`, `MatrixPerspectiveFovRH` with `Camera+0x8c/+0x90/+0x94`, viewport depth range [1.0, 1.0 - `RenderLoop[0x1118]`]). |
| Scope | **shared transient** (not history): each view rewrites and consumes it within its own chain |
| Candidate mechanism | None needed under **C** or **A** as long as one eye's chain finishes before the next starts. |
| Hazards | A mechanism that interleaves eyes pass-by-pass (e.g. both GBuffers first, then both TAA) would overwrite it. The front-pass ViewContext needs the same per-eye projection and view as the main one (its fov/near/far are separate camera fields): for VR the front pass projection must also take the OpenXR per-eye FOV or the bike foreground gets wrong motion vectors (`likely`, matrix details in `camera.md`). |
| Status | confirmed (runtime, single view) for the observed resource, binding and constants; per-eye behaviour and mechanism: candidate |
| Runtime (f8323) | Motion-vector draws: Colour Pass #4, events 20321..20633 (22 DrawIndexed), RT `3399` R16G16_FLOAT (RTV 3402), depth 339 bound; after the GBuffer chunk and before the linear-depth CS (20649). Read by TAA (36731, t3) and motion blur (36833). RT size not read (assumed 1920x1080). The front-pass MV draws were not distinguished. |
| Runtime check | RenderDoc: MV RT contents per view; front-pass cb. |
| Confidence | confirmed (placement); likely (front-pass hazard) |

#### N8. Shared per-frame globals that are not history but gate it

| Global | Address | Role | Hazard |
|---|---|---|---|
| `s_eAntiAliasingMode` | 0x141407dc8 | TAA / FXAA mode; gates jitter, TAA, `ResetProjectionOffset` | write nothing; read to know TAA is on |
| jitter parity counter | 0x1413a88c4 | selects x/y phase; +1 per `DoUpdate`, held at dt == 0 | one increment per frame (P2) |
| `ViewContext::s_bMotionPause` | 0x14140bb30 | set from dt == 0 in `RenderLoop::SetTime` 0x1408dedd0; freezes previous-matrix latching and per-object advance | do not update history while paused (consistent for both eyes) |
| `RenderLoop+0x20` / `+0x24` | frame dt ms / 60-frame average | 0 disables TAA and motion blur; blur strength | keep game dt, not headset dt |
| `DAT_141407830[idx]` | per-slot motion-blur enable | MV pass condition and `RenderMotionBlur` gate | slot 1 flag must match slot 0 |
| `s_vResolutionScaling` | 0x1413602d8 | see N4 | fixed 1.0 |
| `RenderLayer+0x30` / `+0x34` | render id / flip id | `frameId` candidate | per-eye `BeginRender` bumps +0x30 (P7) |
| `Renderer+0xec8` | frame counter (ring of 4) | GPU timing slots | not history |

---

## 4. Mechanism A: field list to swap (slot 0 around each eye render)

Only valid if the render is done as camera index 0 for both eyes (alternate-eye milestone, Phase 4). For eye E, before its `Render`: save slot 0's current contents into the other eye's parked set and restore E's set; after: store back. Fields, with the shared flags that must be handled outside the swap:

- `RenderLoop`: `+0xce8` and `+0xcf0` (TAA RT pointers, swap the **pointers**, the RTs stay allocated), `+0xed8` (0x40), `+0xf58` (0xc), `+0xf70` (8), `+0xf80` (0x40), `+0x1000` (0xc), `+0x1018` (8), `+0x1028` (0x40), `+0x10a8` (0xc), `+0x10c0` (8), `+0x10d0` (8), `+0x10e0` (8), `+0x10f0` (8).
- `DeferredLighting`: `+0x780` (4), `+0x760..+0x778` (the four 3D texture pointers, i.e. `(pp*2+0)*8` for pp = 0,1), `+0x920` (4).
- Tonemap: `+0x138` (8), `+0x14e` (1), the `+0x30` block (0x60) **only if** exposure is per-eye; for shared exposure do the opposite (all eyes use one slot, nothing swapped, and the adaptation step runs once, see P8).
- Shared, handled separately: reset flag `RenderLoop+0xd08` (set per eye, N6), jitter counter `DAT_1413a88c4` (P2), `ShiftInfo` (N5), per-slot CSM state (P10), lens flares per instance (N3, many objects), `SkinMesh+0x158/+0x160` validity (P7).

Costs and risks of A: more fields to keep in step with future game updates (rule 3: signatures only), the swap must happen on the thread that records (main) and before any record that captures the pointers (record-then-execute: per-eye constants are baked at record time, but **history RT bindings are resolved at record time too**: pointer swaps must happen before the recording call for that eye, and nothing may read them later on a deferred context). `guess`, `runtime-check`.

---

## 5. Hypothesis C: requirements and hazards (the part that decides it)

C = eye k -> engine camera index k, using the existing two-view split path. Requirements and what is known:

| # | Requirement / hazard | Evidence | Confidence |
|---|---|---|---|
| H1 | **Split mode must be enabled in-race.** Answered statically 2026-10-16: renderer-only recipe in `frame-anatomy.md` 5.1 (no `GameEngine::SetSplitMode`). `PossibleSplitMode` (Lua video config) must permit mode 1; `SetSplitModeOn` refuses in network sessions; how single-player enables it and what fills `Renderer+0xe50` is unknown. We may be able to call `GameEngine::SetSplitMode` 0x1404d4710 / `RenderTargetManager::SetPossibleSplitMode` 0x1408e2080 and write `Renderer+0xe48/+0xe50` ourselves. | frame-anatomy.md section 5 | confirmed (mechanism exists); open (enabling) |
| H2 | **Eye RT size.** Split mode halves the width (`TransformViewport`, `ChangeRenderTargetsSize`; `ComputeSplitSize` has no callers); answer: back buffer 2 x eyeW wide (`frame-anatomy.md` 5.1), no patch. | `RenderTargetManager` trio | confirmed (halving); runtime-check (patch options) |
| H3 | **Exposure per slot but must be shared** (P8). Needs an explicit sync. | | confirmed (per-slot); the fix is a candidate |
| H4 | ~~Jitter is disabled in split mode~~ **refuted 2026-10-16**: the gate is true in mode 1 when view 1 is an ordinary view (`frame-anatomy.md` 5.1). Both views share the phase, which T1 allows. | disasm 0x1405adfc6..0x1405adfdc | confirmed (static); runtime-check |
| H5 | **TAA reset flag is a single byte** (N6): per-eye resets need a per-eye write or `InitializeTAA(true,2)`. | | confirmed |
| H6 | **No camera-cut reset in the engine** (N6): the mod owns every discontinuity trigger. | | confirmed (static absence) |
| H7 | **Dynamic resolution must be off/fixed** (N4). | | likely |
| H8 | **Slot 1 allocation is lazy** (`OnSplitChanged`); the history RT size comes from `RenderLoop+0xc60`. | | confirmed |
| H9 | **CSM/probes/culling run on the union of views; per-slot cascades may be duplicated** (P10, P11). Culling with the union is good (both eyes see the same set). | | likely |
| H10 | **Per-object previous transforms are idempotent per `frameId`** (P7), good under C because the engine itself expects two views per frame. | | confirmed (mechanism); open (`frameId`) |
| H11 | **HUD and composition**: the engine composes split HUD itself ('Split border', 'Split n HUD', `CopySplitRT`); for VR we want each eye's scene separately and the 2D HUD on a quad (`frame-anatomy.md` section 8). | | confirmed |
| H12 | **Per-view data is baked at record time**: per-eye `ViewContext` must be final before `RenderLoop::Render` (via `InitFromCamera` hook or after `DoUpdate`). | | likely |
| H13 | **The engine treats the two views as two players**: any per-player assumption (HUD lists per camera `Renderer+0xdf8+idx*8`, `GetCamera(i)` player array `+0x3b8` stride 0xc0, `KTUCamera` per player, wind per idx `Wind::GetStrength(idx)`) may break for two eyes of one rider. | | guess |
| H14 | **Max 2 slots**: the 4-view mode (mode 3) overflows the arrays; irrelevant for us. | | confirmed |

If H1 or H2 cannot be solved, fall back to A for Phase 4/5. If A's swap proves fragile, B1 covers TAA + SSR only and the rest (last-VP, froxel, exposure, lens flare) stays shared, which would violate T1 for those rows: stop and show the evidence per `CLAUDE.md` rule 1.

---

## 6. Summary table for PLAN's inventory

| PLAN row | This file | Scope | Engine location (short) | Preferred candidate | Status |
|---|---|---|---|---|---|
| TAA history colour (incl. ping-pong) | P1 | per-eye | `RenderLoop+0xce8/+0xcf0 + idx*0x10` | C, else A (pointer swap), else B1 | confirmed (Phase 6): own-view history only (11.1), leak test PASS, reset and cut tests (11.3) |
| TAA jitter index / offset | P2 | per-eye | `DAT_1413a88c4`, `ViewContext+0x448`, `RenderLoop+0xf70` | hook `InitFromCamera`, add jitter per eye | confirmed (Phase 6): previous jitter = own frame N value, same phase both eyes (11.1) |
| Previous view/proj/VP | P3 | per-eye | `RenderLoop+0xed8`, `+0xf58`, `+0xf80..` | C | confirmed (Phase 6): previous VP / camera = own frame N values (11.1); reset latches previous = current (11.3) |
| Previous depth / Hi-Z | P4 | per-eye (no Hi-Z found) | `RenderLoop+0x10e0` | C | confirmed (Phase 6, bindings, 11.1); functional leak test inconclusive: SSR invisible on the dry road (11.4) |
| Temporal AO / SSR / volumetric | P5 (+N1, N2) | per-eye | SSR via N2; froxel via N1; AO stateless (likely) | C | confirmed (Phase 6) via N1 / N2; SSAO is off in VR and stateless |
| Motion blur camera matrices | P6 | per-eye | `RenderLoop+0xed8`, `+0xf58` | C | confirmed (Phase 6): motion-blur constants carry the own view's previous VP (11.5); motion blur off in VR by decision |
| Per-object previous transforms | P7 | shared | `Mesh+0xc4/+0xf4/+0x70`, `SkinMesh+0x158` | engine idempotent per `frameId`; verify source | confirmed (Phase 5): Mesh once per frame, SkinMesh fixed (5a) |
| Auto-exposure | P8 | shared (engine: per-slot) | `tm+0x130/+0x138/+0x14e/+0x30` | sync slot 1 to slot 0 | confirmed (4c; 11.1): both views compose with view 0's same-frame luminance; not reset (Decisions log) |
| GPU particles / simulations | P9 | shared | `HHParticlesManager` | engine idx == 0 guards | confirmed (Phase 5): once per frame, medium 2 kept on (5d) |
| Shadow cascades | P10 | shared (engine: per-slot) | `CSM+0x25b0 + idx` | force slot 1 = slot 0; verify fit | confirmed (5c; 11.1): one atlas per frame, read by view 1 in the same frame |
| Probes | P11 | shared | `ProbeRenderer::Update`, `RenderProbe` | once per frame | confirmed (Phase 5): once per frame |
| (new) Froxel history | N1 | per-eye | `DL+0x780`, `+0x760`, `+0x920` | C | confirmed (Phase 6): own-view history (11.1), leak test PASS (11.4), reset = index -1 (11.3) |
| (new) Last-frame colour/depth | N2 | per-eye | `RenderLoop+0x10e0/+0x10f0` | C | confirmed (Phase 6, bindings, 11.1); reset clears both slots (11.3); leak test inconclusive (11.4) |
| (new) Lens-flare queries | N3 | per-eye | `LensFlareInstance+0x64/+0x90` | C | per-slot by design; view 1's own flares since 6e (10.4); visual check open (no flare at the start line, 11.6) |
| (new) Dynamic resolution | N4 | shared setting | 0x1413602d8, `FUN_1405b1270` | off / fixed 1.0 | held at 1.0 in VR (6d); the player-setting-on case not exercised (11.6) |
| (new) World Shift fix-up | N5 | event | `PreUpdateLastFrameCamera` 0x1408dc6b0, `ShiftInfo` | rebase parked per-eye positions | confirmed (Phase 6): shifttest PASS (11.3) |
| (new) TAA reset flag | N6 | mod-owned | `RenderLoop+0xd08`, `ClearTAA` | write per eye on every trigger | confirmed (Phase 6): full reset and in-ride cut triggers (11.3) |
| (new) MV RT transient | N7 | shared transient | `RenderLoop+0xca8` | none | confirmed (Phase 6): rewritten and read within each view (11.1); helmet vectors at idle, 11.7 |

Phase 6 (2026-10-21): the Status column above holds the Phase 6 result per row (evidence in section 11).

Rows tagged `confirmed (runtime, single view)` are observed once in capture f8323; nothing above is `confirmed` in the PLAN sense (per-eye cross-leak test T2). Phase 6 turns each row to `confirmed` with a runtime evidence line (RenderDoc event, debugger watch, or a passing cross-eye leak test, T2).

---

## 7. Contradictions between source notes and what was used

| Topic | Conflict | Reading used |
|---|---|---|
| Exposure state | Render-loop note: one tonemap object, no camera key, double adaptation per frame with two views. Temporal note: history is per camera index. | Per-slot (temporal note, later and more detailed): no double step, but independent adaptation per eye. |
| FOV axis (affects P2 and N7 jitter units and the front pass) | Camera note: `Camera+0x78` vertical FOV. Projection note: `MatrixPerspectiveFovRH` 0x140813390 treats it as **horizontal** (`m[5] = m[0] * aspect`). | Horizontal (decompile algebra). Jitter in projection units does not depend on the axis; the front-pass FOV (`+0x8c`) follows the same convention. |
| Jitter argument position | "3rd argument" vs "4th argument" of `InitFromCamera`. | Same parameter, counted with/without `this`. |
| ToneMapper once per view | Render-loop note flags double-stepping as a hazard; the per-slot arrays mean each slot steps once per frame (still once per eye = twice per frame in total for the head, which is the sharing problem). | See P8. |
| CSM scope | Render-loop note: `guess` whether cascades are per view or merged; temporal note: per-camera-index caches. | Per-slot state exists; fitting policy unverified. |
| Markers (affects how temporal passes are found in a capture) | Capture note assumes annotation markers; render-loop note says they may be absent. | Assume absent; find passes by shader hash and bound SRVs (`frame-anatomy.md` section 9). |

---

## 8. Open questions (all need runtime data)

1. Where does `frameId` (param 2 of `Mesh::UpdateRenderData`, param 3 of `FillRenderInstances`) come from, and is it identical for both views of one frame?
2. Does froxel temporal reproject with a matrix? (Offline DXBC disassembly of `csLightFroxelTemporal`, or RenderDoc.)
3. What are `RenderLoop+0xc60` (TAA/last-frame RT size) and `Renderer+0xc60/+0x640` (jitter gate) in split mode; does split mode half-size the history RTs?
4. Why does `ClearTAA` have no callers (Lua/vtable or dead)? Camera-cut handling is then absent in the engine.
5. Exact formats: resolved by f8323 (TAA textures B8G8R8A8_SRGB, so 'RGBA' 0x41424752 is not R8G8B8A8_UNORM at runtime; 'Last Frame Buffer' RGBA16F, consistent with 0x464c4834). Remaining: does the TAA role rotation (3268 / 3272 / 3276) repeat every frame or every 3 frames? (needs a multi-frame capture.)
6. Is `Mesh+0xf4` stale at a World Shift (MV glitch frame)?
7. What do tonemap params `tm+0x30+idx*0x60` and `param_2+0x2c` hold; is pre-exposure applied before the last-frame colour copy?
8. CloudShadow, probe relight cadence and PopcornFX GPU-sim internals are not decompiled and are treated as view-independent by inference.
9. Does `ViewContext+0x464` follow the slot or the camera, i.e. can a single-view render target slot 1?
10. Are cascades fitted per view or merged in `CascadedShadowMapRenderer::SetupRenderContext` 0x140828320, and who issues the cascade draws?

---

## 9. Runtime evidence (RenderDoc capture f8323)

Capture: `C:\dev\tt2vr-re\rdc\tt2_snaefell_helmet_moving_taa_f8323.rdc` (outside the repo), frame 8323, helmet camera, moving, AA = FXAA_TAA, dynamic resolution off, 1920x1080, **one view**. Event ids are RenderDoc ids. Pass order and the full table: `frame-anatomy.md` section 4.1. Constants below are the main-loop values read at event 36731. Tag: `confirmed (runtime, single view)` unless noted.

### 9.1 TAA resolve (event 36731, pixel shader hash acdcd43e-1d326109-4826a244-123b0dde)

| Item | Value | Confidence |
|---|---|---|
| Textures | **Three** B8G8R8A8_SRGB textures: history SRV t2 `LastFrameBuffer` = `3276`; outputs o0 = `3268` (raw current sample, alpha = no-MV flag) and o1 = `3272` (TAA-resolved). Result `3272` is copied into `3101` at event 36758. This corrects P1 ("pair", R8G8B8A8). Whether `3268`/`3272`/`3276` rotate across frames (3-way) is **unverified** | confirmed (runtime, single view); rotation unverified |
| Other SRVs | t0 `DepthBuffer` = depth 339 (D32_FLOAT_S8X24, SRV 340); t1 `FrameBuffer` = `3105` (film-grain output, current jittered frame); t3 `MotionVectors` = `3399` R16G16_FLOAT (written by events 20321..20633). Samplers s0 PointClamp, s3 LinearClamp. Depth test disabled | confirmed (runtime, single view) |
| Size | 1920x1080 (cbViewportStd viewport (1920, 1080, 1, -1), resolutionScaling (1, 1, 1, 1)) | confirmed (runtime, single view) |
| Constants | `$Globals` 208 bytes (resource `3903`, ps cb slot 7) + `cbViewportStd` (resource 108, slot 1) | confirmed (runtime, single view) |

`$Globals` layout (byte offset: name): 0 `TrenchingColor` (uninitialised, ignore), 16 `g_invViewProj`, 80 `g_viewProjLastFrame`, 144 `g_cameraPosition`, 160 `g_cameraPositionLastFrame`, 176 `g_jitter`, 192 `g_TAAParams`. Confirmed (runtime, single view).

| Constant | Value (event 36731) | Reading |
|---|---|---|
| `g_jitter` | (0, -0.00046296295, 0.00026041668, 0) | cur = (0, +1/1080), prev = (+1/1920, 0) after undoing (x*0.5, y*-0.5, x*0.5, y*-0.5). Matches the static formula (P2); this frame: current = vertical phase, previous = horizontal phase. Confirmed |
| `g_TAAParams` | (1, 1, 0, 0) | prevResScale xy = 1, reset flag z = 0, w unused. Matches P1 |
| `g_cameraPosition` | (-211.20682, -14.257798, 50.707253, 1) | equals cbCameraStd.viewPosition |
| `g_cameraPositionLastFrame` | (-210.17304, -13.224817, 50.830555, 0) | per-frame delta (1.034, 1.033, -0.123) m |
| `g_viewProjLastFrame` | rows (memory order) (-0.76377, 0.79589, 0, 0), (-0.38954, -0.37382, 1.88524, 0), (2.7783e-5, 2.6662e-5, 1.1027e-5, 1.00004), (-0.69363, -0.66564, -0.27531, 0) | **jitter-free**: row0 . R2 and row1 . R2 ~ 0 (R2 = -row3). Camera-relative. Confirmed |
| `g_invViewProj` | un-jittered (differs from cbCameraStd.viewProjToCameraCentered, which carries the jitter) | confirmed |

Shader behaviour (read from the disassembly, not reproduced here): reprojection by depth through `g_invViewProj`, add `g_cameraPosition - g_cameraPositionLastFrame`, project with `g_viewProjLastFrame`; if the motion-vector texel is non-zero the MV path is used instead; current frame sampled at uv - `g_jitter.xy`, history at the reprojected uv - `g_jitter.zw`; 3x3 min/max neighbourhood clamp and blend. The depth value is fed to `g_invViewProj` as ndc z (see `conventions.md` 3.7: stored world depth is 0.95 * ndc_z; compensation unresolved). Confidence: confirmed (runtime, single view) for the behaviour read; the resulting precision effect is open.

### 9.2 Motion vectors, exposure, last-frame resources, motion blur

| Resource / pass | Evidence | Confidence |
|---|---|---|
| Motion vectors | RT `3399` R16G16_FLOAT (RTV 3402), written by events 20321..20633 (Colour Pass #4, 22 DrawIndexed, depth 339 bound); consumed by TAA 36731 (t3) and motion blur 36833. Size not read | confirmed (runtime, single view); size unread |
| Exposure chain | 36541 -> `3372` R16_FLOAT 16x16 (reads `3368` R16F); 36570 (likely 4x4, guess); 36606 -> `3385` R16_FLOAT 1x1 (reads `3376` and `3380`, both R16_FLOAT: blend with previous luminance); read by tonemap 36652 as `LuminanceTexture_tex` (t2). The history ping-pong across frames was not observed | confirmed (runtime, single view) for the chain and formats; ping-pong unverified |
| Last Frame Depth | event 35759: depth 339 (SRV 340) -> `3284` R32_FLOAT 1920x1080 | confirmed (runtime, single view) |
| Last Frame Buffer | events 35786, 35819, 35842, 35865, 35888, 35911, 35934, 35957: `3288` RGBA16F, 8 mips (mip0 1920x1080, mip7 15x8), source HDR scene `3591` | confirmed (runtime, single view) |
| Motion blur | event 36833: `DepthBuffer_tex`, `FrameBuffer_tex`, `MotionVectors_tex` (`3399`), cbCameraStd (resource 113), `g_viewProjectionLastFrame`, `g_motionBlurParam1/2` -> `3105`; after TAA | confirmed (runtime, single view) |
| Froxel history | event 32784 (Light Froxel CS) binds `LastFrameLightedFroxel_tex` (3D) next to `LightedFroxel` (UAV); reprojection method not examined | confirmed (runtime, single view) that the history SRV exists; method open (N1) |
| SSR | CS 20770: `SSRTempRead` -> `SSRResult`; last-frame depth/colour bindings not verified | likely |

### 9.3 Not confirmed by the capture

- Per-eye behaviour of every row (the capture has one view); rows keep `candidate` for the mechanism.
- Role rotation of `3268`/`3272`/`3276` across frames, exposure ping-pong, froxel ping-pong, jitter alternation over 4 frames.
- `NoMotionVectorFixTAA` = event 36786 (inputless PS, dual RT `3268`/`3272`, depth 339 bound) is a **candidate** only (guess).
- No separate FXAA pass was seen with AA = FXAA_TAA; see `frame-anatomy.md` section 4.2.

### 9.4 Two-view frame trace (tt2vr frame trace 12770, 2026-10-16)

Split mode 1 through the mod (Phase 4a: both views from eye 0's pose, 1770x1892 per view, AA = FXAA_TAA, riding). Trace indices are from `frametrace_12770.txt` (not in git); shader names are our xxh64 hashes. Status: `confirmed (runtime, two views)` for the observation.

| Row | Observation | Index |
|---|---|---|
| P1 TAA | 'TAA resolve' (ps `fb237089f5d3dd39`) per view: view 0 history t2 `res818` -> RTs `res807`/`res809`; view 1 history `res1678` -> `res1674`/`res1676`. Six distinct textures, neither history is written earlier in the frame: **per-slot history, no cross-eye read**. Depth `res85`, current colour `res795` and MV `res407` are shared transients rewritten per view. | 023082, 046056 |
| N7 MV | One R16G16 MV RT for both views, cleared before each view (22 dynamic-object draws each); view 1's clear comes after view 0's TAA. | 015740, 038779 |
| P4/N2 last frame | Last Frame Depth `res481` / `res1646`, Last Frame Buffer `res483` / `res1648` per view. | |
| N1 froxel | view 0 reads `res1362` writes `res1358`; view 1 reads `res1715` writes `res1713`. | |
| P8 exposure | 1x1 luminance: three rotating textures; each view blends against its own previous (`res756` / `res1672`): **independent adaptation per eye** (PLAN requires shared: Phase 4c). View 1's 'HDR_FinalCompose' uses a different PS permutation (`e0d02d95bd7162df` vs `c654fff3722b5843`), cause unknown. | 022975, 045949 |
| P10 CSM | 6144x2048 map cleared and rendered **per view**, with only 2 cascades (viewports x = 0 and 2048, none at 4096): the split `CascadeCount` cap (`frame-anatomy.md` 5.1); lifted by the mod from the next build. Cascade constants differ per view (fitted per view). | 016157, 039196 |
| P2 jitter | Not visible in the first 16 floats of the TAA constant buffer (trace records 16 floats); open. | |
| HUD | After view 1: 80 draws at 3540x1892 into the back buffer with view 1's camera buffer (the global HUD list across both eyes). | 046411..047417 |

- 2026-10-20, Phase 4c (`game::SharedExposure`): frame trace 4400 (stereo, riding) shows both views' `HDR_FinalCompose` (ps `c654fff3722b5843`) binding the same luminance texture `srv1718` at t2; view 1 no longer runs `HDR_Luminance*`. **P8 shared exposure: confirmed (runtime, two views).**

---

## 10. Phase 6 static findings (2026-10-21)

Ghidra decompile/disasm of the snapshot exe (query server); names and addresses in `symbols.md` sections 1b, 6, 7. Confidence per item.

### 10.1 What a full history reset needs (T4)

| Row | Reset mechanism | Evidence | Confidence |
|---|---|---|---|
| P1 TAA | shared byte `RenderLoop+0xd08` before each view's `RenderTAA` (in place since 4d) | N6 | confirmed |
| N1 froxel | write -1 to `DeferredLighting+0x780+idx*4` for both idx before the views render: `LightFroxel` then runs the non-temporal technique once, sets the index to 0 and continues temporally from that frame's volume. The stale volume is never read, so no clear | `LightFroxel` 0x140839120: `if (pp[idx] == -1) {... new = 0} else new = (old+1)&1` | confirmed |
| P3/P6 previous camera | call `PostUpdateLastFrameCamera(renderLoop, renderLoop+0x1f00)` right after `RenderContext::Prepare(view)` for each view: slot idx latches previous = current, so the cut frame's motion vectors, SSR and TAA reprojection see zero camera motion instead of the jump. The engine's own call after motion blur still runs | `PostUpdateLastFrameCamera` 0x1408dc2f0 reads only the prepared RenderContext; first readers are the MV passes in `RenderGBuffer` | confirmed (static); runtime check |
| P4/N2 last-frame colour/depth (SSR) | no engine validity check exists: SSR samples whatever the slot's last-frame RTs hold (the pre-cut frame). Candidate: clear both slots' 'Last Frame Depth' to the far value and 'Last Frame Buffer' to 0 before the cut frame executes (rays find no hit, as on fresh RTs); A/B in the cut test | `DeferredLighting::RenderSSR` 0x14083bd50 gated only by `DL+0x21+idx` (last frame's CVars) | confirmed (no check); the clear's visual result is a runtime check |
| P8 exposure | shared since 4c; `tm+0x14e` (word 0x0101) snaps both slots on the next luminance pass. The engine never snaps on restarts or cuts (only init and a debug menu item) | 10.1 decision in PLAN | confirmed |
| N3 lens flares | queries and fades are per slot; nothing to reset (the engine has none; results lag a few frames after a cut) | | likely |

**The two-view toggle is not a full reset.** `OnSplitChanged` 0x1408dc2b0 = `InitializeTAA(true, p)` + `InitializeLastFrameBuffers(p)`: both slots' TAA pair and last-frame RTs are recreated (uninitialised content), but the TAA reset byte is **not** set, and the froxel index and volumes, `lastVP`/`lastCamPos`/`lastJitter`, the previous resolution scale, the tonemap state and the SSR flags persist (slot 1's from the previous two-view session). The mod must treat "two views switched on" as a reset trigger. `confirmed (static)`.

### 10.2 World Shift (N5)

- Order: `GameContext::RenderOneFrame` calls `GameEngine::UpdateShift` (vtable +0x420, only if `GameEngine+0x48 == 1`) **before** `Renderer::DoUpdate` (0x14048c536) in the same frame, so DoUpdate's `InitFromCamera` already sees the shifted camera. `UpdateShift`'s own `InitFromCamera` calls (0x1404d5a41) run outside DoUpdate, so the mod's view override and cut detector ignore them. `confirmed`.
- Flag: `Renderer+0xd28` (= RenderDesc+0xcf8, same bytes) is cleared at the start of every `UpdateShift` and set with the vector (`+0xd1c..+0xd24`, grid cell x 2048) after a shift: a one-frame pulse. `confirmed`.
- Rebase: `PreUpdateLastFrameCamera` (call 0x140848bf4, per view inside the TAA/post branch) subtracts the vector from **its own slot's** `lastCamPos` and front-pass position and clears nothing, so in a two-view frame each slot is rebased exactly once. `lastVP` is camera-relative and needs nothing. A view that skips the TAA/post branch would keep the old origin (not the case while TAA is on). `confirmed`.
- **Mod hazard:** the camera-cut detector compares the game camera position frame to frame; on a shift frame it moves by about 2048 m and the detector resets both eyes' TAA. The vector must be subtracted from the stored position when the flag is set. `confirmed (static)`; runtime check.
- `Mesh+0xf4` (previous world) is not rebased: possibly one frame of wrong object motion vectors at a shift (engine behaviour, flat too). `guess`; runtime check.

### 10.3 Dynamic resolution (N4)

The only writer of `DrawPass::s_vResolutionScaling` (export, 0x1413602d8) and `DrawPass::s_bDynamicResolutionEnabled` (export, 0x1414082ea) is the controller `FUN_1405b1270`, called once per frame from `Renderer::OnRender` (0x1405af618) after DoUpdate and before the render loop. Mode 0 writes exactly 1.0 / 1.0; modes 1 and 2 scale. `UpscaleDynamicResolution` runs whenever the enabled flag is set (even at scale 1.0). All readers except `LensFlareInstance::Update` (DoUpdate) are inside the render loop. So writing scale 1.0 / 1.0 and enabled = false in the `DeferredRenderLoop::Render` pre-hook covers every reader in that frame and the next DoUpdate; the controller rewrites both from the untouched mode CVar once the mod stops writing. Do not write the mode CVar (0x1413a7e08: loaded from UserSettings; the Ansel-style restore `FUN_1405aee10` only knows modes 0 and 2). `confirmed (static)`.

### 10.4 Lens flares (N3) and the HUD-list swap

Each camera's flare sprites are HudNodes that `LensFlareManager::Update` (in DoUpdate, per camera index) adds to that camera's HudItemList 2D root; the occlusion queries and 'Lens flares render' are drawn per view by `LensFlareManager::Render` with the camera index. Since 4b the mod points view 1's `ViewContext+0x5e8` at camera 0's list during `Render` (the dashboard is a 3D list in camera 0 only), so **eye 1 draws eye 0's flare sprites**: a T2-style cross-eye leak of view-dependent state. Fix: keep view 1's own list and, for the duration of `Render`, append camera 0's `HudList3D*` entries (arrays `+0x1b8` / `+0x1c8`) to camera 1's list, restoring the array headers after. `confirmed (static)`; runtime check.

### 10.5 MeshPainter

`MeshPainter::Update` (per view, unguarded latch) is the livery editor, active only while painting (`this+0x38`); not a temporal effect, and the second view paints nothing. No action. `likely`.

---

## 11. Phase 6 runtime evidence

### 11.1 History audit, two consecutive frames (6a, 2026-10-21)

Test mode (simulated headset, eye 1770x1892, IPD 0.05 m from the profile), Time Attack start line, bike idle, AA FXAA_TAA, `[capture] cb_floats = 64`. Remote `frametrace 2` wrote frames **10260** and **10261** (not in git). Checker: a scratch script (not in the repo) that tracks every draw/dispatch/copy/clear's bound SRVs, RTVs, UAVs and DSV per recording context, matches resources across the two files by pointer, and splits each frame at the per-view first dispatch `cs:4f3101da75386a3b`. A "history read" is a read in frame N+1 of a resource that frame N+1 has not written yet; its frame-N writer decides the verdict. `confirmed (runtime, two views)` for everything below.

| Verdict | Count | Resources |
|---|---|---|
| T1, history from the **same** view's frame N | 9 | per view: 'Last Frame Depth' R32 and 'Last Frame Buffer' RGBA16F 8 mips (read by `cs:4ba56b47c32e768f`, SSR; view 0 `res444`/`res446`, view 1 `res2079`/`res2081`), froxel volume tex3d 111x119x32 RGBA16F (`cs:d0b5b98b2772efab`; `res1039` / `res2084`), TAA history (`ps:fb237089f5d3dd39` t2; `res1682` / `res2106`); view 0 only: previous luminance 1x1 R16F (`ps:80abe7e4b00b6cfa`, `HDR_LuminanceFinal`) |
| T2 violation, history from the **other** view | **0** | |
| History written by the per-frame segment | 0 | |

Same-frame reads of view 0's output by view 1 (not history): the shadow atlas 12288x4096 (shared since 5c), the 1x1 luminance (shared since 4c, both `HDR_FinalCompose` bind `res1701`), and a read-only depth test against the 3540x1892 window depth that view 0's split copy cleared for the whole window (view 1 tests only its own half, viewport x = 1770).

TAA constants (`$Globals`, PS slot 7, 52 floats) frame N -> N+1, per view:

| Check | View 0 | View 1 |
|---|---|---|
| `g_cameraPositionLastFrame`(N+1) = `g_cameraPosition`(N) | exact (diff 0) | exact |
| ... the other view's `g_cameraPosition`(N) | differs by 0.050 m (the IPD) | 0.050 m |
| `g_jitter.zw`(N+1) = `g_jitter.xy`(N) | (0.000282, 0) = (0.000282, 0) | same |
| `g_jitter.xy` alternates | (1/1770, 0) -> (0, -1/1892) | same phase as view 0 |
| `g_viewProjLastFrame`(N+1) x `g_invViewProj`(N) | identity to 2.5e-6 | 2.6e-6 |
| ... with the other view's `g_invViewProj`(N) | 0.36 off identity | 0.36 |
| `g_TAAParams` | (1, 1, 0, 0): previous scale 1, no reset | same |

So for P1 (TAA history), P2 (jitter), P3 (previous VP / camera position), P4 + N2 (last-frame depth and colour), N1 (froxel) and the shared rows P8 (luminance), P10 (shadows) and N7 (MV RT, rewritten per view: no history read) the per-eye split holds at the D3D level and in the constants. P6 (motion blur) reads the same `lastVP[idx]` as TAA (static); its own constant buffer is checked with motion blur on in 6g.

### 11.2 Cut detection misses in-ride cuts (2026-10-21)

Same session, after the bike moved off (one of A / X is the pad's gear-up) and crashed: the switch to the crash camera and the respawn back into the helmet camera produced **no** "camera cut" line, so neither reset the history. The 15 m / 45 deg thresholds only catch menu/loading jumps (log: 180.7 m, 1574 m, 1512 m, 3.8 m with 131 deg). Fix in 6b: a speed-relative distance limit, a lower turn limit, and the player camera's active mode (`KTUCamera+0x48` / `+0x68`) changing as a trigger.

Also seen in the crash camera: **the left eye is desaturated (grey) and the right eye is in colour**. A crash post effect reaches camera slot 0 only (not temporal; per-slot post-process state, `renderer.md` 6). Recorded for Phase 8 (crash and chase cameras).

### 11.3 Full reset, cuts and World Shift (6b, 6c; test mode, 2026-10-21)

- **Reset coverage** (log "a history reset covers TAA, froxel (index -1), previous camera (latched per view) and SSR's last-frame buffers (2 of 4 cleared)": slot 1's buffers exist only with two views). The convergence reference resets TAA only.
- **Triggers seen:** "two views on" at ride entry; camera-mode changes (menu -> riding); the reworked distance / turn detector caught the crash camera (4.3 m, limit 3.5, 16 deg), the respawn (6.8 m, 7.0 m, 7.7 m, 16-19 deg) and pairs of crash + respawn on every attempt; **no false reset** in 10 s idle or 40 s of riding up to 144 km/h. Limits after the first runs: 2 m + 1.5 x speed x dt, 10 deg.
- **Cut test:** A (reset) after a respawn: frames +1 / +2 clean in both eyes; B (`cuttest noreset`, crash camera): no visible smear either (the neighbourhood clamp hides most of it), so the reset matters mainly for SSR, froxel and motion blur, which have no clamp.
- **World Shift** (`shifttest` PASS): shift (0 -2048 0) while riding at about 67 km/h; the cut detector rebased its stored position (camera step 0.31 m, 2048 m without) and did not fire; both slots' previous camera positions 0.31 m from the current after `PreUpdateLastFrameCamera` (one frame of motion); 0 resets in the 12 frames after; frame-to-frame change 8-18 / 255 (normal riding); screenshots clean.

### 11.4 Leak tests per row (6g)

| Row | Tint (eye 0's history) | Left eye | Right eye | Result |
|---|---|---|---|---|
| TAA (P1) | eye 0's TAA input red for one frame | 0.0% -> 67.9% red | +0.00% | PASS |
| Froxel (N1) | slot 0's froxel volume written this frame cleared to (2, 0, 0, 0.05) through a UAV | red excess +6.13 / 255 | +0.006 | PASS |
| SSR (P4, N2) | slot 0's 'Last Frame Buffer' (all mips) cleared to red (8, then 512 HDR) between frames | +0.023 / +0.006 | +0.039 / +0.018 | INCONCLUSIVE: SSR adds nothing visible on the dry start-line road; the per-slot binding audit (11.1) is the evidence. Re-test on a wet road |

### 11.5 Motion blur (P6), test mode with `[xr] motion_blur = true`, traces 9740 / 9741

'Render MotionBlur' = `ps:d325d05c25da1a12` (reads depth, colour and the MV RT after TAA). In both frames and both views its `$Globals` (PS slot 7) holds, from float 12, the **same** previous VP as that view's 'TAA resolve' `g_viewProjLastFrame`, and from float 44 its previous camera position; neither view's buffer contains the other view's matrix (0.36 apart). Slot 6 carries each eye's own asymmetric projection (m02 -0.269 / +0.269).

### 11.6 VRAM, debug views, open visual checks (6d-6f)

- **Per-eye history VRAM** (log, eye 1770x1892): 78.8 MB per eye = TAA pair 25.5, last-frame depth + colour 46.8, froxel volumes 6.4, previous luminance < 0.01; the debug copies and thumbnails add 27 MB while shown.
- Debug views (overlay "Temporal (Phase 6)", remote `views on`): history, motion vectors and previous depth per eye render; `freeze on` softens eye R during a head shake while eye L stays sharp.
- Lens flares (6e): the dashboard still draws in eye 1 (camera 0's 3D lists); no flare sprite was on screen at the start line (both views draw the same 7 post-TAA passes in trace 11190), so the flare A/B needs a spot with a visible sun or lamp.
- Dynamic resolution (6d): Balazs's setting is off; traces show `resolutionScaling` (1, 1, 1, 1) and no upscale draw; the forced path for a player with dynamic resolution on is not exercised.

### 11.7 Helmet-interior motion vectors and the convergence margin

`mvsave` (both eyes' MV RT copies taken at each view's TAA draw), bike idle, head still: the world is 0 (not drawn by the MV pass), but the camera-attached helmet interior (top band and chin) moves: left eye mean 2.4 px (max 9 px), right 7.9 px (max 38 px); with `[camera] helmet_follows_view = false` 7.5 / 11.2 px (max 41 / 47 px), so the mod's helmet override is not the cause. Each view's MV-pass camera inputs are its own and consistent (current camera position = previous, previous VP close to the current, trace 11780). Reading: the game's HelmetSprings sway the helmet relative to the camera (real motion; `likely`), larger in the right eye at 10-20 cm through the asymmetric frustum (`guess`). Not a cross-eye leak. Consequence: the static convergence test is dominated by the moving lining (about 40% of the image, `convergence_diff.png`); its ratio was 0.48-0.65 over 9 runs (limit 0.60, 2 FAIL in the right eye), against 0.31 / 0.35 on 2026-10-21 before Phase 5. Open: compare with flat play (RenderDoc), and make the test ignore pixels with object motion (the MV copies exist now).
