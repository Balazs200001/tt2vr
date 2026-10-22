# Renderer: per-frame vs per-view work in the two-view frame (Phase 5)

Build 4989319 (see `README.md`). Every address below refers to that exe. Names: `symbols.md` is the source of truth.

**Status: static (Ghidra decompile/disasm, 2026-10-21) plus frame-trace evidence.** The frame traces are tt2vr traces (not in git): **4400** (2026-10-20, two views, stereo, riding, after Phase 4c), 12770 / 9191 / 13970 (two views, Phase 4a/4b), 10866 (2026-10-16, flat, one view). Trace indices are `frametrace_<frame>.txt` line ids. Confidence: `confirmed` (read directly in decompile or seen in a trace), `likely` (inferred), `guess`. Rows that need a runtime check say what to log.

Context: the mod renders eye k as engine camera index k (`ViewContext+0x464`) through the split-screen path, one `DeferredRenderLoop::Render` 0x1408469c0 call per frame (`frame-anatomy.md` 5.1, `temporal.md` mechanism C). Pass order: `frame-anatomy.md` section 4. This file answers PLAN Phase 5: which work runs once per frame, once per scene, once per view, and where the second view breaks an assumption.

---

## 1. Summary

| Work | Address / call site | Scope in the two-view frame | Correct for two eyes? | Confidence |
|---|---|---|---|---|
| `RenderLayer::BeginRender` (`RenderLayer+0x30` += 1, the `frameId`) | 0x1408d3d50, call 0x1405af5b7 in `Renderer::OnRender` | frame | yes: one increment per presented frame | confirmed |
| `ProbeRenderer::Update` | 0x14056b3a0, call 0x1405af4ba | frame (its `RenderDesc` argument is unused) | yes | confirmed |
| `RenderLoop::RenderProbe` -> `FUN_1408c7270` (probe relight state machine) | 0x1408ddeb0, call 0x1405aff42 | frame; picks the probe nearest to **any** view (min distance over the view array) | yes (eyes 6 cm apart) | confirmed (placement), likely (semantics) |
| Probe relight cadence | `FUN_1408c7270` | one cube face per call, `Probe::UpdateProbeTexture` on step 6: a 7-frame cycle | unchanged by two views | confirmed (structure) |
| Ocean FFT (spectrum 257x257, butterfly 256x768 ping-pong, displacement/normal 256x256) | trace 4400 dispatches 000494..000582 | frame | yes | confirmed (trace) |
| Probe filtering (256 cube -> 64 / 8 cubes, 6-mip chain) | trace 4400 dispatches 000417..000483 | frame | yes | confirmed (trace) |
| Texture streaming copies (BC1 260x260 tiles, CB staging) | trace 4400 000000..000056 | frame | yes | confirmed (trace) |
| PopcornFX `UpdatePhysicalSpawn` / `Evolve` / `Fence`, `UpdateEvolve`, `UpdatePkFxSceneCB` | `RenderOneFrame` 0x14048c49a / 0x14048c4b8 / 0x14048c5e6; `OnRender` 0x1405af563 | frame | yes | confirmed |
| `HHParticlesManager::SetViewContext` | 0x1409a4f80, call 0x14048c44a | frame, per camera index; **copies** the ViewContexts before `DoUpdate` rebuilds them, so PopcornFX uses last frame's views (engine behaviour, same in flat) | yes | confirmed |
| `HHParticlesManager::UpdateFence`, `SceneManager::UpdatePopCornBound`, `UpdateVisibilityInfo(type 4)` | calls 0x1408479a5 / 0x1408479b9 / 0x1408479ee | view loop, guarded `camIdx == 0` | yes (once) | confirmed |
| `StartRenderParticles` / `KickRenderParticles` / `AddParticleLights` | 0x1409a6a10 / 0x1409a3e00 / 0x140840b90 | view (own collections and sort per camera index) | yes | confirmed |
| `HHParticlesManager::CullParticles` | 0x1409a2a90, call 0x1405affb9 | frame; **union of both views** (bits `1 << (camIdx*13 + pass)` ORed over the active-view mask) | yes | confirmed |
| `LensFlareManager::Update` (fade, occlusion queries per slot) | 0x14089a770, call 0x1405ae3a7 in `Renderer::DoUpdate` | frame, per camera index, **after** the `InitFromCamera` calls (so it sees the eye-adjusted view the mod writes in its `InitFromCamera` post-hook) | yes | confirmed |
| `LensFlareManager::Render` | 0x14089a3a0, call 0x140848cbd | view | yes | confirmed |
| Culling: `AddCullingData` per view + CSM/static views, `UpdateVisibilityInfo` | 0x14091c850 (call 0x140846c5f ...) | scene (one culler, per-view frusta and bits) | yes | confirmed |
| `FillRenderInstances` jobs (`FUN_1408d3370`) | 0x1408d3370 | scene job, fanned out **per visible view** per entity, same `frameId` | yes for `Mesh`; **no for `SkinMesh`** (section 4) | confirmed |
| `DeferredRenderer::SetupLights` -> `CSM::SetupRenderContext` | call 0x140846cdd; 0x140828320 | scene; fits one cascade set **per camera index** | yes, but double cost (section 3) | confirmed |
| `CascadedShadowMapRenderer::Render` (clear atlas, static map, 3 cascades) | 0x140826230 via `DeferredLighting::RenderDirectionalCascadedShadowMap` 0x14083a7b0 | **view** | correct, renders the shared shadow state twice (section 3) | confirmed (static + trace) |
| `SimpleForwardRenderer::Render` (aux render-to-texture camera, `GameServices::SimpleForwardView`) | 0x140904070, call 0x140847957 | frame, in the view whose camera index matches its registered index | yes | confirmed |
| `ToneMapper::Render` luminance chain | `FUN_140914680`, call 0x140848768 | view 0 only since Phase 4c (view 1 composes with view 0's luminance) | yes (by design) | confirmed (trace 4400: 4 `HDR_Luminance*` draws in view 0 only, 032724..032843) |
| Global 2D HUD list | after the view loop | frame | Phase 7 | confirmed (trace 4400 068645..069418) |
| `MeshPainter::Update` | 0x1408a7920, call 0x140847cff | view, unguarded prev <- cur latch (only if `this+0x38 != 0`) | candidate double latch | likely (RC) |
| `ToneMapper` CPU readback `tm+0x148` | `FUN_140914680` | view, one shared float: the last view wins | harmless with shared exposure (both views read slot 0's luminance) | likely |

**Rule derived from the table:** everything the engine does before `RenderLoop::Render` and everything it guards with `camIdx == 0` already runs once per frame in the two-view frame. Inside the view loop only two things are wrong for two eyes of one head: the cascaded shadow render (duplicated, section 3) and the skinned-mesh previous palette (latched twice, section 4). Slot-1 state that only a second player would feed: section 6 (particle medium 2, water-drop speed, DOF focus).

---

## 2. Frame-trace evidence (trace 4400)

Method: `split_views.py` (scratch tool, not in the repo) walks the trace in submission order, keys every draw/dispatch by its shader hashes (unset stage = null), and splits at the per-view first dispatch `cs:4f3101da75386a3b` (7x119x1, view 0 at 000599, view 1 at 033250). `confirmed (trace)`.

| Segment | Draws | Dispatches | Other (clears, copies) |
|---|---|---|---|
| before view 0 | 0 | 16 (probe filter, ocean FFT) | 57 (streaming copies) |
| view 0 | 2627 | 14 | 5 |
| view 1 + global HUD | 2930 | 14 | 4 |

- Every per-view shader key appears in both views. Draw counts differ by per-eye culling only (e.g. `vs:91ffe9c5 ps:e23d863d` 136 vs 194).
- View 0 only: the four `HDR_Luminance*` draws (Phase 4c) and the back-buffer clear of the first `CopySplitRT` (`dsv1774`, 033202).
- View 1 segment only: the global 2D HUD (`vs:1be36d8009b11853`, 78 draws).
- **Shadows are rendered per view:** the cascade depth draws (`vs:db8ceef844d4db53` 632 / 633, `vs:acef520db588fa5a` 44 / 45, `vs:ecddfce1efdc6967` 49 / 52) and the atlas clear (`dsv202`, 018073 / 053431) appear once in each view.
- No second probe or ocean dispatch, no second streaming copy: per-frame passes appear once.

---

## 3. Shadows (cascaded shadow maps)

All `confirmed (static)` unless tagged.

**Fit.** `CascadedShadowMapRenderer::SetupRenderContext` 0x140828320 (from `DeferredLighting::SetupLights` 0x14083d2c0, call 0x14083d427, once per scene, whole view array) loops the views; index = `ViewContext+0x464`; skipped entirely if `CSM+0x25c2`. Per cascade slice: view matrix = `TransformMatrix::Get(vc+0x18, 0)` (the ViewContext's view, i.e. the eye pose after the mod's `InitFromCamera` fix-up), projection = `MatrixPerspectiveFovRH(Camera+0x78, near, far, vpW/vpH * Camera+0x84)` with near `vc+0x430` (clamped to `Camera+0x90` if `Camera+0x98`), split far values from `vc+0x470` (`CascadedShadowRenderDesc`, count `vc+0x4dc`), slice overlap `CSM+0x265c`. The mod's choke point has no current eye in the scene phase, so each slice uses the **union** of both eye frusta. Corners -> light space -> `BuildCropViewProjectionMatrix` 0x140823510 (power-of-two width, texel-snapped origin; no history, so identical inputs give bit-identical cascades). Cascade contexts: `CSM+(idx+0x17)*0x10` -> array stride 0x830, ViewContext at +0x130, view = light matrix, proj = crop ortho, name `Cascaded %d`.

- **No seam from head rotation:** the fit follows each eye's real orientation; the un-head-tracked scene-graph camera is not read for cascades.
- The static (distant) map is the only reader of the scene-graph camera (`SetupStaticRenderContext` 0x140828eb0: translation from `DAT_1413cda88[Camera+0x50]`): its centre ignores the head offset, harmless for a >100 m grid-snapped map.

**Render.** In the per-view loop (gbuffer path) `DeferredRenderer::RenderDirectionalCascadedShadowMap` 0x140841d60 (thunk via `[this+0x200]`) -> `DeferredLighting::RenderDirectionalCascadedShadowMap` 0x14083a7b0 -> `CascadedShadowMapRenderer::Render` 0x140826230 if the light is cascaded (`DL+idx*0x1b0+0x100 == 6`), else `SetDefaultCascadedShadowContext` 0x140827fd0; then `RenderFarShadows` 0x140826a90 if `DL+0x460` is 5 or 6. `CSM::Render`: `FUN_140903040` queues 'Clear Shadow Map' on the shared atlas RT (`*(CSM+0x25b8)+0x18`, one object for both indices), `RenderStaticShadow` 0x140827240 for (idx, 0/1), then per cascade a 'ShadowMap' depth-only pass (`FUN_1409037d0`) into the same atlas tiles for both indices (viewport `atlasW/count * i`).

**Lighting input.** End of `CSM::Render`: a stack `CascadedShadowContext` (atlas texture, 3 crop-VP matrices, split planes, per-index texel scales `CSM+idx*0x10+0x64/+0x84`, static-map matrix/size `CSM+idx*0x11a0+0x318` / `+0xa50`, far-shadow data) goes to `RenderContext::SetCascadedShadowContext` 0x1408cfb00 on the per-view `RenderLoop+0x1f00` context, which builds the `cbPSCascadedShadows` data from `RenderContext+0x5d0..+0x794`. The atlas SRV is always the same texture; its content is whatever the last queued shadow passes wrote, which is correct only because each view's lighting runs after its own shadow passes.

**Consequence.** Two cascade sets per frame, fitted from two eye positions 6 cm apart: double shadow cost (about 725 depth draws and 3 deferred lists per view in trace 4400), and the two eyes' cascades can differ by a texel snap (shadow-edge rivalry, `guess`, not observed). CLAUDE.md classifies shadow maps as view-independent state: once per frame, shared.

**Sharing recipe (static, `likely`, needs a runtime test):**

1. View 0 runs `CSM::Render` normally; the mod keeps a copy of the `CascadedShadowContext` it passes to `SetCascadedShadowContext` (rebuilt every frame; never reuse across frames: World Shift and static-map scrolling change it).
2. View 1: skip `CSM::Render` (and `RenderFarShadows`) in a `DeferredLighting::RenderDirectionalCascadedShadowMap` hook and call `SetCascadedShadowContext(rc, saved)`. The atlas still holds view 0's cascades when view 1's lighting runs (nothing else writes it in between; spot/point shadows use their own atlas, `likely`).
3. Fit for both eyes: view 0's cascades are fitted from the left eye. The right eye is 6 cm to the side; the power-of-two box usually covers it, but the snapped min edge has less than one texel of slack. Fit index 0 from the head-centre pose (mid-eye view, union projection) while `SetupRenderContext` runs, then restore the eye view.
4. CPU saving: clear `CSM+0x25b0+1` after `SetupRenderContext` so index 1's cascade and static-map culling are skipped. **Done 2026-10-22.** `SetupRenderContext` rewrites the byte every frame (`idx + 0x25b0 = light type == 6`). It is read only by the two cull loops (`DeferredRenderLoop::Render`, `ForwardRenderLoop::Render`), by `IsSetup` 0x1404fd550 (no direct callers) and, for index 0 only, by `FUN_14084c6c0`; this was checked with a code scan for the displacement. The mod restores the byte at the first `CSM::Render`. Test mode: the main thread's time before the views went from 2.34 to 1.81 ms, and both eyes' shadows are identical.

**The `cbPSCascadedShadows` matrices are world-space** (`confirmed`, RenderDoc `shadows_rider_frame3561`, Sum Lighting CS ev 34195: `cbCameraStd.viewPosition` (902.07, -1197.93, 85.34) maps through `viewProjToLightProjSpaceArray3[0]` (row vector times matrix) to (-0.44, -0.11, 0.92), inside cascade 0, and through `viewProjToLightProjSpaceStatic` to (0.002, 0.000), the static map's centre; a camera-relative matrix would map the origin there instead, which lands at (-14.3, 33.3, -37.3)). The lighting adds each view's own camera position, so one context serves both eyes exactly.

**Implemented (PLAN 5c, `game::SharedShadows`):** steps 1-3 (step 4 not done). Runtime, test mode: one atlas clear per frame (trace 12870, `dsv` flags=1 once; per-view trace 12270: twice), view 1 1564 draws instead of 2534, GPU view 1 9.60 -> 6.64 ms, frame 19.76 -> 17.06 ms.

**Static map cadence.** A scrolling toroidal cache per index (`BuildStaticProjectionMatrix` 0x1408239d0 every frame): nothing if the centre moved less than a texel, an edge strip (modes 3..6) for small moves, a progressive tiled refill (mode 2) after a jump larger than the map. Not a per-N-frames re-render. `confirmed (static)`.

---

## 4. Per-object previous transforms: `Mesh` is right, `SkinMesh` latches twice

**frameId.** The fill job `FUN_1408d3370` reads `*(int*)(RenderLayer::s_pInstance + 0x30)` (render id) at run time and calls the entity's `FillRenderInstances` (vtbl +0x70) for **each** culler view whose bit is set in the entity's visibility mask, sequentially on one thread, all with the same `frameId`. `RenderLayer+0x30` changes only in `BeginRender` (once per `OnRender`; `RenderFlipEmptyFrame` adds one on resize frames). `confirmed`.

**`Mesh::UpdateRenderData` 0x1408a3f00.** The first view of the frame advances prev <- cur (`Mesh+0xf4..` <- `+0xc4..`), later views see `frameId == Mesh+0x70` and only rebuild the per-call record from the same prev/cur. Both views get identical object motion. `confirmed`.

**`SkinMesh::UpdateRenderData` 0x140906b90: bug in two-view frames.** `confirmed (static)`, `confirmed (trace)`.

- The mesh owns **one** `EntityRenderData` (`SkinMesh+0x68`) and one pair of bone palettes (cur / prev, per mesh, not per camera index). Each call clears its flags `+0x30` (motion valid) / `+0x31` (reset) and, for the main pass (`ViewContext+0x460 == 0`), sets `+0x30 = renderFlags[idx] bit 3` if `frameId == last[idx]+1` (`SkinMesh+0x158+idx*4`) and the LOD matches `lastLod[idx]` (`+0x160+idx*2`), else `+0x31` on a frame/LOD change; then `last[idx] = frameId`.
- Reuse (no refill) only if `Mesh+0x70 == frameId` **and** `+0xc0 == camIdx` and the LOD matches. View 1 has another camera index, so it refills: the fill (`FUN_1409062b0`, SkinEntityRenderData vtbl 0x141004678 slot 1) does prev <- cur again (slot 1's own `last[1]` says "valid"), then cur <- pose N.
- The draw records copy the palettes into the constant buffers at record time (`FUN_140905f70`), i.e. in the per-view passes, after both fills. **Both views draw with prev == cur.** Skinned objects (bike and rider parts) get zero object motion: their motion vectors show only camera motion, so TAA reprojects them as if they were static in the world.
- Trace evidence: the skinned motion-vector shader `vs:51bfa2c7f479d821` binds the cur / prev palettes as two 2880-byte CBs (VS slots 8 and 9). First 16 floats of each pair: flat trace 10866: 5/5 **differ**; two-view traces 12770, 9191, 13970, 4400: 41 of 44 pairs **identical** in both views (13970 was taken while riding).
- This plausibly explains the "smear on bike and rider" of the Phase 4a runtime test (`PLAN.md` decisions, 2026-10-20: "MV data identical in both views"): the data was identical in both views because both were wrong. `likely`.
- Stock split screen has the same flaw (two players rarely look at the same skinned mesh).

**Fix candidate (no code patch, data write before the original).** Hook `SkinMesh::UpdateRenderData` (export `?UpdateRenderData@SkinMesh@Graphics@PlayAll@@UEAA_NAEBVViewContext@23@IH@Z`). Pre: if the main pass (`vc+0x460 == 0`), `idx = vc+0x464 < 2`, and `last[1-idx] == frameId` (the other eye's main pass already latched this frame), write `last[idx] = frameId` and `lastLod[idx] = lastLod[1-idx]`. The original then sets neither `+0x30` nor `+0x31` (no second latch) and refills cur from the same pose. A mesh visible in one eye only still latches once. If the two eyes pick different LODs, `+0x31` (reset) fires and that mesh shows no motion for one frame. Thread: render-phase enki fill job, one job per entity for all its views (no race). Runtime check: trace pairs `DIFF` in both views while riding. **Runtime (2026-10-21, test mode, trace 12270, two views, bike idling on the start line): with `game::SkinMotion` all 10 pairs differ (5 per view), and view 0's and view 1's pairs are identical (one latch per frame, shared).**

---

## 5. Smaller items

- **PopcornFX culling** is the union of both views (corrects `temporal.md` P9, which said view 0 only).
- **`SimpleForwardView`** (`SimpleForwardView::Create` 0x1405df0f0, RT 'SimpleForwardView', own `MatrixLookAtRH` view from an actor node): rendered once per frame into its own RT; both eyes sample the same texture. Purpose (mirror, in-world screen) `guess`.
- **`s_vLastViewPos0`** `DAT_1413cdb14..1c`: `RenderOneFrame` copies last frame's `ViewContext[0]` position there (0x14048c4ee) before the scene update; readers unknown. With the mod it holds the left eye position (6 cm off the head centre). RC: find readers.
- **`MeshPainter::Update`** (call 0x140847cff): per view, latches its prev <- cur state without a frame guard. RC: is `this+0x38` ever non-null while riding?

---

## 6. Slot-1 state audit

Question: which per-camera-index state does view 1 read, and is slot 1 written in the mod's renderer-only two-view mode (no `GameEngine::SetSplitMode`, no second player)? Method: the per-view loop of `DeferredRenderLoop::Render` and every callee taking `ViewContext+0x464`, then the writers of slot 1. `confirmed (static)` unless tagged.

**Fed every frame without a second player (already fine):**

- **Environment.** `DayTime::Manage` 0x14045e1e0 -> `EnvironmentSettingsMixer::LerpAndApply` 0x14047a2d0 -> `ApplyParams` 0x140479000 loops `idx = 0..1` unconditionally (gated only by the mixer enable and the DayTime actor flag, not by split): Bloom (`RenderLoop+0x1ec8`, stride 0x1c), HDR settings and colour grading (`tm+0x30+idx*0x60`, `likely`), wind (`Wind::s_oStrengths` 0x14140bf98, 2 slots), vignette (`SplitViewMaterial[idx]`), weather (wet / rain `+idx*0x1d8+0x1bc/+0x1c0`, `WaterDrop::SetRainIntensity(idx)`), fog (`Scene+0x138+idx*0x98`), ambient cube (`Scene+0x20+idx*4`), sky animation. The per-index blocks live in the shared `Game::Scene`; `DoUpdate` copies `Scene[idx]` into `ViewContext[idx]`, so the mod's `Renderer+0xd88 = +0xd80` makes view 1 read slot 1 of the level scene, which the mixer fills.
- **Per-slot CVars** (`ICVar<bool>`: value `+0x80+idx`, split value `+0x82`, main `+0x83`, cap `+0x84+idx`; setter `FUN_140107b30`): motion blur `DAT_141407830[idx]`, SSAO `DAT_14140c7b0[idx]`, SSR `DAT_1413cc2f0[idx]`. Both slots take the main value; only `CVarManager::s_bSplitEnabled` (never set by the mod) clamps them to the split value.
- **Post-process lists** `PostProcessManager+0x50+idx*0x10` rebuilt per view (`UpdateActivePostProcess` 0x1408bfb90); `Enable/DisablePostProcess` with idx -1 write both slots; `RenderLoop::UpdateDof` 0x1408df210 handles both.
- Weather scroll (`FUN_140921d20`, once per `Render`, both slots), froxel noise time (`DL+0x920+idx*4`, self-accumulating), water-drop enable (both slots), lens-flare state (`LensFlareManager::Update` for every mask bit), fog geometry `RenderLoop+0xc98+idx*8`, effector map slot 1 (created by `DoUpdate`, centre copied by the mod), `DeferredLighting` per-slot bytes (initialised identically; the owner-player ones are mirrored by the mod).

**Gaps (slot 1 not fed in the mod's mode):**

| # | State | Writer of slot 1 | Effect in view 1 | Fix class | Confidence |
|---|---|---|---|---|---|
| A | PopcornFX medium 2 (per-camera effect pairs: `HHParticlesComponent::PostObjectInit` 0x1405091f0 makes instances in mediums 1 and 2 when `component+0x4a` and `GameContext+0x23 == 0`) | `HHParticlesManager::EnableMedium` 0x1409a3100, only caller `RenderOneFrame`: `EnableMedium(Renderer+0xdd8, 2, GameEngine+0x184 != 0)` every frame; the mod never sets `+0x184` | view 1 renders collection 2, whose medium is disabled: per-camera effects may show in the left eye only | **fixed** (PLAN 5d, `game::ParticleMedium`: `EnableMedium` hook keeps medium 2 on while two views run) | confirmed (writer, skip of spawn/evolve, unconditional allocation); visible effect not observed yet |
| B | Water-drop camera speed / acceleration (`WaterDrop` = `RenderLoop+0x1ec0`, slot stride 0x68 from `+0x60`: `+0xa4` speed, `+0xb0` accel) | `FUN_1403be8e0`, loop `idx < 2` over `g_pGameEngine vtbl+0x570(idx)` (player actor); null for slot 1 | lens droplets in view 1 get no motion streaks (rain only) | mirror slot 0 -> 1 per frame | likely |
| C | DOF focus per slot (`KTUDof+0x368/+0x370`) | `KTUDof::OnManage` 0x1409e6680: slot 1 only if `Renderer+0xe40 != 0` and `KTUDof+0x364 != -1` (a DOF target pushed with mask bit 2) | default focus in view 1 when a script/cutscene uses DOF with mask 1 (`SetCameraDof` uses mask 3: fine) | runtime check (cutscene, replay) | confirmed (mechanism), guess (impact) |
| D | Crash grey: the "FadeToColor" post process of `TTCollisionEffect` (`TTGameEngine+0x778`) | its Manage `FUN_14017c4f0` enables / disables the post process and writes the material for slot `+0x90` only (ctor sets 0, no other writer) | after a crash the left eye turns grey, the right stays in colour (`temporal.md` 11.2) | mirror slot 0 -> 1 per frame (below) | confirmed (slot 0 only), likely (that this is the grey seen) |

Gap A details (static, 2026-10-21): the active flags `HHParticlesManager+0x28..+0x2b` are copied from the requested flags `+0x2c..+0x2f` (written by `EnableMedium`, default 1) in `BeforeParticleUpdate` 0x1409a1890; `UpdateSpawn` 0x1409a7810 and `UpdateEvolve` 0x1409a7350 skip a disabled collection (`UpdatePhysical*` gate on collection 3 only); rendering (`StartRenderParticles`, `KickRenderParticles`) reads no flag. `HHParticlesComponent+0x4a` is the serialized property "Update Per Split" (`RegisterMembers` 0x1404c9e00); with it and `GameContext+0x23 == 0` (`EngineInitDesc` byte 0, which also creates an `EditorSaveCallback`: an editor/tool flag, `likely`) `PostObjectInit` loads the effect twice, `Load(instance, path, medium 1 / 2)` (`instance+0x6c`), both linked to the same actor node; medium k is evaluated with camera index k-1's view (`SetViewContext`). All four collections are allocated unconditionally in `HHParticlesManager::Create` 0x1409a20d0. B is deferred to Phase 8 (visor drops differ per eye anyway), C to Phase 7.

Gaps B and D details (static, 2026-10-21, Phase 8; `confirmed (static)` unless tagged):

- **WaterDrop** (`RenderLoop+0x1ec0`): shared part `+0x00..+0x58` (four RTs: drop mask, refraction, blur ping / pong; constant buffer, VB, effects), redrawn by every `Render` call, so no cross-view hazard. Slot `S = +0x60 + idx*0x68`: three growable 0x48-byte particle arrays (rain drops `S+0x00`, lines `S+0x10`, splashes `S+0x20`; counts at `+0x04/+0x14/+0x24`), max drops `S+0x30/+0x34`, rain `S+0x40`, camera speed `S+0x44..+0x4c`, acceleration `S+0x50..+0x58`, spawn timer `S+0x5c`, alive `S+0x60`, enable `S+0x61`.
- The drop simulation runs **inside** `WaterDrop::Render(dt, lastFrameColour, idx)` 0x14092a9c0, called unconditionally per view (call 0x1408486f6, `dt = RenderLoop+0x1f30`); spawning uses the global `gen_rand32`, so the two slots always hold different drops. `Render` ignores the enable byte; `HasAliveParticles(idx)` 0x14092a490 (alive and enable) gates the last-frame colour copy and the drop composite (`FUN_140914e20`).
- Feeding: weather manager `FUN_1403be8e0` (per game update, idx < 2): rain intensity and enable for both slots; speed and acceleration from player idx's actor, which is null for slot 1 (stays 0). The game's own "off" is `WaterDrop::Clear(idx)` 0x140928d70 + enable false.
- Options, all from the `DeferredRenderLoop::Render` pre-hook (after the game update, before view 0): **per eye** = copy slot 0's speed and acceleration to slot 1 (`+0xa4 -> +0x10c`, `+0xb0 -> +0x118`, 12 bytes each); **shared** = hook `WaterDrop::Render`, for idx 1 call it with idx 0 and dt 0, then copy the alive byte (`+0xc0 -> +0x128`); **off** = both slots' counts, rain, speed, acceleration and enable to 0 (or `Clear`). All `likely` (not run).
- **FadeToColor** (gap D): `TTCollisionEffect` Create `FUN_14017b880` loads `Data:/Engine/PostProcess/FadeToColor.PMA` (`SplitViewMaterial*` at `+0xe8`, post-process index `+0x94`, `PostProcessManager*` `+0x88`); its collision delegate `FUN_14017c900` sets the intensity `+0x98` and time `+0x9c`; Manage writes `fBlend`, `fGrey = 1`, `vLevels`, `vColor` (white) into slot `+0x90`'s material. Per-slot material `M_s = *(*(SplitViewMaterial + s*8) + 0x18) + 0x30`; accessors `MaterialDefinition::GetFloat` 0x14089e570, `GetVector` 0x14089e690, `SetFloat` 0x14089e720, `SetVector` 0x14089e820. Post-process object: active byte per slot `+0x80+s`, request `+0x84+s`; `EnablePostProcess` 0x1408bde30 / `DisablePostProcess` 0x1408bdca0 (`ppm, idx, slot`, slot < 0 = both), `IsPostProcessEnabled` 0x140451470, `GetPostProcessIndex` 0x1408be260, `GetPostProcessMaterial` 0x1408be2a0; the render loop's manager is `DeferredRenderLoop+0x3d0`.
- Same post effect by mask: `KTUFadeToColor::ActivateGrey` 0x1409e74e0 (Lua) -> `KTUPostProcessManager::Activate` 0x140a1dfe0, mask bit 0 = slot 0, bit 1 = slot 1. Code callers use mask 3 (both) except `FUN_140399080` (mask = player + 1, no grey).
- Fix (likely): in the `DeferredRenderLoop::Render` pre-hook, while two views run, make FadeToColor's slot 1 enable state follow slot 0 and copy `fBlend`, `fGrey`, `vLevels`, `vColor` from `M_0` to `M_1` every frame it is on. This also covers a Lua grey with mask 1. Never re-run the effect's Manage for slot 1 (it would count its timer down twice).

**Re-entrancy (state written in view 0, read in view 1):** the single TAA reset byte `RenderLoop+0xd08` (cleared at the end of view 0's `RenderTAA`; the mod already writes it before each view, Phase 4d); the `camIdx == 0` work (keyed on camera index 0, not on "first view": breaks only if index 1 were rendered alone); `SimpleForwardRenderer::Render` once per frame; projection-type state `DeferredRenderLoop+0x2740` (same camera for both views); shared transients re-prepared per view (`RenderContext` at `+0x1f00`, MV RT, linear depth, tonemap `tm+0x130` swap). None is a hazard for two eyes except the ones above.

---

## 7. GPU timing

**Engine whole-frame timer** (`confirmed`): ring of 4 slots (`Renderer+0xec8 & 3`), 3 queries per slot at `Renderer + (slot*3 + 0x1cd)*8` (start TIMESTAMP, end TIMESTAMP, TIMESTAMP_DISJOINT). `FUN_140865330` (call 0x1405af5fc in `OnRender`): Begin(disjoint), End(ts0). `FUN_140865160` (call 0x1405af3c7 in `OnFlushRender`, after queue execution): End(ts1), End(disjoint). `FUN_1408651f0` (call 0x1405af33d in `OnFlip`): spins on the previous slot, writes the GPU frame time in seconds to `Renderer+0xf28`; `OnFlip` then increments `+0xec8`. The bracket covers both views' execution.

**Profiling events** (`confirmed`): exported `RenderLayer::BeginProfilingEvent` 0x1408d3d40 / `EndProfilingEvent` 0x1408d6a90 (both = `AddProfilingEvent` 0x1408d3aa0 with name / null) take a `SimpleGPUPerformanceQuery*` (`+0` `ID3D11Query*`, `+0xc` int queue index) and queue a kind-5 pass (payload `{q, name}` at `DAT_1414082e0[DAT_1414082f8]`, count reset in `RenderPass::FrameFlip`). At execution `ExecuteRenderPassRange` 0x1408d6ad0 calls `RenderLayerDX11` vf+0xa0 `FUN_140948010` on the context of the job that runs the range (immediate or a worker's deferred context): `End(query)`, then `ID3DUserDefinedAnnotation::BeginEvent(name)` or `EndEvent()`. The engine never calls them (no `SimpleGPUPerformanceQuery` is ever created). Kind-5 passes carry no cost, so they never move the job split.

**Use for per-eye timing:** queue a timestamp event from the `RenderContext::Prepare(view)` hook (the `DeferredRenderLoop+0x1f00` context only) at the start of each view and one at the end of `DeferredRenderLoop::Render`; Begin/End pairs keep the annotation markers balanced (and name the views in RenderDoc). Deltas give view 0 and view 1 GPU time; the engine's own timer slot of the frame gives the frequency, the disjoint flag and the frame total. **Runtime (2026-10-21, `game::GpuTimers`, test mode, two views, start line):** view 0 9.90 ms, view 1 9.60 ms, frame 19.76 ms; the stamps are monotonic and the two views plus the per-frame passes add up to the engine's bracket, so the events land at the view boundaries.

---

## 8. Runtime checks

1. Skinned palettes after the fix: trace pairs of `vs:51bfa2c7f479d821` differ in both views while riding; TAA on the bike/rider without smear.
2. Shared shadows: one atlas clear and one cascade set per frame in the trace; both eyes' shadow edges identical; no missing shadow at the right eye's outer edge.
3. GPU timestamps: view 0 < view 1 start < end, monotonic with `RenderJobCount` 15 (events on deferred contexts).
4. `MeshPainter` activity, `s_vLastViewPos0` readers (section 5).
5. Particle medium 2 (section 6 A): log `Renderer+0xdd8` bytes `+0x28..+0x2f` and `GameContext+0x23`; A/B `EnableMedium(2, true)` and compare bike-attached dust/smoke in both eyes.
6. Water-drop slot 1 in rain (6 B): `WaterDrop+0xa4..+0xbc` for both slots. DOF slot 1 in a cutscene/replay (6 C): `KTUDof+0x364`.
7. Crash grey (6 D): FadeToColor enabled in slot 0 after a crash (`IsPostProcessEnabled(ppm, idx, 0)`), slot 1 off; after the mirror both eyes equally grey.

Runtime (Phase 8, test mode, 2026-10-21; `game::SlotMirror`):

- Item 7, `confirmed (runtime)`: after a crash FadeToColor turns on in slot 0 and the mirror turns slot 1 on with it (log `crash grey (FadeToColor) on in the right eye too`), and off when it fades; mean saturation of the two halves 0.162 / 0.172 (before 0.155 / 0.200). The `TTGameEngine+0x778` pointer, the post-process index at `+0x94` and the per-slot material path (`*(*(SplitViewMaterial + s*8) + 0x18) + 0x30`; the `+0x30` is the ICF-folded getter 0x1401acf70 `lea rax, [rcx+0x30]`) work as read statically.
- Item 6: forced rain on slot 0 (`drops`) fills slot 0's drop array (360) and sets its alive byte; the hook copies it to slot 1. No drops appear in either eye: the drop composite is gated by something the forced rain does not set (`guess`: the weather's wet state). Time Attack offers no rain.

## 9. Pass queue for a per-pass GPU profile (Phase 9, static, 2026-10-21)

`confirmed (static)` unless tagged.

- The append helper `FUN_1408e0a40(queue, kind, poolIndex)` has exactly six callers, all exported `RenderLayer` methods: `DrawCommandBuffer` 0x1408d6a10 (`DrawPass*`, kind 0), `Dispatch` 0x1408d68d0 (`DispatchCommand*`, kind 1), `InsertFence` 0x1408d7c40 / `WaitFence` 0x1408d87b0 (kinds 2 / 3, no GPU work), `CopyTextureSlice` 0x1408d5db0 (`CopySubRessourcePass*`, kind 4), `AddProfilingEvent` 0x1408d3aa0 (kind 5). Draw, compute and copy go to queue 0; each has the immediate branch (`RenderLayer+0x268`, set only while the queues execute). Queue entry: ushort pool index +0, kind +4; header {ptr, ushort capacity +8 (queue 0: 0x46c0), ushort count +0xa}; no capacity check.
- Names: `DrawPass` (pool 0x1414082c0, stride 0x990, 256 per drain) embeds its `DrawPassDesc` at +0x18, name `const char*` first (may be null); `DispatchCommand` (pool 0x1414082c8, stride 0x3d0, 256) name at +0x3b8 ('Light Froxel CS', ...); copies (0x2000) and fences are unnamed.
- Order: `ExecuteRenderPassRange` 0x1408d6ad0 runs entries in append order; `ExecuteRenderPasses` 0x14094b320 runs queues 6..1 inline (empty: async compute is off, `likely`), then cuts queue 0 into contiguous job ranges by the summed cost of kind-0 entries (`pass+0x96c`), job 0 on the main thread's immediate context, the others on deferred contexts, then `ExecuteCommandList` in job order. Profiling events add no cost, so they never move a cut; timestamps are therefore monotonic in GPU order (`likely`, to check at runtime).
- The queues drain at least twice per frame (`OnFlushRender`, `RenderLayer::EndRender` in `OnFlip`), each ending in `RenderPass::FrameFlip`; the profiling payload array `DAT_1414082e0` holds 0x4480 entries per drain, unchecked. Everything (probes, shadows, HUD, views) goes through the same queue; entries carry no view id, so the recording-time view (the mod's `RenderContext::Prepare` marker) attributes a pass.
- Runtime (test mode, start line, 1770x1892 per eye, RTX 3060, 2026-10-21): `confirmed`: names come through for draw and compute passes, timestamps are monotonic (no pass dropped), and the per-view sums match the per-view brackets (view 0 8.60 / 6.67 ms). Passes outside the views: probe filtering, ocean FFT, copies (0.32 ms). The HUD capture's 2D HUD draws land in 'view 1' (recorded after the view loop while the view-1 marker is still open).
- 'FXAA' (0.145 ms per eye): `RenderLoop::RenderFXAA` 0x1408dd260 (export) runs when `RenderLoop+1000` (the `PostProcessFXAA`) exists and `s_eAntiAliasingMode` (0x141407dc8) is 1 or 2 (`IsUsingFXAA` 0x1404fd610 is the same test); the menu's "TAA" saves mode 2 (FXAA_TAA), so the flat game runs FXAA before TAA too. `confirmed (static)`
  - **FXAA is a step of the post chain's countdown, not an optional extra** (2026-10-22, `confirmed` static + runtime): `PostProcessManager::ResetFramePostProcessCount` 0x1408bfa60 (ppm, finalRT, slot) sets the final target (`ppm+200`) and the number of post steps this frame (`ppm+0xd0` = `ppm+0x54+slot*0x10`); `GetCurrentRenderTarget` 0x1408bdf60 returns ping-pong `ppm+0xb8+(count&1)*8` while count > 0, else the final target. Each step (FXAA included: count--, source = the old current, destination = the new current) moves the image one step on, so the last step writes the final target (the back buffer for a single view). Skipping `RenderFXAA` therefore ends the chain one step early in a ping-pong buffer: in single-view renders (main menu showroom, `ForwardRenderLoop::Render` 0x140862f70 has the same FXAA -> TAA order) the scene never reached the back buffer and the menu UI drew over stale frames (trails, the desktop overlay of the previous frame). Two-view frames are unaffected because `CopySplitRT` copies each view's current target into its half. The mod's skip (`[xr] fxaa = false`) is now limited to two-view frames with TAA running after it (`IsUsingTAA` 0x1404fd660 export and `RenderLoop+0x20` float dt != 0, the same gate as `RenderTAA`).
- 'RenderCopyTexture' (5 per view, about 0.09 ms each): `PostProcessManager::RenderCopyTexture` 0x1408bf670, called from `RenderGBuffer`, `DeferredRenderLoop::CopyHDRRenderTarget`, `RenderLoop::CopyBuffersForNextFrame` (last-frame colour), `RenderTAA`, the post chain and `RenderLoop::CopySplitRT` 0x1408da340 (each view's result into its half of the back buffer: there because of the split path). `confirmed (static)`
- Profiler design (`game::PassProfiler`): pre-hook the three GPU appenders; while a profile runs, queue a named event before and an unnamed one after each pass (own query each), at most 768 passes per frame; frames = Present to Present, bracketed by a disjoint query on the immediate context in the Present post-hook (inside the engine's immediate-context lock).

## 10. Occlusion culling (static 2026-10-22, delegated RE; runtime switch in place)

`confirmed (static)` unless tagged.

- **No depth buffer, no GPU occlusion.** Occluders are `Occluder` objects (oriented boxes, `BoundedVolume`, 0x2e0 bytes) in the scene graph. GPU occlusion queries exist only for lens flares (`LensFlareManager`). `Engine.Renderer.Occlusion` is a Lua table, not a CVar: `VisibilityCuller::Create` 0x14091cae0 reads one int, `MaxViewOccluders`, into `VisibilityCuller+0xfc8`.
- **Source:** the world streamer builds up to 80 terrain occlusion boxes per tile. `UpdateLoadTile` 0x140691320 calls `CleanTerrainOcclusionBoxOccluders` 0x140683400, then `BuildTerrainOcclusionBoxOccluders` 0x140682930 (main thread), placing them with `Terrain::ToWorldSpace`. The box data is rebuilt on a worker (`RebuildTerrainOcclusionBoxes` 0x14068c6a0, "Rebuilding Occlusion!" in `FUN_14067eed0`). `OccluderComponent` entities add their own boxes to the same pool.
- **Per frame and scene,** in both `DeferredRenderLoop::Render` and `ForwardRenderLoop::Render`:
  - `ClearOccluder` 0x14091cab0 empties the per-view lists.
  - The cull loop runs, then `UpdateVisibilityInfo(type 3)`.
  - `VisibilityCuller::PrepareOccluder` 0x14091d490 (export, main thread) keeps, per view, the occluders that survive `Occluder::PreparePlanes(ViewContext)` 0x1408afab0. That function builds a shadow volume from the box silhouette seen from that view's `camPos`, so each eye has its own. They go into `+0xdc8` (all) and `+0xbc8` (those with `OccludeGISTerrain`, `Occluder+0x2d8`).
  - The cull jobs (`StartUpdateVisibilityInfo` pool types 0, 1, 5 with the flag true) and `Terrain::Cull` 0x14066b320 (`s_bUseOccluders` 0x141357c3c) cull a volume entirely inside those planes (`Occluder::IsCulled` 0x1408af710). `GenericInstanceSpawner` does the same, gated by `s_bUseOccludersLocal` / `s_bUseOccludersGlobal`.
- **Switch:** skipping `PrepareOccluder` leaves every list empty: no occlusion culling, frustum culling unchanged. This is the mod's `[xr] occlusion = false` (remote `vreffect occlusion off`). Runtime, test mode at the start line: no measurable cost (GPU 12.03 vs 12.02 ms, same CPU).
- **Suspected misfires** behind the "distant landscape seen through nearby scenery for a frame" reports (Balazs, 2026-10-22: both eyes, everywhere): (1) boxes offset for a frame around a World Shift or a tile's Clean / Build, `guess`; (2) a camera inside or right beside a box gives a degenerate silhouette, `guess`. To settle with the switch in the headset.

## 11. Vegetation and terrain per camera: the one-frame "see-through" glitch (2026-10-22)

`confirmed (runtime)` for the mechanism and the fix (test mode with `[debug] flash_detect` and `game::CullProbe`); layouts `confirmed (static)` by delegated RE.

- **Symptom:** for one frame, one eye (almost always the right eye) lost every nearby tree, bush, hedge and wall, so distant houses and the sky showed through. Far geometry, the road and street furniture stayed. Balazs saw this "every so often, anywhere on the track". In test mode it happened about every 3 s while riding.
- **Per-camera terrain node lists:** `HeightmapComponent::PostManage` 0x140664f30 (game update) builds a `CameraPosAndIndex` array (stride 0x10: float3 position, ushort camera index at +0xc). It holds each Renderer camera k < 2 that is non-null at `Renderer+0xe48+k*8` and has bit k of `Renderer+0xe58` set (position taken from its scene node), plus an explicit third position (index 2). It passes the array to `Terrain::Manage` 0x140671570 (export). `Manage` empties both cameras' active node lists (`Terrain+0x2ec + idx*0x10` count, `+0x2f0` data) and refills only the cameras it was given. `Terrain::Cull` 0x14066b320 and `GenericInstanceSpawner::StartCulling` 0x14062f980 read those lists in the frame's render. The spawner writes per-camera billboard lists (`spawner+0xdc + idx*0x10` count) and per-view-slot visible groups (`+0x11c + (idx*13 + viewType)*0x10`), on the main thread or summed by jobs that start after the cull job. Its enki chain is race-free (`AddTaskSetToPipe` adds the dependent before releasing the WaitGroup).
- **Runtime finding:** in each glitch frame `Terrain::Manage` received 2 cameras (index 0 and 2) instead of 3 (0, 1, 2). Camera 1's node list was therefore 0 (camera 0: 156 / 163), its billboards 1 (camera 0: 6 / 4) and its groups short. The neighbouring frames were normal. The mod's two-view CameraDesc is rewritten every `DoUpdate`, and no desc change was logged, so something briefly dropped camera 1 between the mod's writes (writer not identified).
- **Fix (`game::SplitView`, always on with `[xr] stereo`):** the mod hooks `Terrain::Manage`. While two views run, if the array has camera 0 but not camera 1, it is called with camera 1 added after camera 0, at camera 0's position. Both eyes share the game camera, so this is the array a normal frame passes. Test mode, 8 min of AI riding: no "terrain nodes N / 0" frame (before: one about every 3 s), and the fix fired as logged.
- **Ruled out on the way:** occlusion culling (`[xr] occlusion`, section 10), the culling frustum (identical to the neighbours, render and cull views equal), spawner job races (static), and the runtime's frame synthesis (the glitch is in the rendered image).
- **Open:** in test mode with the game's AI riding (`ai on`), a different one-frame artifact appeared twice in both eyes: the camera looks out through the rider's own helmet shell. It was not seen in player-ridden runs, so it may come from the AI switch.

## 12. Eye depth for depth submission (2026-10-22, delegated RE; runtime-checked in test mode)

- **Source:** each eye's 'Last Frame Depth' (`RenderLoop+0x10e0 + idx*8`, R32_FLOAT, eye render size), a bit-exact copy of the main depth (`RenderLoop::CopyBuffersForNextFrame` 0x1408d9ee0 -> `RenderCopyTexture32Bits` 0x1408bf4a0). The copy runs after the forward, front and transparency passes and before TAA, so it holds this frame's depth at Present. `confirmed` The copy only runs while SSR is on, water drops are alive, or a view flag is set. With SSR on in VR it is fresh every frame (test mode: identical in neighbouring frames only when standing still).
- **Mapping:** the projection is reversed-Z with a finite far plane (`MatrixPerspectiveFovRH` 0x140813390: m22 = n/(f-n), m23 = n f/(f-n)), so d = n (f - z) / (z (f - n)) and z = n f / (n + d (f - n)). `confirmed`
- **ZFrontPass split:** while `RenderLoop+0x111c` (enable) and the camera's front pass (`Camera+0x98`) are both on, `SetupZBackViewContext` 0x1408def50 puts the GBuffer world passes in viewport depth [0, 1-s] and `SetupZFrontViewContext` 0x1408df070 puts the front pass (bike and rider, `Camera+0x90/+0x94` near/far) in [1-s, 1]. Here s = `RenderLoop+0x1118` (Lua `Engine.Renderer.ZFrontPass.ZRange`, default 0.05). The world forward-opaque batch is probably not wrapped, so it is stored unscaled (a 5% distance error at most when decoded as split). `likely` The helmet camera runs with the front pass off (near 0.01, far 25000; front 1 / 1).
- **The mod (`d3d::Blitter::EyeDepth`):** decodes per pixel to real metres (divided by `[xr] world_scale`), re-encodes as reversed-Z with near 0.05 m and far = the camera far / world scale, and writes D32 depth swapchain images. The layer gets minDepth 0, maxDepth 1, nearZ = far and farZ = near (nearZ > farZ marks reversed depth). Test mode, start line: road 3.6 m ahead, handlebars 0.34 m, tank 0.24 m, trees 11-58 m, sky = far. The depth image lines up with the colour image.

## 13. Lens visibility mask (2026-10-22; runtime-checked in test mode)

- **Depth clear:** each view's depth (D32_FLOAT_S8X24) is cleared by a draw-less pass at the start of `DeferredRenderer::RenderGBuffer` 0x140842040. It is recorded by `RenderLayer::ClearRT` 0x1408d49d0 (export; flags 0xd = discard colour + clear stencil + clear depth, name "discard framebuffer & clear stencil & depth", RenderTarget at pass+0x70, writable DSV at RenderTarget+0x118). Depth clears to 0 (far / sky), and geometry tests GREATER_EQUAL. 'Clear GBuffer' is a full-screen pass at depth 0 recorded after the GBuffer geometry; it fills the GBuffer only where depth == 0. `confirmed` A third 0xd clear per frame targets a full-width (two-view) 3320-wide target. The mod ignores it: a clear counts only when that view's 'Clear GBuffer' follows it.
- **Execution:** kind-0 queue entries run through RenderLayerDX11 vtable +0x80 (`FUN_14094ad70`) into `FUN_14094aab0`(layer, ContextWrapper*, DrawPass*), signature-scanned. The wrapper's +0 is the job's ID3D11DeviceContext (job 0 immediate, the others deferred). `confirmed`
- **The mod (`game::LensMask`):** passes are mapped to their eye when recorded (the pass profiler's `DrawCommandBuffer` hook, open view). At execution, on the same context with all touched state put back, it draws the runtime's hidden-area mesh (`XR_KHR_visibility_mask`, projected with the eye's rendered tangents):
  - at depth 1.0 right after the GBuffer clear: all geometry, including the front pass, fails there;
  - at 0.0 before 'Clear GBuffer': the pixels become sky, the GBuffer is filled and Sum Lighting takes its depth-0 path;
  - at 1.0 / 0.0 around 'Ocean': otherwise the sea plane draws into the sky pixels and SSR traces its water (+0.32 ms).

  Test mode, synthetic mask (outside an ellipse inscribed in the eye frustum, about 30% of the pixels), start line: GPU 13.47 -> 12.53 ms (Main Lighting -0.55, ZPrepass / Opaque / Alpha test -0.32, copies -0.11). Masked pixels show sky. The real mesh is saved to `headset_profile.txt` by the first real session (test mode then uses it).
