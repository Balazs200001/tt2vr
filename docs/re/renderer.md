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
4. Optional CPU saving: clear `CSM+0x25b0+1` after `SetupRenderContext` so index 1's cascade culling is skipped (`guess` that nothing else reads it).

Open: whether the `cbPSCascadedShadows` matrices are world-space (shareable as-is) or camera-relative (index 1 would need a rebase): compare both views' cascade CBs in a trace with sharing on.

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

**Fix candidate (no code patch, data write before the original).** Hook `SkinMesh::UpdateRenderData` (export `?UpdateRenderData@SkinMesh@Graphics@PlayAll@@UEAA_NAEBVViewContext@23@IH@Z`). Pre: if the main pass (`vc+0x460 == 0`), `idx = vc+0x464 < 2`, and `last[1-idx] == frameId` (the other eye's main pass already latched this frame), write `last[idx] = frameId` and `lastLod[idx] = lastLod[1-idx]`. The original then sets neither `+0x30` nor `+0x31` (no second latch) and refills cur from the same pose. A mesh visible in one eye only still latches once. If the two eyes pick different LODs, `+0x31` (reset) fires and that mesh shows no motion for one frame. Thread: render-phase enki fill job, one job per entity for all its views (no race). Runtime check: trace pairs `DIFF` in both views while riding.

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
| A | PopcornFX medium 2 (per-camera effect pairs: `HHParticlesComponent::PostObjectInit` 0x1405091f0 makes instances in mediums 1 and 2 when `component+0x4a` and `GameContext+0x23 == 0`) | `HHParticlesManager::EnableMedium` 0x1409a3100, only caller `RenderOneFrame`: `EnableMedium(Renderer+0xdd8, 2, GameEngine+0x184 != 0)` every frame; the mod never sets `+0x184` | view 1 renders collection 2, whose medium is disabled: per-camera effects may show in the left eye only | runtime check, then `EnableMedium(mgr, 2, true)` while two views run | confirmed (writer), guess (visible effect) |
| B | Water-drop camera speed / acceleration (`WaterDrop` = `RenderLoop+0x1ec0`, slot stride 0x68 from `+0x60`: `+0xa4` speed, `+0xb0` accel) | `FUN_1403be8e0`, loop `idx < 2` over `g_pGameEngine vtbl+0x570(idx)` (player actor); null for slot 1 | lens droplets in view 1 get no motion streaks (rain only) | mirror slot 0 -> 1 per frame | likely |
| C | DOF focus per slot (`KTUDof+0x368/+0x370`) | `KTUDof::OnManage` 0x1409e6680: slot 1 only if `Renderer+0xe40 != 0` and `KTUDof+0x364 != -1` (a DOF target pushed with mask bit 2) | default focus in view 1 when a script/cutscene uses DOF with mask 1 (`SetCameraDof` uses mask 3: fine) | runtime check (cutscene, replay) | confirmed (mechanism), guess (impact) |

**Re-entrancy (state written in view 0, read in view 1):** the single TAA reset byte `RenderLoop+0xd08` (cleared at the end of view 0's `RenderTAA`; the mod already writes it before each view, Phase 4d); the `camIdx == 0` work (keyed on camera index 0, not on "first view": breaks only if index 1 were rendered alone); `SimpleForwardRenderer::Render` once per frame; projection-type state `DeferredRenderLoop+0x2740` (same camera for both views); shared transients re-prepared per view (`RenderContext` at `+0x1f00`, MV RT, linear depth, tonemap `tm+0x130` swap). None is a hazard for two eyes except the ones above.

---

## 7. GPU timing

**Engine whole-frame timer** (`confirmed`): ring of 4 slots (`Renderer+0xec8 & 3`), 3 queries per slot at `Renderer + (slot*3 + 0x1cd)*8` (start TIMESTAMP, end TIMESTAMP, TIMESTAMP_DISJOINT). `FUN_140865330` (call 0x1405af5fc in `OnRender`): Begin(disjoint), End(ts0). `FUN_140865160` (call 0x1405af3c7 in `OnFlushRender`, after queue execution): End(ts1), End(disjoint). `FUN_1408651f0` (call 0x1405af33d in `OnFlip`): spins on the previous slot, writes the GPU frame time in seconds to `Renderer+0xf28`; `OnFlip` then increments `+0xec8`. The bracket covers both views' execution.

**Profiling events** (`confirmed`): exported `RenderLayer::BeginProfilingEvent` 0x1408d3d40 / `EndProfilingEvent` 0x1408d6a90 (both = `AddProfilingEvent` 0x1408d3aa0 with name / null) take a `SimpleGPUPerformanceQuery*` (`+0` `ID3D11Query*`, `+0xc` int queue index) and queue a kind-5 pass (payload `{q, name}` at `DAT_1414082e0[DAT_1414082f8]`, count reset in `RenderPass::FrameFlip`). At execution `ExecuteRenderPassRange` 0x1408d6ad0 calls `RenderLayerDX11` vf+0xa0 `FUN_140948010` on the context of the job that runs the range (immediate or a worker's deferred context): `End(query)`, then `ID3DUserDefinedAnnotation::BeginEvent(name)` or `EndEvent()`. The engine never calls them (no `SimpleGPUPerformanceQuery` is ever created). Kind-5 passes carry no cost, so they never move the job split.

**Use for per-eye timing:** queue a timestamp event from the `RenderContext::Prepare(view)` hook (the `DeferredRenderLoop+0x1f00` context only) at the start of each view and one at the end of `DeferredRenderLoop::Render`; Begin/End pairs keep the annotation markers balanced (and name the views in RenderDoc). Deltas give view 0 and view 1 GPU time; the engine's `Renderer+0xf28` gives the frame total.

---

## 8. Runtime checks

1. Skinned palettes after the fix: trace pairs of `vs:51bfa2c7f479d821` differ in both views while riding; TAA on the bike/rider without smear.
2. Shared shadows: one atlas clear and one cascade set per frame in the trace; both eyes' shadow edges identical; no missing shadow at the right eye's outer edge.
3. GPU timestamps: view 0 < view 1 start < end, monotonic with `RenderJobCount` 15 (events on deferred contexts).
4. `MeshPainter` activity, `s_vLastViewPos0` readers (section 5).
5. Particle medium 2 (section 6 A): log `Renderer+0xdd8` bytes `+0x28..+0x2f` and `GameContext+0x23`; A/B `EnableMedium(2, true)` and compare bike-attached dust/smoke in both eyes.
6. Water-drop slot 1 in rain (6 B): `WaterDrop+0xa4..+0xbc` for both slots. DOF slot 1 in a cutscene/replay (6 C): `KTUDof+0x364`.
