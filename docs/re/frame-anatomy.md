# Frame anatomy (Phase 0, static)

Build 4989319 (see `README.md`). Every address below refers to that exe.

**Status of this document: static (decompile) plus one runtime pass.** Everything here comes from decompilation of the snapshot exe plus the game log, except what section 4.1 and the tagged rows record from RenderDoc capture `tt2_snaefell_helmet_moving_taa_f8323.rdc` (frame 8323, helmet camera, moving, AA = FXAA_TAA, 1920x1080, dynamic resolution off, **one view**; the file lives in `../tt2vr-re/rdc/`, not in git). The pass table in section 4 is the order in which the engine *calls* the passes; section 4.1 is the observed GPU event order, and the order **matched** (exceptions in section 4.2). Each finding keeps the confidence its source gave: `confirmed` (decompile evidence read directly), `likely` (inferred from decompile, not seen at runtime), `guess`. Items marked `runtime-check` are in the checklist in section 10; items the capture settled are tagged `confirmed (runtime, f8323)`. Names are ours or Ghidra's demangled export names; `symbols.md` stays the source of truth for names.

Not recorded here (belongs elsewhere): matrices and depth convention (`conventions.md`), camera data flow (`camera.md`), per-state temporal detail (`temporal.md`).

---

## 1. Threading model

| # | Finding | Address / evidence | Confidence |
|---|---|---|---|
| 1.1 | **No render thread.** The process main thread runs simulation, view building, command recording, D3D11 submission and Present, serially, once per frame. | `WinMainLike_RunGame` 0x140159740 -> `KTUGameEngine::Run` 0x1409faf50 -> `GameContext::Run` 0x14048c9a0 -> `GameContext::RenderOneFrame` 0x14048bf80. Game log: device creation, 'GameContext Run Loop', 'World Shift', PostFlip lines all come from thread 0x3364 (7451 of 8140 lines). `VideoDevice::Create` 0x140918590 stores the creating thread id at `VideoDevice+0x88`. No 'RenderThread' strings. | confirmed |
| 1.2 | **Record-then-execute.** `RenderLoop::Render` makes no D3D calls. `RenderLayer::DrawCommandBuffer` 0x1408d6a10 appends the pass to global per-queue lists; execution happens later in `RenderLayerDX11` vtable slot 9 = `FUN_14094b320` (our name: `RenderLayerDX11::ExecuteRenderPasses`). | Queue lists `g_RenderPassQueues` 0x141408300 (7 queues, 16 bytes each); pools 0x1414082c0 (DrawPass, 0x990 bytes), 0x1414082c8 (compute, 0x3d0), 0x1414082d0 (fence), 0x1414082d8 (copy). Pass kinds: 0 draw, 1 dispatch, 2/3 fence, 4 copy, 5 profiling event (`ExecuteRenderPassRange` 0x1408d6ad0). | confirmed |
| 1.3 | **Immediate mode switch.** If `RenderLayer+0x268` is nonzero, `DrawCommandBuffer` executes the pass at once instead of queuing. Source: CVar `Engine.Renderer.UseImmediateRenderPass` (0x141407420, value byte 0x1414074a0, default 0), copied into `+0x268` in `RenderLayer::Flip` 0x1408d7410. `+0x268` is also set to 1 while the queue is being executed. | `DrawCommandBuffer` / `Flip` decompile | confirmed (default value from static init) |
| 1.4 | **Execution when.** Queue is drained from `OnFlushRender` (Renderer vtable +0x48, call site 0x14048c7d0) and again from `RenderLayer::EndRender` 0x1408d6aa0 (called in `OnFlip`). | `EndRender`: `(vtbl+0x48)(this,0)` then `DoubleBuffer::SwapBuffer`, `GraphicsObjectPool::EndFrame` | confirmed |
| 1.5 | **Deferred contexts: yes, one per enkiTS thread index.** `EnableVideoAdapter` 0x140958eb0 creates `CreateDeferredContext` for i = 1..N-1 (call 0x1409592af). N = `enki::TaskScheduler+0x14`, stored at `VideoDeviceDX11+0x488`. Context arrays: `+0x468` (ID3D11DeviceContext\*[N], index 0 = immediate), `+0x470` (ctx1), `+0x480` (annotation). Default config = 4 workers + main = **5 contexts** (this count needs a runtime read). | `g_PlayAllThreadConfig` 0x1413c3f10 `+0xd8` = 4 workers; tt2vr frame trace f10866: immediate + 4 deferred contexts on 4 worker threads, 9 command lists (same count as RenderDoc f8323), ~36k calls | confirmed (N=5 confirmed at runtime, 2026-10-16) |
| 1.6 | **Job split.** `FUN_14094b320` splits queue 0 across `RenderJobCount` jobs (CVar `Engine.Renderer.RenderJobCount`, object 0x14140d220, value 0x14140d2a0, default 15, range 1..20; forced to 1 if fewer than 3 contexts, to 4 if exactly 3). Job 0 runs on the **main thread on the immediate context** and gets twice the cost share. Jobs 1..n go to enki workers at priority 0; each records on its own thread's deferred context and calls `FinishCommandList` (call 0x14094ae9c, `RenderJob::Execute` 0x14094ae00). The main thread waits for the jobs in order and calls `ExecuteCommandList(cl, FALSE)` on the immediate context (0x14094b697). Then `RenderPass::FrameFlip` 0x1408e09e0 clears the queues. | | confirmed (list order and call sites); balancing by `DrawPass+0x96c` cost is likely |
| 1.7 | **Immediate context lock.** `VideoDeviceDX11::LockImmediateContext` 0x140957780 is a recursive Win32 mutex (`VideoDeviceDX11+0x490` wrapper, HANDLE at `+0x4a0`, owner tid `+0x4a8`). Taken by main-thread pass execution (context id 0x80), Present, GPU queries, texture Lock/CopyFrom, ImGui. The device was created with Flags 0x80 only (no `SINGLETHREADED`), so loader threads create resources. | `D3D11CreateDeviceAndSwapChain` calls at 0x140959180 / 0x1409591e9 | confirmed |
| 1.8 | **Present path.** `Renderer::OnFlip` 0x1405af2d0 -> RenderLayer vtbl+0x40 (`EndRender`) -> RenderLayer vtbl+0x50 (`Flip`) -> `RenderLayerDX11::Flip` `FUN_14094aec0` -> `IDXGISwapChain::Present(SyncInterval = vsync bool, Flags 0)`, call 0x14094b1aa on the `WindowWin32+0x48` swapchain, under the immediate-context lock (lock 0x14094b195, unlock 0x14094b2da). Then `RenderLayer::Flip` -> `VideoDeviceDX11Windows::PostFlip` 0x14095a600 (window mode change, device reset). Smart-vsync may turn vsync off after more than 60 slow frames. Swapchain desc: `R8G8B8A8_UNORM_SRGB`, BufferCount 1, `SwapEffect` DISCARD (blit model), set up at 0x1409590db..0x140959118. Present is also called from `Renderer::RenderFlipEmptyFrame` 0x1405b02b0 (resize path). | | confirmed |
| 1.9 | **No CPU frame lag.** `DoubleBuffer` is a thread-safe linear allocator reset every `EndRender`, not a two-buffer scheme. Simulation, camera, record, execute, present all finish inside one `RenderOneFrame`. `OnFlip` also spins on `GetData` for the previous frame's GPU timestamp queries (`FUN_1408651f0` 0x1408651f0), so the CPU runs at most about one frame ahead. | `DoubleBuffer::SwapBuffer` 0x14084a880 = reset | confirmed (no lag); likely (about 1 frame bound) |
| 1.10 | **enkiTS fork-join inside the frame**: visibility culling, ocean cull, `GenericInstanceSpawner`, `RenderLoop::CopyRenderInstances`, `SceneManager::Update`/`ShiftOrigin`, animation, PhysX, PopcornFX, FMOD, and the pass-execution jobs of 1.6. All joined before the frame ends. | `AddTaskSetToPipe` 0x140792a80 caller list | confirmed |
| 1.11 | **Physics is the only overlap.** `PhysicsManager::StartSimulateByConstantStep` runs right after `OnPreRender` (0x14048c5f2), joined after `OnFlushRender` (0x14048c7e4). | | likely |
| 1.12 | **Async compute is off on DX11** (`RenderLayer+0xba` stays 0; fence executors are no-ops). All dispatches go to queue 0. | `RenderLayer::IsDispatchAsync` 0x1408d7d50 | likely |
| 1.13 | **Frame counters.** `RenderLayer+0x30` (render id, `GetFrameID` 0x1403d3df0) increments on every `BeginRender`; `RenderLayer+0x34` (flip id) increments in `Flip`. A BeginRender/EndRender cycle per eye bumps `+0x30` twice per presented frame. The external-scene loop already does that today. Anything keyed on `+0x30` (candidate source of the per-object `frameId` in `Mesh::UpdateRenderData`; source not yet traced) must be checked. | `BeginRender` 0x1408d3d50, `Flip` 0x1408d7410 | confirmed (counters); the `frameId` link is open |
| 1.14 | **Runtime: deferred command lists seen.** 9 `FinishCommandlist`/`ExecuteCommandList` events: ZPrepass 1 list (Execute 3349), GBuffer 3 + 2 chunks (Execute 10141, 13900, 17824, then 17985 and 18945), cascaded shadows 1 list per cascade (Execute 23219, 26804, 32767), 2D HUD 1 list (Finish 38669). Compute dispatches, fullscreen passes, TAA, tonemap, probes run on the immediate context. RenderDoc inlines each list's draws before its Finish/Execute pair. Pairs Finish N / Execute N+2 match for 8 lists; Execute 3349 has no listed Finish and Finish 38669 no listed Execute. | RenderDoc f8323 (event ids in 4.1) | confirmed (runtime, f8323) for 1.5/1.6 (deferred contexts, split into jobs); the exact context count and `RenderJobCount` value were not read |

### Consequences for the mod (derived rules)

- If both eyes are rendered in one `RenderOneFrame`, both can be recorded and executed on the main thread. No cross-thread handoff is needed for the render itself.
- If a D3D11 hook records draw state, it must cover the **deferred contexts** too. With `RenderJobCount` = 1 (or fewer than 3 contexts) every draw goes through the immediate context.
- Any mod work on the immediate context must run on the main thread or inside `LockImmediateContext`.
- Because rendering is recorded first and executed later, **per-view data (camera constant buffers, jitter) is baked at record time.** Per-eye matrices must be in the `ViewContext` before `RenderLoop::Render` runs for that eye, not patched afterwards.
- A mod could write the two CVar values (0x14140d2a0, 0x1414074a0) in memory to force one-context execution. `guess`; unverified whether stable. Not a fix for anything, only a capture/hook convenience (and bypass toggles must stay `[debug]` per `CLAUDE.md`).

---

## 2. Per-frame call skeleton

`GameContext::RenderOneFrame` 0x14048bf80 drives everything. The Renderer is `GameEngine+0xb0`, class `RendererWin32` (vtable 0x140f7bdb0; base `Renderer` vtable 0x140f65120). `GameContext::Run` only loops over it. All rows: `confirmed`, call-site addresses from the decompile.

| Step | What | Function / call site |
|---|---|---|
| 1 | `Clock::FrameTick`; input (`InputManager::UpdateDriver`) | 0x14048bfe4; 0x14048c17c |
| 2 | `BaseObject::Process(0)` = `GameEngine::OnManage` 0x1404c0670; `World::OnManage`/`OnPostManage`; physics results fetch (scene vtbl+0x130); animation jobs (`StartProcessAnim` 0x14048c3fb / `WaitProcessAnim` 0x14048c4c9); PopcornFX `UpdatePhysicalSpawn`/`Evolve`; `HHParticlesManager::SetViewContext` per camera (0x14048c44a, runs **before** step 4, so PopcornFX may see last frame's view; `guess`) | 0x14048c344, 0x14048c362, 0x14048c39c |
| 3 | `SceneManager::UpdateScenesHierarchy` (0x14048c520) | |
| 4 | **Renderer vtbl +0x58 `OnUpdate`** (call 0x14048c536) -> `RendererWin32::OnUpdate` 0x140709070 -> `Renderer::DoUpdate` 0x1405addd0. Builds `ViewContext[0..1]` from the `Camera*` pair at `Renderer+0xe48` / `+0xe50` via `ViewContext::InitFromCamera` 0x14091aeb0 (6 call sites 0x1405ae0a7..0x1405ae2d7). Also computes the TAA jitter and sets the view mask `Renderer+0xd18`. | see sections 5 and 6 |
| 5 | `World::OnPostUpdate`, `Process(3)` (0x14048c559); `SceneManager::PreRenderScenes` (0x14048c5ac) | |
| 6 | **vtbl +0x38 `OnPreRender`** 0x1405af410 (call 0x14048c5c2) -> `RenderLoop::PreRender` 0x1408dc620: fills the active-view array `RenderLoop+0x1ea8` (count `+0x1eac`, pointers `+0x1eb0`) from the view mask | |
| 7 | `PhysicsManager::StartSimulateByConstantStep` (0x14048c5f2): physics runs on workers while rendering is recorded | |
| 8 | ImGui frame start: `Graphics::ImGuiNewFrame` 0x140923440 (call 0x14048c2a7, every frame), `ImGuiWindowManager::Begin` (0x14048c308 / 0x14048c764), debug menus (`MenuDebug::Render`) | section 8 |
| 9 | **vtbl +0x40 `OnRender`** 0x1405af470 (call 0x14048c6fe). Records the whole 3D scene. Details below. | |
| 10 | If `MenuDebug::IsUsingImGui`: `ImGuiRenderMenuDebug(720.0)`, `ImGui::Render` (call 0x14048c796) -> `ImGuiRenderDrawLists` 0x1409239e0 queues pass 'ImGui'; `ImGuiWindowManager::PostUpdate` 0x14048c7bd / 0x140513c50 | |
| 11 | **vtbl +0x48 `OnFlushRender`** 0x1405af360 (call 0x14048c7d0): executes the recorded queue (section 1.6). Then `WaitSimulateByConstantStep` (0x14048c7e4), `DeferredCallback::Call` (0x14048c7f9) | |
| 12 | **vtbl +0x50 `OnFlip`** 0x1405af2d0 (call 0x14048c84f): `EndRender`, `Flip` -> Present -> `PostFlip` | section 1.8 |

Slot numbering note: the RendererWin32 vtable lists slot 7 `OnPreRender`, 8 `OnRender`, 9 `OnFlushRender`, 10 `OnFlip`, 11 `OnUpdate`. Slot 17 (+0x88) is a shared `ret 0` stub, so the `Renderer vf+0x88` call inside `OnRender` is a no-op (ICF-folded, also named `ValueTracker::AddVar<int>`).

### 2.1 Inside `Renderer::OnRender` (single-view path, 0x1405af470)

Order, `confirmed`: `ProbeRenderer::Update` 0x14056b3a0; `HHParticlesManager::UpdateEvolve`; `EffectorMapManager::SetDeltaTime`; `HHParticlesManager::UpdatePkFxSceneCB`; `FUN_140865330` (GPU timer begin, slot = `Renderer+0xec8 & 3`); `TextureBaker::Render`; `RenderableTextureManager::RenderAll`; `RenderLoop::SetTime` 0x1408dedd0 (sets dt `RenderLoop+0x20`, `ViewContext::s_bMotionPause`); resize block (`Renderer+0xe39` set -> `ChangeRenderTargetsSize`, `OnWindowResized`, `RenderLoop` vtbl+0x38 = `OnSplitChanged`, `CreateStaticShadowMap`); `RenderLoop::RenderProbe` 0x1408ddeb0; **`RenderLoop` vtbl+0x48 `Render(RenderDesc&)`** with `RenderDesc` = `Renderer+0x30`; finally `HHParticlesManager::CullParticles` on view 0.

All of the work before `Render` is **per frame** (shared), not per view.

### 2.2 `RenderDesc` and `ViewContext` layout (likely, from decompile; field meanings need a live dump)

| Field | Offset | Note |
|---|---|---|
| `RenderDesc` base | `Renderer+0x30` | `ViewContext[2]`, stride **0x620**: view 0 at `Renderer+0x30`, view 1 at `Renderer+0x650` |
| CSM pointer | `RenderDesc+0xc40` | `CascadedShadowMapRenderer*` |
| Weather | `+0xc48` | `WeatherRenderer*` |
| Global HUD list | `+0xc50` | `HudItemList*` (split/global HUD) |
| Split mode | `+0xc60` (= `Renderer+0xc90`) | 0 off, 1 vertical, 2 horizontal, 3 = 4 views, 4 = special |
| View mask | `+0xce8` (= `Renderer+0xd18`) | bit i = ViewContext i active |
| ShiftInfo | `+0xcec` (flag `+0xcf8`) | World Shift vector |
| `ViewContext+0x450` | `Camera*` | |
| `ViewContext+0x464` | camera index (ushort) | selects every per-slot history array |
| `ViewContext+0x466` | stereoscopic byte | dead path, only read in `DeferredRenderLoop::Render` |
| `ViewContext+0x448/+0x44c` | projection offset (TAA jitter) | |
| `ViewContext+0x590` | final `RenderTarget*` | |
| `ViewContext+0x5e8` | per-view `HudItemList*` | |
| `ViewContext+0x624..+0x630` | viewport fractions (offset xy, size xy) | unverified, `guess` |

---

## 3. Which render loop is active

`Renderer::Create` 0x1405ace80 creates `DeferredRenderLoop` if `RendererDesc+0x90 == 0` (or `+0x94` set), `ForwardRenderLoop` if `== 1`; stored at `Renderer+0xd38` / `+0xd40`, active pointer `Renderer+0xd30`. **The game almost certainly uses Deferred** (GBuffer, froxel and IntelSSAO pass names exist), `likely`, `runtime-check`: read `Renderer+0xd30` and compare its vtable with 0x140f7bfc8 (Deferred) / 0x140f919a0 (Forward).

RenderLoop vtable slots (`confirmed`): 7 (+0x38) `OnSplitChanged` 0x1408dc2b0; 8 (+0x40) `PreRender` 0x1408dc620; 9 (+0x48) `Render` (Deferred 0x1408469c0 / Forward 0x140862f70); 10 (+0x50) `CopySplitRT` 0x1408da340; 11 (+0x58) `PrepareRenderInputPostProcess` 0x1408469b0; 12 (+0x60) `RenderPostProcess` 0x1408ddd90; 13/14 (+0x68/+0x70) `GetDepthTexture`/`GetNormalTexture`; 17 (+0x88) `FillSceneConstantBuffer`; 18 (+0x90) `CopyHDRRenderTarget` 0x140846180.

---

## 4. Per-view pass order (STATIC, UNCONFIRMED)

**This is the call order inside the per-view loop of `DeferredRenderLoop::Render` 0x1408469c0, read from the decompile. It is not an observed event list. Pass names are `DrawPassDesc.name` strings (offset 0 of the desc, colour at +0x08), which is what a RenderDoc marker would show if markers exist (see section 9).** Scope column: `frame` = runs once per frame before/after the view loop, `scene` = once per scene before the view loop with the whole view array, `view` = once per `ViewContext` inside the loop. Scope is a hypothesis built from call placement and from the camera-index arrays; "Temporal inputs" are the engine's history reads. Details for each temporal item are in `temporal.md`.

Before the view loop (frame and scene scope):

| Step | Pass / function | Address | Scope | Notes |
|---|---|---|---|---|
| a | `ProbeRenderer::Update`, `HHParticlesManager::UpdateEvolve`, `UpdatePkFxSceneCB`, `TextureBaker::Render`, `RenderableTextureManager::RenderAll`, `RenderLoop::SetTime`, `RenderLoop::RenderProbe` | 0x14056b3a0, 0x1408ddeb0 | frame | `RenderProbe` receives the view array (`RenderLoop+0x1ea8`) and `RenderLoop+0x10d0`, so probe relight may touch all views |
| b | Per unique scene: `VisibilityCuller::ClearFrustum/ClearOccluder`; `PrepareForCulling` + `AddCullingData` for **every view** of the scene; `UpdateVisibilityInfo`; `DeferredRenderer::SetupLights` -> `DeferredLighting::SetupLights` 0x14083d2c0 -> `CascadedShadowMapRenderer::SetupRenderContext` 0x140828320 (call 0x14083d427) with the whole view array; weather update; `FillRenderInstances`; `enki::WaitGroup::Wait`; `CopyRenderInstances` | `DeferredRenderLoop::Render` | scene | Culling is the **union of all views**; light and CSM setup shared. Per-object previous transforms are advanced here by `Mesh::UpdateRenderData` 0x1408a3f00 through `RenderEntity::FillRenderInstances` 0x1408d06c0 |

Per-view loop (`this+0x1eb0` pointer array, count `this+0x1eac`, camera index `ViewContext+0x464`). A shared `RenderContext` at `DeferredRenderLoop+0x1f00` (embedded `ViewContext` at `+0x2030`) is re-prepared per view with `RenderContext::SetSceneContext` / `RenderContext::Prepare(ViewContext const&)` 0x1408cfad0:

| # | Pass (DrawPass name or function) | Function / address | Scope | Temporal inputs |
|---|---|---|---|---|
| 1 | `UpdateDof`, `UpdateActivePostProcess` | | view | |
| 2 | `SimpleForwardRenderer::Render` | | **first view only** | |
| 3 | `HHParticlesManager` `StartRenderParticles`/`KickRenderParticles`, `AddParticleLights`; `UpdateFence` + `SceneManager::UpdatePopCornBound` only when camera idx == 0 | | view (fence: once) | sim is view-independent |
| 4 | `CullFroxel` ('Tile Min Max Depth CS', 'Clear Dispatch CS', 'Cull Light CS', 'Froxel Cull Light CS', 'Z Bin Light CS') | `DeferredLighting::CullFroxelLight` 0x140836300 | view | |
| 5 | **`RenderGBuffer`**: 'ZPrepass', 'Opaque Front', 'AlphaTest Front', 'Opaque', 'Alpha test', 'PKFX-Opaque', 'Clear GBuffer', 'Before Ocean Decal blend', 'Ocean', 'Gbuffer blend', 'Post-Decal blend', 'Decal blend', 'Decal AO blend', **'Motion Vectors'**, **'Motion Vectors front pass'** | `DeferredRenderer::RenderGBuffer` 0x140842040 | view | MV passes first call `UpdateLastFrameCameraCB` 0x1408df3c0 (shared CB `cbVSCameraLastFrameStd`, StringId 0x1414075b0). MV RT `RenderLoop+0xca8` is shared and reused per view. Condition: per-idx motion-blur flag `DAT_141407830[idx]` or TAA on **Runtime: confirmed (f8323, rows 3-8 of 4.1: ZPrepass 748..3348, GBuffer 3447..20248, MV 20321..20633).** |
| 6 | `PrepareLight` | `DeferredLighting::PrepareLightingDispatchCommand` 0x14083a010 | view | |
| 7 | `RenderLinearDepth` ('Downscale Linear Depth' appears later in the list) | | view | **Runtime: confirmed (CS 20649..20739, row 10 of 4.1).** |
| 8 | **`RenderSSR`** ('SSR pass') | `DeferredRenderer::RenderSSR` 0x140844de0 | view | **last-frame depth + last-frame HDR colour + `lastVP[idx]` + `lastCamPos[idx]` + prev resolution scale** **Runtime: confirmed order (CS 20758/20770, row 11 of 4.1); last-frame bindings not verified.** |
| 9 | CSM / shadow maps (`RenderDirectionalCascadedShadowMap` 0x140841d60 and `RenderShadowMaps` 0x140844f00 are 12-byte stubs; shadow draws are queued through the CSM render contexts 'Cascaded %d', 'Static Shadow', 'Far Shadow Compute') | `CascadedShadowMapRenderer` | per camera index (`CSM+0x25b0 + idx`, stride 0x11a0) | static shadow cache re-shifted at World Shift (`ShiftStaticShadowMap` 0x140829380) **Runtime: confirmed, D32 static pass 20818..21624 then 3 CSM cascades 21630..32765 (rows 13-14 of 4.1).** |
| 10 | **`LightFroxel`** ('Light Froxel CS') then 'Volumetric accumulation CS' | `DeferredLighting::LightFroxel` 0x140839120 | view | **froxel history ping-pong per idx** (`DL+0x780+idx*4`, `DL+0x760+(pp*2+idx)*8`) **Runtime: confirmed (CS 32784, 32801; `LastFrameLightedFroxel_tex` bound).** |
| 11 | `IntelSSAO::Render` ('IntelSSAOPrepareDepths/Normal/Generate/Blur') | 0x140927c50 | view | looks stateless (`likely`) **Runtime: confirmed (32879..33317, apply 33354).** |
| 12 | `MeshPainter::Update` | | view | |
| 13 | `RenderLighting` ('Main Lighting', 'Fog On Sky', 'Sum Lighting CS') | `DeferredLighting::RenderLightDispatchCommand` 0x14083a840 | view | **Runtime: confirmed ('Sum Lighting CS' 33418).** |
| 14 | `SkyRenderer::Render` ('Render Sky Deferred') | 0x140909c30 | view | reads `s_vResolutionScaling` **Runtime: confirmed (33464, 33515 into HDR `3591`).** |
| 15 | `RenderForwardOpaque` ('Render Forward Opaque') + Front variant; `UpdatePostProcessGeom` | | view | |
| 16 | `RenderFog` ('RenderFog') | `RenderLoop::RenderFog` 0x1408dd330 | view | |
| 17 | `vf+0x90 CopyHDR` (`CopyHDRRenderTarget` 0x140846180) | | view | **Runtime: confirmed (33544 into `3596`).** |
| 18 | `RenderDeferredTransparency`, `RenderLowResTransparency` ('LowRes Transparency 125', 'Transparency Low Res', 'Render Transparency', 'Final Blend pass') | `DeferredRenderer::RenderLowResTransparency` 0x140843f20 | view | **Runtime: transparency/forward confirmed as Colour Pass #7 33544..35759 (contents not separable without markers).** |
| 19 | **HUD flag 8** ('HUD3D Renderer Frontpass'): 3D HUD into the HDR scene RT, before post | `HudRenderer::Render` 0x14088b530 | view | **Runtime: **not seen** (no events between 33544..35759 identified as 3D HUD flag 8; open, 4.2).** |
| 20 | **`CopyBuffersForNextFrame`** ('Generate last frame mips', 7 mip passes, texture name 'LinearInput'): copies scene depth to 'Last Frame Depth' and HDR colour (pre-tonemap, **after** transparency and the flag-8 HUD) to 'Last Frame Buffer'. **Conditional**: `DAT_1413cbda0` && per-ctx flag, or `WaterDrop::HasAliveParticles(idx)`, or view flag `[0x61e]` | `RenderLoop::CopyBuffersForNextFrame` 0x1408d9ee0 (call 0x14084867f) | view | writes the last-frame colour/depth history **Runtime: confirmed (35759 Last Frame Depth, 35786..35957 Last Frame Buffer; copies ran).** |
| 21 | `WaterDrop::Render` | | view | per-idx state; uses last-frame colour |
| 22 | **ToneMapper** ('Downscale', 'HDR_LuminanceInit', 'HDR_LuminanceDownscale', 'HDR_LuminanceFinal', 'HDR_FinalCompose'); CPU readback of the 1x1 luminance to `tm+0x148` | `ToneMapper::Render` `FUN_140914680` (call 0x140848768); `FUN_1409120b0`, `FUN_1409131d0` | view | **auto-exposure: per-idx previous-luminance RT** `tm+0x138+idx*8`, first-frame flag `tm+0x14e+idx` **Runtime: confirmed (bloom 35990..36339, luminance 36541/36570/36606, `HDR_FinalCompose` 36652).** |
| 23 | `RenderFXAA` (AA mode 1 or 2) | | view | **Runtime: **no FXAA pass seen** with AA = FXAA_TAA (open, 4.2).** |
| 24 | `PreUpdateLastFrameCamera` (World Shift rebase of stored camera positions) | 0x1408dc6b0 (call 0x140848bf4) | view | |
| 25 | **`RenderTAA`** ('TAA resolve'), then `NoMotionVectorFixTAA` | `RenderLoop::RenderTAA` 0x1408ddf70 (call 0x140848c03); `NoMotionVectorFixTAA` 0x1408dc0e0 | view | **TAA history RT pair per idx**, `lastVP[idx]`, `lastJitter[idx]`, MV RT, reset flag `RenderLoop+0xd08` **Runtime: confirmed TAA 36731 + copy 36758; `NoMotionVectorFixTAA` only a candidate (36786, open).** |
| 26 | `RenderMotionBlur` ('Render MotionBlur') | 0x1408dd5e0 | view | `lastVP[idx]`, `lastCamPos[idx]`, MV RT, dt **Runtime: confirmed (36833, after TAA).** |
| 27 | `PostUpdateLastFrameCamera` (latches `lastVP`, `lastCamPos`, `lastJitter` for the next frame); `ViewContext::ResetProjectionOffset` (removes the jitter) | 0x1408dc2f0 (call 0x140848c58); ResetProjectionOffset 0x14091b570 | view | |
| 28 | `LensFlareManager::Render` ('Lens flares occlusions query', 'Lens flares render') | | view | per-idx queries (`LensFlareInstance::Update` 0x140898df0) **Runtime: not seen as a distinct pass (no lens-flare events identified).** |
| 29 | `RenderPostProcess` (`PostProcessManager::Render` 0x1408bed60, call 0x1408dde8c; DOF, bloom, grading are data-driven .PFX chains; 'PostProcess Read') | `RenderLoop::RenderPostProcess` 0x1408ddd90 | view | **Runtime: confirmed order (36912 LDR pass, 36947 colour grading); DOF not separable.** |
| 30 | `RenderTransparency` (after post) | | view | |
| 31 | **HUD flag 0x14 (= 4 + 0x10)**: 3D HUD on the tonemapped RT, after TAA, MB, lens flare, post; stencil cleared first (`ClearRT(...,4,0xff000000)`), 0x10 = no depth test | `HudRenderer::Render` | view | **Runtime: **not seen** (no 3D HUD between 36947 and 36985; open, 4.2).** |
| 32 | `FUN_1408c1dd0` (purpose unknown) | | view | |
| 33 | `UpscaleDynamicResolution` ('Dynamic resolution upscale', 'upscale_simple') | 0x1408df750 | view | `s_vResolutionScaling` **Runtime: **no separate upscale** with dynamic resolution off; the final blit 36985 is 1:1 (open, 4.2).** |
| 34 | **HUD flag 0x22 (= 2 + 0x20)** ('HUD2D Renderer'): per-camera 2D HUD list `ViewContext+0x5e8` | `HudRenderer::Render` | view | **Runtime: confirmed, 2D HUD 37099..38606 drawn directly into the back buffer after the final blit 36985.** |
| 35 | `RenderDebug` | | view | **Runtime: not observed.** |
| 36 | `CopySplitRT` (vtbl +0x50) when `RenderTargetManager+0xc != 0` | 0x1408da340 | view | |

After the view loop: 'Split border' pass (0x14084935e); global HUD list `RenderDesc+0xc50` with flag 0x22 ('Split n HUD') into the final RT `*(ViewContext+0x590)+0x18`; debug bounds; screenshot dumps. Then, outside the render loop: 'ImGui' pass queued by `ImGui::Render` (step 10 of section 2), on the back buffer.

Other pass names seen in strings, position not established: 'Downscale Linear Depth', 'PostProcess Read', 'Oculus render mirror' (dead, section 7), 'Render Movieplayer', 'SceneRenderer', 'Wait graphics queue' (a GPU fence label only).

**Per-frame vs per-view summary (hypothesis, `runtime-check`):**

- Shared or once per frame: everything in 2.1 before `Render`, probes, culling (union of views), light and CSM setup call, `FillRenderInstances`/`CopyRenderInstances`, weather, particle `UpdateFence`, `SimpleForwardRenderer::Render` (first view only), `CullParticles`.
- Per view: GBuffer, SSR, froxel cull/light, SSAO, lighting, sky, fog, transparency, tonemap, FXAA, TAA, motion blur, lens flare, post chain, all HUD lists, upscale.
- Single objects hit once per view inside the loop (hazards): the **ToneMapper** call (step 22), the **shared `RenderContext`** at `+0x1f00`, the **shared MV RT**, the **shared GBuffer/HDR RTs** (reused serially, which is safe because each view runs its whole chain before the next).

---

### 4.1 Runtime pass table (capture f8323)

Source: RenderDoc capture `tt2_snaefell_helmet_moving_taa_f8323.rdc` (not in git), frame 8323, helmet camera, moving, daytime, AA = FXAA_TAA (TAA observed), 1920x1080, dynamic resolution off, one view. Analysed through the RenderDoc MCP bridge. **C** = confirmed in the capture (event id or value given), **G** = guess (inferred from shape/order). Real events are 374..38673; ids above 38673 are RenderDoc auto-group ids. RenderDoc lists each deferred list's draws before its `FinishCommandlist`/`ExecuteCommandList` pair.

Capture-wide facts:

- **No GPU markers** (confirmed, f8323): the event list has no engine pass names; passes are identified by structure (formats, shader resource names, order). Matches section 9's decompile reading.
- Totals: 2602 actions; 30 Dispatch; 86 non-indexed `Draw()` (fullscreen/utility); ClearDepthStencilView at 584, 20778, 36989; ClearRTV at 20248; many DiscardView.
- **No `CopyResource`/`CopySubresourceRegion`** (confirmed): the engine copies with fullscreen draws (for example 35759, 35786.., 36758, 36985), so a hook cannot rely on copy calls to find them.
- **Deferred command lists: 9** (list in 1.14). Per pass: ZPrepass 1, GBuffer 3 + 2 chunks, cascaded shadows 1 per cascade (3), 2D HUD 1. Compute, fullscreen, TAA and tonemap are on the immediate context.
- **2D HUD** draws **37099..38606** directly into the back buffer `337` (R8G8B8A8_SRGB RTV) **after** the final blit at 36985; **Present at 38673**.
- Key resource ids: depth `339` (D32_FLOAT_S8X24, DSV 341/342, SRV 340), back buffer `337`, GBuffer `3571` / `3575` / `3579`, motion vectors `3399`, HDR scene `3591`, HDR copy `3596`, LDR `3101` / `3105`.
- Sizes are the viewport at the draw (the bridge returns no texture descriptions); formats come from RT/SRV views.

Columns: Scope = per frame (shared) or per view (G where inferred); Temporal = history the pass reads or writes.

| # | Events | Guessed engine pass | RTs / depth (format, size) | Inputs | Scope | Temporal | C/G |
|---|---|---|---|---|---|---|---|
| 1 | 374..573 (18 CS) | Probe filtering (`cbProbeFilteringParam`, `EnvironmentTexture_tex` -> `Dest`) = `RenderProbe` | UAV `Dest` (uint4) | cube env map | per frame (G) | probe state | C names, G scope |
| 2 | 581..584 | Discard x3 + ClearDepthStencilView 584 on 339 | depth 339 | - | per view | - | C |
| 3 | 748..3348 (196 DrawIndexed; Execute 3349) | **ZPrepass** | no RT; 339 D32_FLOAT_S8X24, GEQUAL, write on, stencil off; 1920x1080, **viewport MinDepth 0 / MaxDepth 0.95** | VS only | per view | - | C |
| 4 | 3447..17914 (Colour Pass #1; lists Finish 10139 / 13898 / 17822) | **GBuffer** (opaque, alpha test; sub-passes not separable) | `3571` R10G10B10A2_UNORM, `3575` R8G8B8A8_SRGB, `3579` R8G8B8A8_UNORM, 1920x1080; 339 GEQUAL, write on, **stencil write mask 0x14, Replace** (4133), MaxDepth 0.95 | BC1_SRGB / BC1_UNORM materials | per view | - | C |
| 5 | 17955 | Fullscreen copy (`RenderTarget0_tex`) | `3587` R32_FLOAT | `RenderTarget0_tex` | per view | - | C copy, G purpose |
| 6 | 17982 (+ list Finish 17983 / Execute 17985) | Same copy shader | `3583` B8G8R8A8_SRGB | `RenderTarget0_tex` | per view | - | C copy, G purpose |
| 7 | 18446..20248 (Colour Pass #3; Finish 18943 / Execute 18945; ClearRTV 20248) | GBuffer continuation (decals, blend) | same 3 GBuffer RTs (checked 18446, 18462, 19231, 20248); 339 | - | per view | - | C RTs, G contents |
| 8 | 20321..20633 (22 DrawIndexed) | **Motion vectors** (front pass G) | `3399` R16G16_FLOAT (RTV 3402), size G 1920x1080; 339 | - | per view | last-frame matrices (G) | C RT/format |
| 9 | 20636..20638 | Discard x3 | - | - | per view | - | C |
| 10 | 20649, 20664, ..., 20739 (7 CS) | **Linear depth** CS (`Depth_tex` -> `LinearDepthResult`, `VisibilityResult`, `LinearDepthMip0Result`; cbCameraStd slot 2) | UAVs | depth 339 | per view | - | C |
| 11 | 20758, 20770 (2 CS) | **SSR** CS (20770: `SSRTempRead` -> `SSRResult`, cbViewportStd) | UAV `SSRResult` float4 | `SSRTempRead` | per view | last-frame depth/colour (G, bindings not verified) | C 20770, G 20758 |
| 12 | 20778 | ClearDepthStencilView (shadow depth) | 3730 (G) | - | - | - | G |
| 13 | 20818..21624 (73 children) | Depth-only Pass #2: static/far shadow (G), has a PS | D32 `3730`, viewport 2048x2048 | - | per frame (G) | static shadow cache (G) | C format/size, G role |
| 14 | 21630..32765 (Depth-only Pass #3, 799 children; lists Finish 23217 / 26802 / 32765) | **Cascaded shadow maps, 3 cascades**, one deferred list each (draws 22525 / 25915 / 29348 checked) | `3736` D16 atlas; viewports 2048x2048 at x = 0 / 2048 / 4096, scissor 1536 / 1504 / 1496 px square | VS only | per frame (G) | - | C |
| 15 | 32784 (CS 1x1x32 groups) | **Light Froxel CS** (`LightedFroxel` 3D UAV) | 3D UAV | StaticShadowMap, ShadowMap, FarShadowHeightMap, CloudShadow, `LastFrameLightedFroxel_tex` (3D) | per view | **froxel history** | C |
| 16 | 32801 (CS 8x8) | **Volumetric accumulation CS** (`LightedFroxelRead` -> `VolumetricLight` 3D) | 3D UAV | - | per view | - | C |
| 17 | 32879 | IntelSSAO prepare (`g_NormalmapSource`) | `3249` B8G8R8A8_UNORM | normals | per view | - | C shader, G role |
| 18 | 32918..33317 | **IntelSSAO** (prepare depths / generate / blur) | 4x R16_FLOAT (`3195/3205/3215/3225`), R8G8_UNORM `3235` | depth, normals | per view | - | C RT/format, G assignment |
| 19 | 33354 | SSAO apply (`g_FinalSSAO`, cb `g_ASSAOConsts`) | GBuffer RT0/RT1 (`3571`/`3575`); 339 | `g_FinalSSAO` | per view | - | C |
| 20 | 33418 (CS 8x8) | **Sum Lighting CS** (UAV `Result`; GBuffer, shadows, `SSR`, probes, `FroxelScatteringAccumulation_tex`, IES, cloud shadow; cbSceneStd, cbCameraStd, cbPSCascadedShadows, cbPSStdEnvLight, cbCSLightData, cbFroxelParam) | UAV `Result` (G: HDR `3591`) | GBuffer, SSR, froxel | per view | SSR result, froxel | C shader, G target |
| 21 | 33464, 33515 (Colour Pass #6) | **Sky** (`cbSkyParam`, DepthBuffer) | `3591` R16G16B16A16_FLOAT (HDR scene); 339 | depth | per view | - | C |
| 22 | 33544 | **CopyHDR** (`RenderTarget0_tex` copy) | `3596` RGBA16F; 339 bound | HDR `3591` | per view | - | C copy, G name |
| 23 | 33544..35759 (Colour Pass #7, 136 children) | Forward opaque / fog / transparency / PKFX (G) | `3591` RGBA16F; 339 GEQUAL, **write off** (33703) | - | per view | - | C RT/state, G contents |
| 24 | 35759 | **Last Frame Depth** copy | `3284` R32_FLOAT 1920x1080 | depth 339 (SRV 340) | per view | writes history | C |
| 25 | 35786, 35819, 35842, 35865, 35888, 35911, 35934, 35957 | **Last Frame Buffer** mip chain | `3288` RGBA16F, 8 mips (mip0 1920x1080, mip7 15x8) | HDR `3591`, previous mip | per view | writes history | C |
| 26 | 35990..36339 | **Bloom** down/up chain | `3306` 960x540, `3314`, `3322`, `3330` 120x67, RGBA16F; `3338` | HDR `3591` | per view | - | C RT/size, G naming |
| 27 | 36370..36505 (5 Draw) | Unidentified (G: bloom up / DOF / luminance init) | G | - | per view | - | G |
| 28 | 36541, 36570, 36606 | **Exposure luminance chain** | `3372` R16_FLOAT 16x16 (36541); 36570 likely 4x4 (G); `3385` R16_FLOAT 1x1 (36606, reads `3376` + `3380`) | HDR via chain | per view (state shared by rule 1) | **previous luminance** | C 36541/36606, G 36570 |
| 29 | 36652 | **Tonemap = HDR_FinalCompose** (PS 1976; stencil test NotEqual ref 4 mask 4) | `3101` B8G8R8A8_SRGB 1920x1080 | t0 `81` HDRLut, t1 `3591`, t2 `3385`, t4..t8 bloom `3306/3314/3322/3330/3338`, t9 `82096` BloomDirt BC1_SRGB, t10 `3356` water-drop overlay, t11 `3393` | per view | exposure, bloom | C |
| 30 | 36692 | Film grain (`GrainTexture_tex`, `ColorBuffer_tex`) | `3105` B8G8R8A8_SRGB | `3101` | per view | - | C shader |
| 31 | 36731 | **TAA resolve** | 2 RTs `3268`, `3272` B8G8R8A8_SRGB; depth test off | depth 339, `FrameBuffer` `3105`, `LastFrameBuffer` `3276`, MV `3399` | per view | history `3276` | C |
| 32 | 36758 | Copy `3272` -> `3101` | `3101` | `3272` | per view | - | C |
| 33 | 36786 | PS without inputs, dual RT `3268`/`3272`, depth 339 bound: **`NoMotionVectorFixTAA` candidate** (stencil-gated fill) | `3268`, `3272` | none | per view | - | G |
| 34 | 36833 | **Motion blur** (DepthBuffer_tex, FrameBuffer_tex, MotionVectors_tex, cbCameraStd, `g_viewProjectionLastFrame`, `g_motionBlurParam1/2`) | `3105` | MV `3399` | per view | last VP | C |
| 35 | 36912 | Unidentified LDR pass (G: vignette / sharpen / lens) | `3101` | `3105` | per view | - | G |
| 36 | 36947 | **Colour grading** (`ColorGradingSampler_tex` 3D LUT) | `3105` | `RenderTarget0_tex` | per view | - | C |
| 37 | 36985, then ClearDSV 36989 | **Final blit**: `3105` B8G8R8A8_SRGB -> back buffer `337` (RTV R8G8B8A8_SRGB), 1:1, no upscale | `337` | `3105` | per view | - | C |
| 38 | 37099..38606 (100 DrawIndexed; list Finish 38669) | **2D HUD** straight into the back buffer, depth test disabled | `337` R8G8B8A8_SRGB | - | per view | - | C |
| 39 | 38673 | Present(`337`) | - | - | - | - | C |

Observed order: probes -> clear -> ZPrepass -> GBuffer (+ copies, decals) -> motion vectors -> linear depth -> SSR -> shadows (D32 static pass, 3 cascades) -> froxel -> volumetric -> SSAO -> SSAO apply -> Sum Lighting -> sky -> CopyHDR -> forward/transparency -> Last Frame Depth/Buffer -> bloom -> luminance -> tonemap -> grain -> TAA -> copy -> NoMVFix candidate -> motion blur -> LDR pass -> colour grade -> final blit -> 2D HUD -> Present. **This matches the static order of section 4 (confirmed, f8323)**, with the exceptions in 4.2. Per-view vs per-frame scope is still by static reading: the capture contains one view.

### 4.2 Open items from the capture (contradictions with the static table)

1. **No FXAA pass** between tonemap 36652 and TAA 36731 although AA = FXAA_TAA (static step 23 `RenderFXAA` for modes 1 and 2). Only film grain 36692 sits between. FXAA is folded into `HDR_FinalCompose`, skipped, or hidden in the unidentified passes 27/35 (unresolved).
2. **No 3D HUD** pass between colour grading 36947 and the final blit 36985 (only a DiscardView at 36951), and none identified as the flag-8 HUD before the post chain (static steps 19 and 31). In the helmet view the dashboard may simply not draw as a separate pass, or it is inside Colour Pass #7 (unresolved; sample a cockpit-dashboard view).
3. **No separate dynamic-resolution upscale** with dyn-res off: the final blit 36985 is a 1:1 fullscreen draw into the back buffer (static step 33 expects 'Dynamic resolution upscale'). The pass probably only runs with scale < 1 (likely; untested).
4. **Event 36786** (inputless PS, dual RT `3268`/`3272`, depth 339 bound) is the candidate for `NoMotionVectorFixTAA`; not proven.
5. Also open: front-pass viewport range (not sampled), TAA texture rotation across frames, clear value 0.0 (args unreadable); see `conventions.md` 3.7 and `temporal.md` 9.
6. New finding: world ZPrepass/GBuffer draws use **viewport MaxDepth 0.95** (events 748, 3348, 4133), contradicting the earlier assumption of 1.0; see `conventions.md` 3.7.

## 5. Split-screen multi-view mechanism

This is the engine's only live multi-view path and the best candidate vehicle for synchronous stereo (hypothesis C in `temporal.md`). `confirmed` unless marked.

- **Split mode.** `GameEngine+0x178` = mode (0 off, 1 vertical / side by side, 2 horizontal / top-bottom); `+0x184` = split-on byte. Setter `GameEngine::SetSplitMode` 0x1404d4710 calls `RenderTargetManager::SetPossibleSplitMode` 0x1408e2080 and `EffectorMapManager::SetSplitMode`. `RenderTargetManager::SetSplitMode` 0x1408e2090 **rejects any mode except the one set as 'PossibleSplitMode'** (`RenderTargetManager+8`; read from the Lua video config key `PossibleSplitMode` in `GameEngine::ConfigureVideoDevice` 0x1404aa550). Active mode at `RenderTargetManager+0xc`. `SwitchSplit` (0x1403d7c30, Lua binding `FUN_1409ecf50`, GUI table 0x14104a620) toggles to mode 1, or mode 2 if already 1. `SetSplitModeOn` 0x1404d4760 refuses in network sessions. How split mode is enabled in a single-player race, and what fills `Renderer+0xe50`, is **not established** (open question, `runtime-check`).
- **View build.** `Renderer::DoUpdate` switches on `Renderer+0xc90`. Mode 0: one ViewContext (slot = ctz(mask)). Modes 1 and 2: `InitFromCamera(Renderer+0x30, Camera@+0xe48, ...)` and `InitFromCamera(Renderer+0x650, Camera@+0xe50, ...)` with half-size viewports (mode 1: view 1 x-offset fraction 0.5, width 0.5, from `[0xc44]=0.5, [0xc4c]=0.5, [0xc50]=1.0`).
- **Render targets.** `RenderTargetManager::TransformViewport` 0x1408e20e0 halves the width (mode 1) or height (mode 2). `ComputeSplitSize` 0x1408e1960 sizes the RTs; `CreateSplitRenderTarget` 0x1408e1ca0 makes the 'Split' RT. **Open problem for VR: each eye RT would be half the window width** unless `ComputeSplitSize` / `TransformViewport` are patched or the window is made 2x eye width (`runtime-check`; whether any path hardcodes width/2 is unknown).
- **View count.** `DeferredRenderLoop::Render` derives the count from `RenderDesc+0xc60`: modes 1, 2, 4 give 2 views, mode 3 gives 4, mode 0 gives 1. Per-slot history arrays only hold **2** entries, so the 4-view mode would overflow them.
- **One call, all views.** `Render` loops over the active views sequentially in a single call, reusing the same GBuffer/HDR RTs, with culling, lights and CSM setup shared across the view array (section 4 b). `CopySplitRT` (vtbl +0x50) stashes each view's result; 'Split border' draws after.
- **Per-view history exists only for 2 slots and is allocated lazily.** `RenderLoop::Create` calls `InitializeTAA(false,false)` and `InitializeLastFrameBuffers(false)`, which allocate slot 0 only. `RenderLoop::OnSplitChanged` 0x1408dc2b0 (called from the `OnRender` resize block via vtbl +0x38) calls `InitializeTAA(true, p)` and `InitializeLastFrameBuffers(p)`, extending to slot 1.
- **Jitter is disabled in split mode.** `DoUpdate` computes it only when `Renderer+0x640 == 0` and (`splitMode == 0` or `Renderer+0xc60 == 0`). Both views would get the same offset if it were on (one counter increment per frame). See `temporal.md` for the consequence under the temporal rule.
- **HUD in split mode** is composed by the engine ('Split n HUD', two HUD item lists), not as an RT per eye.
- **External scenes.** `Renderer::OnRender` has an `ExternalSceneRendering` path (enable `Renderer+0xe08`, count `+0xe14`, entries `+0xe18`, RT `+0xe20`, max 4 registered via `RegisterExternalSceneRenderer` 0x1405b01e0, which has no code callers). When enabled it overrides the cameras/viewports of views 0 and 1 in `DoUpdate`, and in `OnRender` it re-runs `InitFromCamera` + `PreRender` + `RenderProbe` + `Render` once per entry (iterating from the last entry down to 0; the first is marked at `ViewContext+0x510`), draws into the 'ExternalSceneRendering' RT, composites with a 'SceneRenderer' quad, and runs `EndRender`/`BeginRender` per entry. **Proves `Render(RenderDesc&)` is re-entrant inside one frame**, and bumps `RenderLayer+0x30` per entry. Used for menu previews (bike garage, `likely`); a VR hook must not treat those as the main view.
- **Other re-entry** is not in-frame: `ProbeRenderer::RenderAt` 0x140568410 (offline probe bake); `HighResolutionScreenshot` (`MakeScreenshot` 0x140873da0, `MakeTemporalAntiAliasingScreenshot` 0x140874710) and Ansel change the projection and accumulate over many frames.

Candidate per-view injection points (all `guess` until tested; order by cost):

1. Overwrite `ViewContext[0]`/`[1]` after `DoUpdate` (view +0x464 camera idx, +0x450 Camera*, stride 0x620), force split mode 1 with RT split size patched to full eye resolution.
2. Hook `ViewContext::InitFromCamera` 0x14091aeb0 and replace view and projection for camera idx 0/1. Callers: `DoUpdate` x6, `GameEngine::UpdateShift` (0x1404d5a41), `OnRender` (0x1405afcb3), `ProbeRenderer::RenderAt`, `Pick3dInList`, `RenderContext::Prepare` (0x1408cfae0, a 19-byte wrapper). Only the `DoUpdate` calls are the main views.
3. Replace the final composition (`CopySplitRT` + 'Split border') with an OpenXR submit.
4. Call `RenderLoop` vtbl +0x40/+0x48 per eye (re-entrancy proven by external scenes). Per-eye history would then need mechanism A or B (`temporal.md`).
5. `ViewContext::InitFromCameraStereo` 0x14091b100 / `SetViewProjectionStereo` 0x14091b850: **dead code**, see section 7.

---

## 6. Jitter and projection entry (pointer only)

`DoUpdate` alternates the jitter by the global counter `DAT_1413a88c4`: even frames `(1/viewportW, 0)`, odd frames `(0, 1/viewportH)`; passed to `InitFromCamera`, which adds it to projection elements `[2][0]`/`[2][1]` (byte offsets +8, +0x18) and stores it at `ViewContext+0x448/+0x44c`. Details and gates: `temporal.md`. Projection: `Math::MatrixPerspectiveFovRH` 0x140813390 (right-handed, reversed-Z, finite far): see `conventions.md` and `camera.md`.

---

## 7. Native Oculus / stereo remnants (dead code)

`confirmed`. `PlayAll::Game::Oculus` (vftable 0x140fa7890, ctors 0x140701480 / 0x140701700 / 0x1407019b0) has **no code callers** and no setting enables it. `ViewContext::InitFromCameraStereo` 0x14091b100, `SetViewProjectionStereo` 0x14091b850 and `IsStereoscopic` 0x1404fd5c0 have only data/export references. `ViewContext` carries two `TransformMatrix` blocks (eye 0 at +0x18, eye 1 at +0x1a8, stride 0x190) and a stereo byte at `+0x466`, but `SetActive` 0x14091b780 and `RebuildFrustum` 0x14091b4e0 read slot 0 only. The only reader of `+0x466` in the render loop is `DeferredRenderLoop::Render`, where it clears the lighting path flag (`pVVar2[0x466] != 0`, with `cVar42 != 0 || bVar10 || splitmode == 4`); the lens-flare note says it disables lens flares and still renders the HUD (two notes describe the same check differently; see contradictions). No shader-side stereo (no `SV_ViewportArrayIndex`, no eye-index strings), no single-pass stereo. `FUN_140bdb060` resolves LibOVR 1.x (`ovr_*` pointer table 0x141427630..0x1414277f8); no `LibOVRRT*.dll` ships. **Do not plan around reviving it.** It is a reference for the intended data layout only: the old design used a separate HUD swapchain at 0.7x eye width (`Oculus+0x1e0`, `GetCurrentHUDRenderTarget` 0x140702870) submitted as a quad layer, which matches our HUD-as-quad plan.

---

## 8. HUD, ImGui, movie and menu states

### 8.1 HUD insertion points

One renderer, `HudRenderer` (`RenderLoop+0x410`), `HudRenderer::Render` 0x14088b530 (RenderTarget\*, HudItemList\*, RenderLoop\*, RenderContext\*, CameraRenderDesc\*, flags, bool). Pass names 'HUD3D Renderer', 'HUD3D Renderer Frontpass', 'HUD2D Renderer'. Flag bits: **8** = 3D HUD before post (HDR scene RT, `RenderLoop+0x1ef0 -> +0x1d8`), **4** = 3D HUD after post (current PostProcessManager RT), **2** = 2D HUD, **0x10** = no depth test for 3D, **0x20** = 2D after dynamic-res upscale. `confirmed` (decompile) for the call flags below; target RT choices are `likely`.

| Call | Flags | Position in the view chain | Target | Per-view? |
|---|---|---|---|---|
| Deferred #1 | 8 | after deferred transparency, before tonemap | HDR scene RT | yes, perspective from `RenderContext+0x130` |
| Deferred #2 | 0x14 | after post and `RenderTransparency`, before upscale; stencil cleared first | current PostProcessManager RT | yes |
| Deferred #3 | 0x22 | after `UpscaleDynamicResolution`, then `RenderDebug` | current PostProcessManager RT (upscaled); per-camera list `ViewContext+0x5e8` | yes, ortho at TargetInfo size |
| Deferred #4 | 0x22 | after the view loop: global/split list `RenderDesc+0xc50` | final RT `*(CameraRenderDesc+0x590)+0x18` | once (global) |
| Forward | 4 then 2 (after 'Clear Stencil'), global list with 2 | | current PP RT | |

- HUD items are `HudNode`/`FlashRenderer` trees (Flash vector UI; material `DATA:/ENGINE/EFFECTS/QUADTEXTURECOLOR3D.PFX`, created in `GuiManager::CreateDefaultMaterials` 0x140888ef0). There is **no intermediate UI-only target**: the HUD composites in place on the scene RT.
- The **cockpit dashboard** (`TTDashboardHUD::Create` `FUN_140202260`, Flash symbols `J_HUD_AnalogGauge`, `TXT_KMPH`, `TXT_Speed`) is registered as a 3D list via `GameContext::GetHudItemList(0)->Add3DList` (`likely`), so it is a world-placed 3D HUD and **inherits the per-eye ViewContext**. `TTHotSpot3D` (`FUN_140205930`/`FUN_140205ad0`) likewise. 3D lists: `HudItemList::Get3DList` 0x140879820, `Get3DBeforePostProcessList` 0x1408797c0, `Add3DList` 0x140879730.
- Per-camera `HudItemList` table at `Renderer+0xdf8+idx*8` (`Renderer::GetHudItemList` 0x1403de850); camera -1 = global list at `GameContext+0x88` (`GameContext::GetHudItemList` 0x14048b4c0). Menus (`KTUGuiManager`) use the global list.
- **Rule for the mod:** the 3D HUD (flags 8, 0x14) is part of the eye render and is already per eye. The 2D HUD (0x22) is screen-space ortho; to put it on a quad layer, redirect or capture the 0x22 passes (per-view and global), and copy the scene colour **before** the flag-0x14 call if a HUD-free eye image is wanted. Whether flag-8 3D HUD elements should be in the eye image is a design decision (they are in-world, so yes).

### 8.2 Ansel `markBufferBind`

Only importer is `FUN_140948b50` (call 0x140948dca, IAT 0x140ec8030). It is the DX11 render-target bind of a DrawPass, called only from the draw-pass executor `FUN_1409480f0` (call 0x1409486bf). It calls `markBufferBind(0, 0, -1)` only if `isAnselAvailable()` (call 0x140948db8) **and** `DrawPassDesc+0x4b4 & 0x10`. Mangled export read from the DLL: `?markBufferBind@ansel@@YAXW4BufferType@1@W4HintType@1@_K@Z`. Who sets flag 0x10 was **not found statically** (descs live on the stack; only `DrawPassDesc::Reset` 0x1408e0af0 zeroes it; byte scan found only the two TEST sites 0x140948da8 and 0x140948df8). Enum values `guess`. Ansel is active only if the NVIDIA driver reports availability, so this is **not** a reliable HUD-free anchor; use the pass structure above.

### 8.3 Dear ImGui (game's own)

Old Dear ImGui (1.4x era: `ImGuiDrawContext`, `ImGuiTextEditState`, `RenderDrawListsFn`) statically linked, 345 exported `ImGui::` names (e.g. `CreateContext` 0x1407a0220, `GetCurrentContext` 0x1407a3b60). `ImGuiInit` 0x140922590 sets `RenderDrawListsFn` (thunk 0x1409239d0). Per frame (section 2 steps 8 and 10): `ImGuiNewFrame` 0x140923440 every frame, then if `MenuDebug::IsUsingImGui`: `ImGui::Render` -> `ImGuiRenderDrawLists` 0x1409239e0 -> DrawPass 'ImGui' drawn **onto the window back buffer** (target 0xffffffff default), after the scene and HUD, just before flip. Input comes from `InputManager` action states (not WndProc). Window manager `ImGuiWindowManager` 0x140512bf0 / 0x140513c50 / 0x140513350 (docking, `ImwWindowManager::Update` 0x1407f4170).

Toggle: actions `ACTION_TOGGLE_MENU_DEBUG` (StringId 0x14139aa20), `ACTION_TOGGLE_PAD_MENU_DEBUG` (0x14139aa28), `ACTION_TOGGLE_PROFILER_WINDOW` (0x14139aa18), `ACTION_TOGGLE_STATS_WINDOW` (0x14139aa10) in `GameContextDebug::UpdateInput` 0x14048e5c0 -> `MenuDebug::SetEnable` 0x140767f20 (byte 0x141358540, forced 0 in `MenuDebug::Create` 0x1407645b0). Honoured only if `GameContextDebug+0x31` (Lua `Engine.AllowMenuDebug`, default false, loaded in `GameContextDebug::Create` 0x14048d8f0) and `+0x32` (default 1), or already enabled. 'Use ImGui' (0x141358541, default 1) selects ImGui vs the legacy Printf menu. While on, `InputManager+8` blocks game input. `imgui.ini` (49 bytes, `[Debug] Pos=60,60 Size=400,400`) already exists in the game folder; origin unknown (game's own ImGui or an earlier test).

**Mod rule:** our overlay uses its own ImGui context (`IniFilename = NULL`), no symbol clash, drawn on our own swapchain/overlay layer, never through the game's `RenderLayer`. If the game's ImGui is on, its 'ImGui' pass lands on the back buffer and would not appear in an eye image taken from the pre-HUD scene copy.

### 8.4 Movie, menu, loading, pause, replay states

All `runtime-check` unless marked. Read these each frame to build the VR-presentation state table.

| State | Where | Evidence | Confidence |
|---|---|---|---|
| Fullscreen movie | `Renderer+0xde8` = `MoviePlayer*` (`IsPlayingMovie` 0x1405af290), `Renderer+0xe61` fullscreen, `+0xe3a` paused, `+0xe3b` finished (cleared/released in `OnFlushRender`) | `Renderer::OnRender` draws **only** a 'Render Movieplayer' quad to the back buffer and **skips `RenderLoop::Render` for that frame** | confirmed (decompile) |
| Cutscene | `CutSceneManager` at `GameEngine+0x140`; `IsPlaying` 0x140459830 = `(+0xdd \|\| +0xde)` | `CreateCutSceneManager` 0x1404ae690 | confirmed |
| Loading | `LevelLoader::IsLoading` 0x1404bb020 (`+0x44 != 0 \|\| +0x58 != 0`); `Level::IsLoading` 0x1403d51c0 (`+0xfd`) | | confirmed |
| Paused | `GameContext+0x48` (time factor) == 0.0. `KTUGuiManager::PauseGame` 0x140a030c0 sets `KTUGuiManager+0xe8 = 1` and `GameContext::PushTimeFactor(0.0)` 0x14048bf10 (stack depth `+0x54`); pop 0x14048bed0; set 0x14048cb40 | | confirmed |
| Menu open | `KTUGuiManager+0xf0` current menu non-null (`GetCurrentMenu` 0x1404b3df0); `+0x361` block in-game menu; HUD hide counter `+0x8` (`ShowHud` 0x140a03720) | globals `g_pGameContext` 0x1414150a8 (also `GameContext::s_pInstance` 0x14139a590), `g_pGameEngine` 0x1414150b0, `g_pGuiManager` 0x1414153c8 | confirmed (fields); likely (semantics) |
| Replay | `TTReplayManager+0xa0`: 0 idle, 1 record, else replay (`FUN_140352a90` debug print, `FUN_140354f20` update). Instance probably `TTGameEngine+0x770` | | likely |
| Menu state machine | log lines `MenuState : PUSH TTxxxState` (`ttmenustatebase.cpp(161)`: `TTPressStartState`, `TTMainMenuState`, `TTOptionsState`, `TTReplayState`, `TTPauseScreen`) | game log | confirmed (log) |
| Active camera / view | `KTUGameEngine::GetCamera(i)` 0x140651200 -> player array (`+0x3b8`, stride 0xc0, count `+0x3b4`) `+0x18` = `KTUCamera`; mode stack `+0x78` (count `+0x74`, element 0 active), mode name at mode`+0x38`. TT camera-mode enum (`FUN_140056a70`): Fixed 0, Spring 1, Cockpit 2, Helicopter 3, PhotoCamera 4. `GameCameraViews` enum (`FUN_14002c180`): FollowFar 0, FollowNear 1, Helmet 2, HelmetNoMesh 3, Dash 4, Fork 5, LeftWheel 6, Saddle 7, Cockpit_01 8, Bubble 9, PitStop 10. The live "current view" field in `TTCameraModeMgr` was **not located** | | likely (layout); open (live field) |
| Menu 3D previews | `ExternalSceneRendering` path (section 5) | | likely |
| Riding (mod signal, Phase 3) | player-0 `KTUCamera` (`KTUGameEngine::GetCamera(0)` export on `g_pGameEngine`) active mode `KTUCamera+0x48` has vtable slot 6 (+0x30, `Manage`, called at 0x1409d9125) == `TTSpringCameraMode::Manage` 0x14019d700 (vftable 0x140f338f8, pattern signature) | disasm of `KTUCamera::Update` 0x1409d90d0..0x1409d9125, vftable dump | confirmed (static); runtime check pending |
| Menu flags at runtime | 2026-10-16 log: `IsGamePaused` true during the startup intros; `IsMenuOpen` and `IsGamePaused` both **false in the main menu** (TT2 menus run on the `TTxxxState` machine, not `KTUGuiManager+0xf0`) | `menu state:` log lines | confirmed (runtime) |

Frame-structure rule for a VR hook: **a frame with a fullscreen movie contains no 3D render at all** (no `DeferredRenderLoop::Render` call), so any per-eye render loop must tolerate "no scene this frame" and fall back to the theater quad.

---

## 9. GPU marker situation

- Engine pass names are `DrawPassDesc.name` strings (list in section 4 and below). `confirmed` that they exist as strings in the descs.
- An annotation path exists: `RenderLayer::AddProfilingEvent` 0x1408d3aa0 (and `AddProfilingEventImmediate` 0x1408d3b70) -> `RenderLayerDX11` vtable slot 20 (+0xa0) = `FUN_140948010`, which calls `ID3D11DeviceContext::End(timestamp query)` and then `ID3DUserDefinedAnnotation::BeginEvent` (vtable +0x18, wchar name) or `EndEvent` (+0x20). The annotation pointers are queried per context into `VideoDeviceDX11+0x480[ctx]` in `EnableVideoAdapter` (IID at 0x141032bc0). `confirmed` (decompile of the wrapper).
- **But** `RenderLayer::BeginProfilingEvent` 0x1408d3d40 / `EndProfilingEvent` 0x1408d6a90 have **no static callers**, `VideoDevice::BeginProfilingEvent`/`EndProfilingEvent` (VideoDevice vtable +0x78/+0x80, `RenderLayerDX11` slots 14-16 = `ret` stub 0x14010a470) are no-ops, `GetUserDefinedAnnotation` 0x1403d4740 has no callers, and the draw-pass executor `FUN_14094aab0` does not emit the pass name. So **markers may be absent from a RenderDoc capture of this release build** (`likely`).
- **Runtime (f8323): no GPU markers in the capture, confirmed** (the event list has no engine pass names; no `BeginEvent` groups). The decompile-based reading below is right. Passes were labelled by structure (section 4.1).
- Contradiction with the capture note, which tells the user to "check the Event Browser shows the engine's marker names (the engine emits ID3DUserDefinedAnnotation markers)". The decompile-based reading above is better supported; treat the capture note's statement as an assumption to test, not a fact.
- **Markers are absent (confirmed, f8323)**, so name the passes in the RenderDoc analysis by their structure: clears, RT formats and sizes, shader hashes, fullscreen-triangle draws, and the pass order of section 4. The 'DrawPass name' list gives the expected sequence to match against.

Complete DrawPassDesc name list (static, `confirmed` as strings): GBuffer: 'ZPrepass', 'Opaque Front', 'AlphaTest Front', 'Opaque', 'Alpha test', 'PKFX-Opaque', 'Clear GBuffer', 'Before Ocean Decal blend', 'Ocean', 'Gbuffer blend', 'Post-Decal blend', 'Decal blend', 'Decal AO blend', 'Motion Vectors', 'Motion Vectors front pass'. Lighting: 'Tile Min Max Depth CS', 'Clear Dispatch CS', 'Cull Light CS', 'Froxel Cull Light CS', 'Z Bin Light CS', 'Light Froxel CS', 'Volumetric accumulation CS', 'Main Lighting', 'Fog On Sky', 'Sum Lighting CS', 'SSR pass', 'IntelSSAOPrepareDepths', 'IntelSSAOPrepareNormal', 'IntelSSAOGenerate', 'IntelSSAOBlur'. Rest: 'Render Sky Deferred', 'Render Forward Opaque', 'RenderFog', 'Render Transparency', 'Downscale Linear Depth', 'LowRes Transparency 125', 'Transparency Low Res', 'Final Blend pass', 'HUD3D Renderer Frontpass', 'Generate last frame mips', 'Downscale', 'HDR_LuminanceInit', 'HDR_LuminanceDownscale', 'HDR_LuminanceFinal', 'HDR_FinalCompose', 'TAA resolve', 'Render MotionBlur', 'Lens flares occlusions query', 'Lens flares render', 'HUD2D Renderer', 'Dynamic resolution upscale', 'upscale_simple', 'Split border', 'ImGui', 'PostProcess Read'. String-xref functions: `RenderGBuffer` 0x140842040, `RenderLowResTransparency` 0x140843f20, `PrepareLightingDispatchCommand` 0x14083a010, `RenderLightDispatchCommand` 0x14083a840, `LightFroxel` 0x140839120, `CullFroxelLight` 0x140836300, `RenderTAA` 0x1408ddf70, `RenderMotionBlur` 0x1408dd5e0, `UpscaleDynamicResolution` 0x1408df750, `RenderFog` 0x1408dd330, `HudRenderer::Render` 0x14088b530, `SkyRenderer::Render` 0x140909c30, `ImGuiRenderDrawLists` 0x1409239e0.

---

## 10. To confirm with RenderDoc

Nothing in sections 1 to 9 is `confirmed` at runtime until the row is ticked. Capture 2 or 3 consecutive frames so temporal history is warm.

- [x] **Passes and markers** (done, f8323: no markers; order matched section 4 except FXAA / 3D HUD / upscale, see 4.2; table in 4.1). Do `ID3DUserDefinedAnnotation` markers appear at all? If not, label passes by structure. Compare the real event order with section 4 (GBuffer -> froxel -> lighting -> sky -> transparency -> HUD flag 8 -> last-frame copy -> tonemap -> FXAA -> TAA -> motion blur -> lens flare -> post -> HUD 0x14 -> upscale -> HUD 0x22 -> 'ImGui').
- [~] **Threading** (partly done, f8323: one Present (38673), 9 ExecuteCommandList/FinishCommandlist events, deferred lists per pass in 1.14; not done: `VideoDeviceDX11+0x488` context count, `RenderJobCount` = 1 experiment). Exactly one Present per frame. Count `ExecuteCommandList` calls on the immediate context (up to `RenderJobCount - 1`, default 14 with at least 4 deferred contexts) and the deferred contexts that recorded them. Setting `RenderJobCount` = 1 (write 0x14140d2a0) should leave zero `ExecuteCommandList`. Read `VideoDeviceDX11+0x488` for the real context count.
- [ ] **Loop and views.** `Renderer+0xd30` vtable (Deferred 0x140f7bfc8 vs Forward 0x140f919a0); `Renderer+0xc90` split mode; `Renderer+0xd18` view mask; `RenderTargetManager+8/+0xc`. Break on `ViewContext::InitFromCamera` 0x14091aeb0 and count calls and call sites per frame (expect 1 from `DoUpdate` in normal mode).
- [ ] **Immediate-mode flag.** `RenderLayer+0x268` / 0x1414074a0 at runtime (decides when per-view constant buffers must be captured).
- [~] **TAA** (done, f8323: bindings, formats B8G8R8A8_SRGB (not R8G8B8A8), 3 textures, copy at 36758; not done: A/B alternation across frames). 'TAA resolve' draw: bound SRVs (`DepthBuffer`, `FrameBuffer`, `LastFrameBuffer`, `MotionVectors`), format and size of 'TAA render target 0/1' (confirm 'RGBA' 0x41424752 = R8G8B8A8), A/B alternation across frames, output copied to the PP RT.
- [~] **Jitter** (one frame done, f8323: g_jitter, g_TAAParams, projectionOffset decoded; not done: 4 consecutive frames). `g_jitter`/`g_TAAParams` constant buffer over 4 consecutive frames (expect alternating (+-0.5/w, 0) and (0, +-0.5/h) with previous in zw); projection element [2][0]/[2][1] in `cbCameraStd` between frames.
- [~] **Last-frame data** (done, f8323: MV RT R16G16_FLOAT 20321..20633, Last Frame Depth R32F 35759, Last Frame Buffer RGBA16F 8 mips 35786..35957, TAA last-frame VP jitter-free; not done: `cbVSCameraLastFrameStd` content, front-pass MV). 'Motion Vectors' and 'Motion Vectors front pass': `cbVSCameraLastFrameStd` (VP + camera position) vs the previous frame's `cbCameraStd` unjittered VP; camera-relative convention. 'Generate last frame mips' only present when SSR or water drops are active; format 0x464c4834 = R16G16B16A16F, 8 mips.
- [~] **Exposure** (done, f8323: chain 36541 -> 36606 -> tonemap 36652, formats; not done: ping-pong across frames, first-frame flag, pre-exposure question). 'HDR_Luminance*' and the 1x1 RT ping-pong across frames; previous-luminance binding (slot 3 of `HDR_LuminanceFinal`); effect of the first-frame flag; whether pre-exposure is applied before the last-frame colour copy.
- [~] **Froxel** (partly, f8323: CS 32784 binds `LastFrameLightedFroxel_tex` (3D) as history; reprojection method and `cbCSLightData` not examined). `csLightFroxelTemporal` dispatch: slot 4 = previous volume; `cbCSLightData` for any previous-frame matrix; scrub camera motion to see if history is reprojected.
- [x] **Shadows** (done, f8323: 3 cascades = 3 deferred lists, D16 atlas `3736` with 2048x2048 viewport tiles, plus a D32 2048x2048 depth-only pass `3730`; draws are issued from deferred lists, atlas total size unread). Cascade count (log says 3 cascades of 2048) and the cascade draws (the engine's `RenderDirectionalCascadedShadowMap` is a stub).
- [~] **HUD** (partly, f8323: 2D HUD 37099..38606 into the back buffer after the final blit, depth test off; no 3D HUD pass seen, 4.2; per-state flags and race/pause/replay/menu not checked). Order and flags of `HudRenderer::Render` calls (log flags/RT/list at 0x14088b530 in race, pause menu, replay, main menu); target sizes; 2D HUD target.
- [ ] **ImGui.** With `Engine.AllowMenuDebug` set, 'ImGui' pass on the back buffer after 'HUD2D Renderer'.
- [ ] **Movies.** During a fullscreen movie, no `DeferredRenderLoop::Render` call.
- [x] **Conventions** (done, f8323: GEQUAL + write, main depth D32_FLOAT_S8X24, back buffer RTV R8G8B8A8_SRGB; clear value 0.0 not readable, implied only; recorded in `conventions.md`). (for `conventions.md`): depth clear value (0 vs 1) against depth function (reversed-Z), back-buffer format.
- [ ] **Split mode (separate experiment).** Force split mode 1 (`SetPossibleSplitMode` 0x1408e2080 then the split path or `Renderer+0xe40`), copy Camera[0] into Camera[1], check that two views render, that `OnSplitChanged` allocated both history slots, and which target each view writes.

### Capture procedure (summary)

> **Blocker (confirmed 2026-10-16):** RenderDoc cannot capture the unmodified game. `EnableVideoAdapter` passes `Flags = 0x80` (`D3D11_CREATE_DEVICE_PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY`) to `D3D11CreateDeviceAndSwapChain`; RenderDoc treats that flag as "application requested not to be hooked" (its log, `d3d11_hooks.cpp(163)`), skips wrapping the device while DXGI is already wrapped, and the call returns S_OK with a null immediate context. The game then crashes at `TT2.exe+0x9592dd` (`mov r9,[rcx]`, QueryInterface on context slot 0). Confirmed 2026-10-16 from 3 CrashRpt minidumps (access violation reading 0, rcx = 0, loop index 0, returned feature level 11_1) and the RenderDoc log; NVIDIA NvCamera and the Steam overlay were ruled out. Fix: our proxy hooks `D3D11CreateDeviceAndSwapChain` and clears bit 0x80 before the call goes on (debug-layer registry behaviour only; no gameplay effect). Until the Phase 1 proxy exists, every RenderDoc launch (Steam launch option or qrenderdoc) crashes during device creation. Disabling NVIDIA Ansel or the Steam overlay does not help.

Full steps, caveats and fallbacks are in the Phase 0 capture notes; the export script is `tools/renderdoc/rd_export_frame.py`.

1. **Prep.** `mkdir C:\dev\tt2vr-re\rdc`. Close the NVIDIA overlay, Steam overlay (or untick 'Enable the Steam Overlay while in-game'), Discord, RTSS/Afterburner. If the game crashes at start, disable Ansel in the NVIDIA app.
2. **Launch from process start** (late injection misses `D3D11CreateDeviceAndSwapChain` in `EnableVideoAdapter` 0x140958eb0). Preferred: Steam launch option `"C:\Program Files\RenderDoc\renderdoccmd.exe" capture --opt-capture-all-cmd-lists --opt-ref-all-resources --opt-hook-children -c C:\dev\tt2vr-re\rdc\tt2 -w %command%`; no `--opt-api-validation`. `--opt-capture-all-cmd-lists` matters because of the deferred contexts of section 1. Fallback: qrenderdoc File > Launch Application with the same options (Steam running, do not add `steam_appid.txt`). Revert the launch option afterwards.
3. **In-game setup.** TAA mode on (FXAA_TAA or TAA; record the exact AA option list), dynamic resolution off, record resolution and window mode (windowed or borderless is more stable).
4. **Capture** (F12 / PrintScreen) after several seconds of running: (a) in-race, helmet camera, daytime, Snaefell Mountain Course, bike moving; (b) same place, bike stationary; (c) main menu. 2 or 3 consecutive frames each.
5. **Export.** Open the `.rdc` in qrenderdoc, wait for load to finish, Tools > Start Python Shell > Run Scripts > `C:\dev\tt2vr\tools\renderdoc\rd_export_frame.py`. Output next to the capture: `<capture>.actions.tsv` (one row per action) and `<capture>.passes.md` (pass summary, Clear events, ExecuteCommandList/deferred events, unique shaders with xxh64 / sha1 / DXBC-header MD5, ERRORS section). Smoke test first with `RD_MAX_EVENTS = 300`. The script was written against the RenderDoc 1.37 API and has **never been run**; `PLAN.md` records RenderDoc upgraded to 1.46, so API names may need fixing (check the ERRORS section). `ShaderReflection.rawBytes` being the raw DXBC blob is assumed, not confirmed.
6. **Analysis path.** Captures are analysed by Claude through the RenderDoc MCP server (LifeSugar/RenderDoc_MCP @359e4e3, hardened locally: no program launch, captures limited to `C:\dev\tt2vr-re\rdc`, registered with Claude Code per `PLAN.md`). Captures never go into git.
7. **Open:** does `SteamAPI_Init` succeed when `TT2.exe` is launched outside Steam under qrenderdoc? (`SteamAPI_RestartAppIfNecessary` is absent from the binary; steam_api64 is resolved dynamically in `FUN_140bda120`.) Steam launch avoids the question.

---

## 11. Contradictions and conflicts between source notes

| Topic | Notes disagree | Better-supported reading |
|---|---|---|
| Event markers in RenderDoc | Capture note: "engine emits ID3DUserDefinedAnnotation markers". Render-loop note: Begin/EndProfilingEvent have no callers, executor emits no name, markers may be absent. | Render-loop note (decompile evidence). Treat markers as unconfirmed. |
| Camera FOV axis | Camera note: `Camera+0x78` is vertical FOV in radians. Projection note: `MatrixPerspectiveFovRH` 0x140813390 uses it as **horizontal** (`m[5] = m[0] * aspect`, so `m[0] = 1/tan(fov/2)` is the horizontal term); `KTUCamera::Update` 0x1409d9060 applies a Hor+ correction beyond 16:9. | Horizontal (projection note, from the matrix algebra). CVar defaults 1.25 rad H / 0.75 rad V are consistent at 16:9. Details in `camera.md` / `conventions.md`. |
| Exposure state | Render-loop note: tone mapper is ONE object with no camera-index key, so two views double-step adaptation. Temporal note (later, more detailed decompile): adaptation history IS per camera index (`tm+0x138+idx*8`, `tm+0x14e+idx`, params `tm+0x30+idx*0x60`); only the CPU readback float `tm+0x148` is a single shared value. | Temporal note. Each slot adapts independently (no double-step), but the eyes can drift apart, which breaks the shared-exposure rule. See `temporal.md`. |
| `OnUpdate` address | Threading note: `RendererWin32::OnUpdate` 0x140709070 -> `DoUpdate`. Render-loop and HUD notes list slot 11 `OnUpdate` as 0x1405b0040. | Both are plausible (`0x1405b0040` may be a base or thunk, `0x140709070` the override). Resolve by reading the vtable at 0x140f7bdb0 slot 11 in Ghidra. Call-site 0x14048c536 (vtbl +0x58) is the stable anchor. |
| Jitter argument position | Temporal note: Vector2 is the "3rd argument" of `InitFromCamera`; render-loop note: "4th argument". | Same parameter, counted with or without `this`: `InitFromCamera(ViewContext*, Camera*, Viewport&, Vector2&)` 0x14091aeb0. |
| Stereo flag `+0x466` effect | Render-loop note: reading it clears a lighting-path flag (`local_70f`). HUD note: it disables lens flares and still renders the HUD. | Unresolved; same check described by two notes. Needs a re-read of the condition in `DeferredRenderLoop::Render`. Only matters if we ever set `+0x466`, which we do not plan to. |
| Last-frame colour/depth timing | Temporal note: copy happens before tonemap, "after transparency/HUD pass" (so after HUD flag 8). Render-loop skeleton order shows 'Generate last frame mips' after the flag-8 HUD, before `WaterDrop::Render`. | Consistent: the copy includes 3D HUD pixels. |
| Probes and shadows scope | Render-loop note flags CSM per-view fitting as `guess`; temporal note says CSM/static shadow caches are per camera index. `PLAN.md` wants one set of cascades fitted to the combined frustum. | Open. The engine passes the whole view array to `SetupRenderContext` but keeps per-idx contexts; see `temporal.md`. |
| FXAA in the AA mode FXAA_TAA | Static step 23: `RenderFXAA` for modes 1 and 2 before TAA | Capture f8323 (AA = FXAA_TAA): no FXAA pass between tonemap 36652 and TAA 36731 | Open (4.2 item 1). |
| 3D HUD passes | Static steps 19 and 31: 3D HUD before post (flag 8) and after post (flag 0x14) | Capture f8323: none identified; 2D HUD only, after the final blit | Open (4.2 item 2); helmet view only. |
| Dynamic-res upscale pass | Static step 33 | Capture f8323 (dyn-res off): 1:1 final blit 36985, no upscale | Open (4.2 item 3), likely a scale-below-1 only pass. |
| Viewport depth range | `conventions.md` 3.4 (static): MaxDepth `1.0 - RenderLoop[0x1118]` with default 0 | Capture f8323: world draws MaxDepth 0.95 | Runtime wins; see `conventions.md` 3.7. |
