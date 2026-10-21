# Symbols: our names <-> addresses (build 4989319)

Source of truth for names. Read this before any analysis; add every new name here. All addresses refer to the exe in `docs/re/README.md` (Steam build 4989319, SHA-256 `103d4b2e...a601a`, image base 0x140000000). Offsets written `+0x..` are byte offsets inside the named struct.

## Reading rules

- **Confidence** = `confirmed` / `likely` / `guess`, as given by the investigation notes. Suffix **(RC)** means the finding is static-only and the notes say it needs a runtime check (debugger, RenderDoc, log) before you build on it. Never hook or patch on a `guess`, or on an (RC) item, without that check (CLAUDE.md).
- **Kind**: function, global (data in .data/.bss), field (struct offset), vtable, vslot (vtable slot), const (call site / instruction address), string.
- **Our name** column: if the engine exports a real name (`PlayAll::...`, demangled by Ghidra) we use it unchanged. Unexported code is `FUN_<addr>` or a name we invented (marked in notes as `ours`).
- **Identical-code-folding caveat.** The exe was linked with `/OPT:ICF`: one address can carry several unrelated export names. A name at an address means "one of the folded bodies", never proof of the caller's intent. Known aliases:

| Address | Folded names | Consequence |
|---|---|---|
| 0x1403ff1d0 | `VideoDevice::SetRenderThreadID`, `SystemInfo::SetVRamMo` | field write `VideoDevice+0x88` |
| 0x1403fc6d0 | `VideoDevice::GetRenderThreadID` (+ folded getter) | `mov eax,[rcx+0x88]` |
| 0x14048caf0 | `Camera::SetFov`, `SetGraduationMinY...` | stores float at `Camera+0x78` |
| 0x14011def0 | `Camera::GetFov`, `Animation::GetDuration` | reads `Camera+0x78` |
| 0x140501650 | `Camera::SetFrontFov`, `Physics::Engine::SetDebugVisualizationScale` | stores `Camera+0x8c` |
| 0x14012e630 | `Camera::GetFrontFov`, `MeshPainter::GetDecalScale` | reads `Camera+0x8c` |
| 0x1401ad120 | `GetBitDataSize` (enki thread count getter) | `mov eax,[rcx+0x14]` |
| 0x14010a470 | shared `ret 0` stub, also `ValueTracker::AddVar<int>` | RendererWin32 slot 17 (+0x88), RenderLayerDX11 slots 18/19, VideoDeviceDX11 slots 14-16 are no-ops |

- **TT2 game classes are not exported.** `TTSpringCameraMode`, `TTGameCameraViews`, `TTCameraModeMgr`, `TTOptionsManager`, `TTHud`, `TTDashboardHUD`, `TTReplayManager` etc. have 0 export hits. Only `PlayAll::*` and global `KTU*` classes are exported. TT2 code is located by string xrefs from `__FUNCTION__` and log literals; those entries below are `FUN_` addresses with our names.
- 27 `__FILE__` path strings carry a 1-byte `?` prefix in the string dump; their true VA is dump VA + 1 (e.g. renderloop.cpp @0x140ffef00).
- CLAUDE.md rule 3: the mod must not hard-code these addresses. They are the analysis reference; `src/game/signatures.hpp` finds them by export name first, pattern scan as fallback.
- Thread of the whole frame: main thread (see Frame loop). "Per frame / per view" classification lives in `temporal.md` / `frame-anatomy.md`; this file only names things.
- Contradictions between notes are collected in the last section ("Conflicts and open questions") and flagged inline with **CONFLICT**.

---

## 1. Frame loop and threading

Verdict (confirmed): no dedicated render thread. The main thread runs simulation, view building, pass recording, D3D11 submission and Present inside `GameContext::RenderOneFrame`, once per frame. Deferred contexts exist but are used only as parallel command-list recorders inside pass execution.

| Our name | Address | Kind | Notes | Confidence |
|---|---|---|---|---|
| `FUN_140159740` WinMainLike_RunGame | 0x140159740 | function | KTUGameEngine::PreRun("TT Isle of Man 2") -> Run -> PostRun; called from CRT FUN_140e523a4 | likely |
| `KTUGameEngine::Run` | 0x1409faf50 | function | calls GameContext::Run (call 0x1409fc1ff) | confirmed |
| `KTUGameEngine::PreRun` | 0x1409f7860 | function | installs CrashRpt via FUN_1409f7550 (call 0x1409f7d29) | confirmed |
| `PlayAll::Game::GameContext::Run` | 0x14048c9a0 | function | loop: `while(!exit && RenderOneFrame()==0)`; logs "GameContext Run Loop" (gamecontext.cpp 543) | confirmed |
| `GameContext::RenderOneFrame` | 0x14048bf80 | function | real per-frame body: update, record, flush, flip. Calls Renderer vtable directly | confirmed |
| RenderOneFrame: Clock::FrameTick | 0x14048bfe4 | const | call site | confirmed |
| RenderOneFrame: InputManager::UpdateDriver | 0x14048c17c | const | call site | confirmed |
| RenderOneFrame: ImGuiNewFrame call | 0x14048c2a7 | const | every frame (see HUD/UI) | confirmed |
| RenderOneFrame: BaseObject::Process(0) | 0x14048c344 | const | runs GameEngine::OnManage then World::OnManage then Process(1) (0x14048c37e) | confirmed |
| RenderOneFrame: World::OnManage / OnPostManage | 0x14048c362 / 0x14048c39c | const | call sites | confirmed |
| RenderOneFrame: physics scene fetch (vtbl+0x130) | 0x14048c3e0 | const | under PhysicsSceneLock | confirmed |
| RenderOneFrame: StartProcessAnim / WaitProcessAnim | 0x14048c3fb / 0x14048c4c9 | const | animation jobs | confirmed |
| RenderOneFrame: HHParticlesManager::SetViewContext | 0x14048c44a | const | fed `Renderer::GetViewContext(i)` BEFORE OnUpdate rebuilds them (PopcornFX may see previous-frame view) | guess |
| RenderOneFrame: UpdateScenesHierarchy | 0x14048c520 | const | call site | confirmed |
| RenderOneFrame: Renderer::OnUpdate call | 0x14048c536 | const | `call [rax+0x58]` -> RendererWin32::OnUpdate 0x140709070 -> Renderer::DoUpdate. Best "views are built" breakpoint | confirmed |
| RenderOneFrame: World::OnPostUpdate | 0x14048c559 | const | call site | confirmed |
| RenderOneFrame: SceneManager::PreRenderScenes | 0x14048c5ac | const | call site | confirmed |
| RenderOneFrame: Renderer::OnPreRender call | 0x14048c5c2 | const | `call [rax+0x38]` | confirmed |
| RenderOneFrame: StartSimulateByConstantStep | 0x14048c5f2 | const | PhysX step runs async during render | confirmed |
| RenderOneFrame: Renderer::OnRender call | 0x14048c6fe | const | `call [rax+0x40]` | confirmed |
| RenderOneFrame: ImGui::Render call | 0x14048c796 | const | between OnRender and OnFlushRender | confirmed |
| RenderOneFrame: ImGuiWindowManager::PostUpdate | 0x14048c7bd | const | call site | confirmed |
| RenderOneFrame: Renderer::OnFlushRender call | 0x14048c7d0 | const | `call [rax+0x48]`; executes the recorded passes | confirmed |
| RenderOneFrame: WaitSimulateByConstantStep | 0x14048c7e4 | const | joins physics | confirmed |
| RenderOneFrame: DeferredCallback::Call | 0x14048c7f9 | const | call site | confirmed |
| RenderOneFrame: Renderer::OnFlip call | 0x14048c84f | const | `call [rax+0x50]`; EndRender + Present + PostFlip | confirmed |
| `BaseObject::Process` | 0x140407eb0 | function | event 0 -> vtbl+0x38 (GameEngine::OnManage), 1 -> +0x40 slot 8 (CameraManager::OnPostManage), 3 -> slot 10; children first | confirmed |
| `PlayAll::Game::GameEngine::OnManage` | 0x1404c0670 | function | GameEngine vftable slot 7; per-frame engine update | confirmed |
| `Renderer vftable` (base) | 0x140f65120 | vtable | slots 7 OnPreRender 0x1405af410, 8 OnRender 0x1405af470, 9 OnFlushRender 0x1405af360, 10 OnFlip 0x1405af2d0, 11 OnUpdate 0x1405b0040 (+0x58), 13 SetCameraDesc (+0x68), 17 (+0x88) no-op | confirmed |
| `RendererWin32 vftable` | 0x140f7bdb0 / 0x140f7bda8 | vtable | two addresses given by two notes (8 bytes apart, probably RTTI-locator vs first slot). Slots 7-11 as above | likely (**CONFLICT**: address) |
| `Renderer::OnUpdate` thunk (RendererWin32) | 0x140709070 | function | forwards to DoUpdate | confirmed |
| `Renderer::OnPreRender` | 0x1405af410 | function | -> RenderLoop::PreRender | confirmed |
| `PlayAll::Game::Renderer::OnRender` | 0x1405af470 | function | once-per-frame work then loop Render; 'Could not render frame, BeginRender failed or ViewContext is invalid.' | confirmed |
| `Renderer::OnFlushRender` | 0x1405af360 | function | executes passes; clears movie-finished flag | confirmed |
| `Renderer::OnFlip` | 0x1405af2d0 | function | EndRender (RenderLayer vtbl+0x40), Flip (vtbl+0x50), GPU timer spin read | confirmed |
| `Renderer::RenderFlipEmptyFrame` | 0x1405b02b0 | function | resize path; also from OnBeginUnload | confirmed |
| `Renderer::DoUpdate` | 0x1405addd0 | function | builds ViewContexts from cameras, computes TAA jitter (see Camera/Temporal) | confirmed |
| `GameEngine::s_pInstance` | 0x14139aa30 | global | GameEngine singleton | confirmed |
| `g_pGameEngine` | 0x1414150b0 | global | exported data symbol (KTUGameEngine*) | confirmed |
| `g_pGameContext` | 0x1414150a8 | global | exported (KTUGameContext*); also `GameContext::s_pInstance` 0x14139a590 | confirmed |
| `g_pGuiManager` | 0x1414153c8 | global | KTUGuiManager* | confirmed |
| `VideoDevice::Create` | 0x140918590 | function | writes `VideoDevice+0x88 = GetCurrentThreadId()` (render-thread id = device-creating main thread) | confirmed |
| `VideoDevice::GetRenderThreadID` | 0x1403fc6d0 | function | ICF-folded; no direct callers | confirmed |
| `VideoDevice::SetRenderThreadID` | 0x1403ff1d0 | function | ICF-folded with `SystemInfo::SetVRamMo`; no direct callers | confirmed |
| `RenderLayer::DrawCommandBuffer` | 0x1408d6a10 | function | RECORDS the pass into a global queue unless RenderLayer+0x268 != 0 | confirmed |
| `RenderLayer::ExecuteRenderPassRange` | 0x1408d6ad0 | function | switch on pass type 0 draw, 1 dispatch, 2/3 fence, 4 copy, 5 profiling event; vtbl +0x80/+0x88/+0x90/+0x98/+0xc0/+0xa0 | confirmed |
| `g_RenderPassQueues` | 0x141408300 | global | 7 per-queue lists, 16 bytes each (ptr, ushort count at +10) | confirmed |
| pass pools | 0x1414082c0 / 0x1414082c8 / 0x1414082d0 / 0x1414082d8 / 0x1414082e0 | global | DrawPass (0x990 B), compute (0x3d0), fence, copy (0x28), unnamed | confirmed |
| `RenderPass::FrameFlip` | 0x1408e09e0 | function | zeroes all queue counts; called at end of ExecuteRenderPasses (0x14094b77f) | confirmed |
| `FUN_1408e0a40` | 0x1408e0a40 | function | appends pass index to a queue list | confirmed |
| `RenderLayer::s_oUseImmediateRenderPass` CVar | 0x141407420 | global | 'Engine.Renderer.UseImmediateRenderPass'; value byte 0x1414074a0 (default 0), copied into RenderLayer+0x268 in Flip | confirmed |
| `CVar_RenderJobCount` | 0x14140d220 | global | 'Engine.Renderer.RenderJobCount'; int value at 0x14140d2a0 (default 15, min 1, max 20); registered by FUN_1400ab7d0 | confirmed |
| `FUN_14009ef30` | 0x14009ef30 | function | registers the UseImmediateRenderPass ICVar | confirmed |
| `DoubleBuffer::Alloc` | 0x14084a600 | function | per-frame thread-safe linear allocator ("DoubleBuffer" name is misleading) | confirmed |
| `DoubleBuffer::Create` | 0x14084a650 | function | size from Lua 'Engine.DoubleBuffer'.Size | confirmed |
| `DoubleBuffer::SwapBuffer` | 0x14084a880 | function | jmp FUN_14081bbd0: atomic offset=0 reset. No second half, no CPU frame lag | confirmed |
| `DoubleBuffer reset` | 0x14081bbd0 | function | ThreadSafeLinearAllocator reset | confirmed |
| `DoubleBuffer alloc impl` | 0x14081bab0 | function | per-thread-slot bump alloc, 64 KB chunk refill | confirmed |
| `GameEngine::CreateDoubleBuffer` | 0x1404aea10 | function | slots = enki thread count | confirmed |
| `RenderLayer::EndRender` | 0x1408d6aa0 | function | `(vtbl+0x48)(this,0)` execute, then DoubleBuffer::SwapBuffer, GraphicsObjectPool::EndFrame | confirmed |
| `RenderLayer::BeginRender` | 0x1408d3d50 | function | increments render id (+0x30) | confirmed |
| `RenderLayer::BeginProfilingEvent` / `EndProfilingEvent` / `AddProfilingEvent` | 0x1408d3d40 / 0x1408d6a90 / 0x1408d3aa0 | function | exported; (`SimpleGPUPerformanceQuery*` {+0 ID3D11Query*, +0xc queue}, name or null): queue a kind-5 pass, payload `{q, name}` at `DAT_1414082e0[DAT_1414082f8]` (count reset in `RenderPass::FrameFlip`); never called by the engine | confirmed |
| `RenderLayer::Flip` | 0x1408d7410 | function | increments flip id (+0x34); copies UseImmediateRenderPass into +0x268; tail-calls VideoDevice vtbl+0x88 = PostFlip | confirmed |
| `RenderLayer::GetFrameID` | 0x1403d3df0 | function | returns +0x30 (render id) | confirmed |
| `RenderLayer::GetContextId` | 0x1408d75c0 | function | queue 0 -> enki GetThreadIndex()\|0x80; queues 1..6 -> (q-1)\|0x40 | confirmed |
| `RenderLayer ctor` | 0x1408d37f0 | function | sets +0xb8=0x101, +0xba=0 (async compute off) | confirmed |
| `RenderLayer::IsDispatchAsync` | 0x1408d7d50 | function | requires +0xb9 && +0xba; false on DX11 | likely |
| `RenderLayer::Dispatch` | 0x1408d6960 | function | only xref of string 'Wait graphics queue' (GPU fence label) | confirmed |
| `RenderInstanceManager::CopyToMainThread` | 0x1408d3290 | function | enki job: merges per-thread render-instance slots 1..n into slot 0, sorts | confirmed |
| `RenderLoop::CopyRenderInstances` | (callee of DeferredRenderLoop::Render) | function | 1 job per list, priority 1, then WaitforTaskSet | confirmed |
| `enki::TaskScheduler::AddTaskSetToPipe` | 0x140792a80 | function | many callers (culling, spawners, scene update, physics, audio, render jobs) | confirmed |
| `enki::TaskScheduler::WaitforTaskSet` | 0x140793920 | function | main thread only helps with priority-1 work, so it does not steal priority-0 render jobs | likely |
| `enki::TaskScheduler::GetThreadIndex` | 0x140792d60 | function | TLS+0xc070; 0 on main, 1..N-1 workers | confirmed |
| `enki::TaskScheduler::StartThreads` | 0x1407930b0 | function | CreateThread(TaskingThreadFunction 0x140793300) + SetThreadName('enkiThread'); inline call at 0x1407931ef | confirmed |
| `enki TaskingThreadFunction` | 0x140793300 | function | thread start address of 'enkiThread' workers (census: TT2+0x793300) | confirmed |
| `CoreModule::Initialize` | 0x1407415e0 | function | enki Initialize at 0x140742302 with numThreads = PlayAllThreadConfig+0xd8 + 1; also gates AntiCheat (Platform) | confirmed |
| `g_PlayAllThreadConfig` | 0x1413c3f10 | global | via GetPlayAllThreadConfig 0x14080a350; +0xd8 enki workers (default 4), +0xdc core mask 0x3a, +0xe0 0xffffffff | confirmed |
| `Core::Thread::CreateThread` | (call _beginthreadex 0x140809497) | function | named threads: AsyncLoader, AsyncDispatcher, WorldStreamerThread, ReadThread, InputsThread, OnlineServicesThread | confirmed |
| `Core::Thread::SetThreadName` | 0x140809950 | function | RaiseException(0x406D1388), visible only to an attached debugger | confirmed |
| Core::Thread trampoline | 0x8099a0 (RVA) | const | start address of named Core::Thread threads (TT2+0x8099a0) | likely |
| `AsyncLoader::Startup` | 0x1403ff740 | function | 'AsyncLoader' + 'AsyncDispatcher' threads | confirmed |
| `WorldStreamer::CreateThread` | 0x140684ec0 | function | 'WorldStreamerThread' | confirmed |
| `FUN_140780490` | 0x140780490 | function | 'ReadThread' (streamfile.cpp) | confirmed |
| `FUN_140bd5bc0` | 0x140bd5bc0 | function | 'OnlineServicesThread' | confirmed |
| `FanatecFSThreadBase::Start` | 0x140be5270 | function | wheel SDK threads (callers FUN_140beacc0 / FUN_140beb0b0 / FUN_140be6110) | confirmed |
| `PopcornFX_CThread::Start` | 0x140bf7400 | function | hh_kernel; worker pool FUN_140bfc420; 'File watcher' FUN_140bf4e30 | likely |
| `libvpx_vp8_create_decoder_threads` | 0x1400f8b70 | function | movie playback | confirmed |
| `RakNet_RakThread::Create` | 0x140a97aa0 | function | 2 MB stack, rakpeer.cpp | likely |
| `FUN_1409b9090` | 0x1409b9090 | function | PopcornFX thread pool wrapper (sized from enki count); pool job FUN_1409b9590 | confirmed |
| `Renderer GPU timer read (spin)` | 0x1408651f0 | function | OnFlip: LockImmediateContext + GetData loop while S_FALSE for previous frame's slot -> CPU runs at most ~1 frame ahead | confirmed |
| `Renderer GPU timer begin/end` | 0x140865330 / 0x140865160 | function | begin disjoint+timestamp (OnRender) / end (OnFlushRender); ring of 4 | confirmed |
| `PhysicsManager::StartSimulateByConstantStep` | (call 0x14048c5f2) | function | only work overlapping render; frame N renders state using N-1's physics result | likely |
| `GameEngine::UpdateShift` | 0x1404d5630 | function | floating-origin 2048-unit shift (65 per two sessions in the log) | confirmed |
| string 'World Shift (%f\t%f)' | 0x140f7c6a0 | string | emitted by UpdateShift; also 'Apply shift' 0x140f7c690, 'PlayAll::Game::GameEngine::UpdateShift' 0x140f7c6b8 | confirmed |
| string 'Wait graphics queue' | 0x140ffe770 | string | GPU fence label only | confirmed |
| string 'Engine.DoubleBuffer' / 'Not enough space in double buffer' | 0x140fdcbf0 / 0x140fdcc20 | string | | confirmed |
| string 'enkiThread' / 'Thread %d' | 0x140fc1830 / 0x140fc1920 | string | | confirmed |
| string 'PxWorker%02d' / 'PopcornFX thread pool' | 0x14105bc48 / 0x1410420f0 | string | | confirmed |
| string 'AsyncLoader' / 'AsyncDispatcher' / 'Thread Loader: Abort...' | 0x140f682a8 / 0x140f682b8 / 0x140f68680 | string | | confirmed |

### 1a. Log thread inventory (runtime facts, session 2026-10-15 13:04)

| Our name | Address | Kind | Notes | Confidence |
|---|---|---|---|---|
| main thread tid 0x3364 | (runtime) | const | 7451/8144 lines; init, device create, 'GameContext Run Loop', World Shift, PostFlip, shutdown. TIDs change per run | confirmed |
| loader worker tid 0x1de8 | (runtime) | const | 521 lines: levelserializer, timelogger, indexbuffer.cpp 'IndexBuffer Format is not 16bits' x78 (creates GPU buffers off-main), fontresourceloader | confirmed |
| WorldStreamer tid 0x32ec | (runtime) | const | worldstreamer.cpp(2679) 'Loading 80 Terrain Occluders' | confirmed |
| online tid 0x2964 | (runtime) | const | leaderboardbase / leaderboardsteam | confirmed |
| async loader tid 0x5d30 | (runtime) | const | 1 line: asyncloader.cpp(1812) 'Thread Loader: Abort...' | confirmed |
| thread role assignment above | | | role names are interpretation of the file names each thread logs | likely |

### 1b. Renderer fields (Renderer = GameEngine+0xb0, class RendererWin32)

| Our name | Offset | Kind | Notes | Confidence |
|---|---|---|---|---|
| Renderer.ansel_sessionActive | +0x19 | field | gates GetAnselProjectionOffset in DoUpdate; set by StartAnselSession | confirmed |
| Renderer.ansel_flag2 | +0x1a | field | set to 1 by StartAnselSession; purpose unknown | guess |
| Renderer.savedMotionBlur / savedDynRes | +0x1b / +0x1c | field | restored by StopAnselSession | confirmed |
| Renderer.renderDesc[0] | +0x30 | field | RenderDesc, ViewContext at desc+0 | confirmed |
| Renderer.renderDesc[1] | +0x650 | field | stride 0x620 | confirmed |
| Renderer.jitterGate? | +0x640 | field | must be 0 for jitter | likely (RC) |
| Renderer.view1Type | +0xc60 | field | = ViewContext[1]+0x610 (view-type int; 0 = ordinary, non-zero only on the external-scene path). Jitter gate `[+0x640]==0 && ([+0xc90]==0 \|\| [+0xc60]==0)` (disasm 0x1405adfc6); +0x640 = ViewContext[0]+0x610. Not RenderDesc+0xc60 | confirmed |
| Renderer.splitMode | +0xc90 | field | = RenderDesc+0xc60; 0 single, 1 vertical, 2 horizontal, 3/4 other | confirmed |
| Renderer.viewMask | +0xd18 | field | = RenderDesc+0xce8; bit i set -> ViewContext[i] active | confirmed |
| Renderer.shiftInfo | +0xd1c..+0xd24, flag +0xd28 | field | World Shift vec3 + flag = RenderDesc (Renderer+0x30) +0xcec/+0xcf8, the same bytes (no copy). Written only by `UpdateShift`: flag cleared at the start of every call, set with the vector after a shift (one-frame pulse while UpdateShift runs every frame). Positions -= vector (grid cell * 2048) | confirmed (2026-10-21) |
| Renderer.activeRenderLoop | +0xd30 | field | via FUN_140528660 | confirmed |
| Renderer.deferredLoop / forwardLoop | +0xd38 / +0xd40 | field | chosen by RendererDesc+0x90 (0 deferred, 1 forward). Deferred is almost certainly active | likely (RC) |
| Renderer.window | +0xd90 | field | Window*; swapchain at Window+0x48, HWND at *(+0xd90)+0x40, viewport source +0x30 | confirmed |
| Renderer.moviePlayer | +0xde8 | field | IsPlayingMovie 0x1405af290 | confirmed |
| Renderer.hudItemList[] | +0xdf8 (+idx*8) | field | per-camera HudItemList; GetHudItemList 0x1403de850 | confirmed |
| Renderer.externalScenes enable / count / list / RT | +0xe08 / +0xe14 / +0xe18 / +0xe20 | field | max 4 entries; overrides cameras/viewports of views 0/1 | likely (RC) |
| Renderer.resizeFlag | +0xe39 | field | triggers ChangeRenderTargetsSize + OnSplitChanged | confirmed |
| Renderer.moviePaused / movieFinished | +0xe3a / +0xe3b | field | | confirmed |
| Renderer.cameraDesc.splitMode | +0xe40 | field | from SetCameraDesc | confirmed |
| Renderer.splitCameras[2] | +0xe48 / +0xe50 | field | Camera* per split view; inputs to InitFromCamera in DoUpdate | confirmed |
| Renderer.cameraDesc.mask | +0xe58 | field | single-camera index = ctz(mask) | confirmed |
| Renderer.fullscreenMovie | +0xe61 | field | when set with movie playing OnRender skips the 3D scene | confirmed |
| Renderer.frameCounter | +0xec8 | field | used modulo 4 for the GPU timer ring | confirmed |
| Renderer.gpuTimerRing | (+0x1cd + ...)*8, +0xf28 | field | 4-slot timestamp/disjoint query ring | likely |
| Renderer.gpuFrameTimeSeconds | +0xf28 | field | previous frame's GPU time (s), written by `FUN_1408651f0` in OnFlip from slot `(ec8-1)&3`; ring slot s = queries at `Renderer+(s*3+0x1cd)*8` {ts start, ts end, disjoint} (renderer.md 7) | confirmed |
| `Renderer::GpuTimerBegin` (FUN_140865330) / `GpuTimerEnd` (FUN_140865160) / `GpuTimerRead` (FUN_1408651f0) | 0x140865330 / 0x140865160 / 0x1408651f0 | function | ours; calls 0x1405af5fc (OnRender: Begin disjoint, End ts0), 0x1405af3c7 (OnFlushRender: End ts1, End disjoint), 0x1405af33d (OnFlip: spin GetData, write +0xf28) | confirmed |
| `Renderer::GetViewContext(ushort)` | 0x1405af270 | function | `this + idx*0x620 + 0x30` | confirmed |
| `Renderer::GetCameraCount` | 0x1405aeef0 | function | 2 for split modes | confirmed |
| `Renderer::SetCameraDesc` | 0x1405b03b0 | function | CameraDesc {+0 split, +8 Camera*, +0x10 Camera*, +0x18 mask} -> Renderer+0xe40..0xe58 | confirmed |
| `Renderer::RegisterExternalSceneRenderer` | 0x1405b01e0 | function | no code callers (export only) | confirmed |
| `Renderer::Create` | 0x1405ace80 | function | creates loops, calls ansel::setConfiguration (call 0x1405adba0) | confirmed |
| `GameEngine::CreateRenderer` | 0x1404b0b60 | function | logs 'Creating renderer' (log line 72) | confirmed |

### 1c. GameEngine fields

| Our name | Offset | Kind | Notes | Confidence |
|---|---|---|---|---|
| GameEngine.anselCam | +0x60 | field | AnselCam object (0x68 bytes); +0x68 = gameplay camera saved during a session | confirmed |
| GameEngine.renderer | +0xb0 | field | Renderer* | confirmed |
| GameEngine.cutSceneManager | +0x140 | field | IsPlaying = +0xdd or +0xde | confirmed |
| GameEngine.splitMode | +0x178 | field | 0 off, 1 vertical (side by side), 2 horizontal; splitOn byte +0x184 | confirmed |
| GameEngine.pauseFlag | +0x185 | field | set by PauseApplication 0x1403d5930 | confirmed |
| GameEngine.cameraManager | +0x1f0 | field | CameraManager* | confirmed |
| GameEngine.shiftGrid | +0x84..+0x8c | field | 2048 grid used by UpdateShift | confirmed |
| GameEngine vslot 7 (+0x38) | 0x1404c0670 | vslot | OnManage | confirmed |
| GameEngine vslot 47 (+0x178) | 0x140f639a0 -> 0x1404d2a20 | vslot | SaveUserSettings | confirmed |
| GameEngine vslot 147 (+0x498) | 0x1404b9a70 | vslot | GetUserSettingsPath | confirmed |
| GameEngine vslot (+0x398) | 0x1403d30e0 | vslot | GetName ("TT Isle of Man 2") | confirmed |
| GameEngine vslot (+0x148) | | vslot | PauseApplication(bool) | confirmed |
| GameEngine vslot (+0x458) / (+0x460) | 0x1404d4dd0 / (n/a) | vslot | StartAnselSession / StopAnselSession | confirmed |
| GameEngine vslot (+0x570) | | vslot | returns KTUCamera by index | likely |

---

## 2. Device, swapchain, RenderLayer

D3D11 device created with `D3D11CreateDeviceAndSwapChain` (static import, the only d3d11 export used).

| Our name | Address | Kind | Notes | Confidence |
|---|---|---|---|---|
| `VideoDeviceDX11Windows::EnableVideoAdapter` | 0x140958eb0 | function | calls CreateDeviceAndSwapChain at 0x140959180, retry 0x1409591e9; creates deferred contexts | confirmed |
| `D3D11CreateDeviceAndSwapChain` call #1 / #2 | 0x140959180 / 0x1409591e9 | const | Flags 0x80 (PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY, NOT single-threaded); FL 11_1 (0xb100), retry 11_0 (0xb000) on E_INVALIDARG 0x80070057 or DXGI_ERROR_UNSUPPORTED 0x887A0004; SDKVersion 7; DriverType 0 UNKNOWN | confirmed |
| swapchain desc: Format | 0x1409590db | const | 0x1d = R8G8B8A8_UNORM_SRGB; BufferUsage 0x20 (@1409590e7); BufferCount 1 (@1409590ee); SwapEffect 0 DISCARD; Flags 0 (@140959118); Sample 1/0; Windowed = (screenMode != 1) | confirmed |
| `VideoDeviceDX11Windows::EnumerateVideoAdapters` | 0x1409595c0 | function | CreateDXGIFactory1 at 0x140959692 | confirmed |
| `VideoDeviceDX11Windows::AddWindow` | (CreateDXGIFactory at 0x140945fbe) | function | extra windows/swapchains possible | confirmed |
| CreateDeferredContext call | 0x1409592af | const | `call [r9+0xd8]` for i = 1..N-1 | confirmed |
| deferred-context error string | 0x141032d50 | string | at 0x14095955f | confirmed |
| `VideoDeviceDX11Windows::PostFlip` | 0x14095a600 | function | after Present: window-mode changes, device reset | confirmed |
| `VideoDeviceDX11Windows::SetSwapChainFullscreen` | 0x14095bd10 | function | | confirmed |
| VideoDevice vtable slot 17 (+0x88) | 0x14095a600 | vslot | PostFlip | confirmed |
| `VideoDeviceDX11::GetImmediateContext` | 0x1403d2d50 | function | `[this+0x468]` | confirmed |
| `VideoDeviceDX11::GetImmediateContextLocalThreadID` | 0x1403d2d60 | function | owner tid at +0x4a8 | confirmed |
| `VideoDeviceDX11::LockImmediateContext` | 0x140957780 | function | recursive Win32 mutex, recursion count in TLS+0xc074 | confirmed |
| `VideoDeviceDX11::UnlockImmediateContext` | 0x1409577f0 | function | | confirmed |
| `VideoDeviceDX11 ctor` | 0x140956b80 | function | `+0x4a0 = CreateMutexA(0,0,0)` | confirmed |
| mutex wrapper vtable | 0x140f675e0 | vtable | +8 WaitForSingleObject, +0x10 ReleaseMutex | confirmed |
| `GetUserDefinedAnnotation` | 0x1403d4740 | function | `*(this+0x480)[i]`; no callers | confirmed |
| `VideoDeviceDX11::GetDepthStencilTextureFormat` | 0x140957400 / 0x140957480 | function | FL32->R32_TYPELESS/D32_FLOAT, ZB16->R16/D16, 32S8->R32G8X24/D32_S8X24, 24S8->R24G8/D24_S8. Which one the main target uses: unresolved | confirmed (RC for main target) |
| `RenderLayerDX11 vftable` | 0x14102e4e0 | vtable | not in RTTI export. Slot 5 Create 0x140949920, 7 BeginRender 0x1409489d0, 8 EndRender thunk 0x14094adf0, 9 ExecuteRenderPasses 0x14094b320, 10 Flip 0x14094aec0, 16 DrawPass exec 0x14094ad70, 17 compute exec 0x14094a310, 18/19 fence no-ops, 20 (+0xa0) profiling event 0x140948010 | confirmed |
| `RenderLayerDX11::Create` | 0x140949920 | function | builds per-context array (+0x290); also fills depth-func table at this+0x35c; clamps RenderJobCount (forced 1 if contexts<3, 4 if ==3) | confirmed |
| `RenderLayerDX11::BeginRender` | 0x1409489d0 | function | per-context state reset FUN_14094c4d0 under immediate-context lock | confirmed |
| `RenderLayerDX11::ExecuteRenderPasses` | 0x14094b320 | function | locks immediate ctx; queues 6..1 inline; splits queue 0 into RenderJobCount enki jobs (priority 0); job 0 on main thread gets 2x budget; ExecuteCommandList in order; optional Flush | confirmed |
| AddTaskSetToPipe call (render jobs) | 0x14094b62d | const | | confirmed |
| `RenderJob::Execute` | 0x14094ae00 | function | job vftable 0x14102e860; ExecuteRenderPassRange on ctx of GetContextId; FinishCommandList if on worker | confirmed |
| FinishCommandList call | 0x14094ae9c | const | `call [rax+0x390]` | confirmed |
| ExecuteCommandList call | 0x14094b697 | const | `call [rax+0x1d0]` on ctx[0], RestoreContextState=FALSE; WaitforTaskSet 0x14094b67a | confirmed |
| Flush call (optional) | 0x14094b6ca | const | vtbl+0x378 | confirmed |
| `RenderLayerDX11::Flip` | 0x14094aec0 | function | smart-vsync query ring, Lock, Present, Unlock, RenderLayer::Flip -> PostFlip | confirmed |
| Present call | 0x14094b1aa | const | `call [rax+0x40]` on Window+0x48 swapchain, SyncInterval = vsync bool, Flags 0. Lock 0x14094b195, Unlock 0x14094b2da, RenderLayer::Flip 0x14094b2e5 | confirmed |
| `FUN_14094ad70` DrawPass exec (slot 16) | 0x14094ad70 | function | takes immediate lock only when context id == 0x80 | confirmed |
| `FUN_1409480f0` DrawPass executor | 0x1409480f0 | function | sets pass desc, calls FUN_140948b50 at 0x1409486bf | likely |
| `FUN_140948b50` DX11 RT bind + Ansel mark + depth clear | 0x140948b50 | function | clear flags 1/2/4/8/0x10; ClearDepthStencilView at 0x140948d94 with depth 0.0; isAnselAvailable at 0x140948db8; markBufferBind at 0x140948dca (IAT 0x140ec8030) | confirmed |
| `FUN_14094d1c0` DepthStencilState cache | 0x14094d1c0 | function | builds D3D11_DEPTH_STENCIL_DESC from 64-bit state (bit37 write, bit38 test, bits39-41 func); CreateDepthStencilState (+0xa8), OMSetDepthStencilState (+0x120) | confirmed |
| `DepthStencilState ctor` | 0x1408e0fd0 | function | default func param 6 -> table[6] = D3D11 GREATER_EQUAL (7) | confirmed |
| depth-func table | this+0x35c in RenderLayerDX11 | field | engine enum 0..7 -> NEVER, LESS, EQUAL, LEQUAL, GREATER, NOT_EQUAL, GEQUAL, ALWAYS | confirmed |
| `FUN_14094aab0` draw-pass executor | 0x14094aab0 | function | does not emit the pass name (annotation markers may be absent) | likely |
| `RenderLayerDX11::ExecuteProfilingEvent` (ours, FUN_140948010; vtbl 0x14102e4e0 slot 20) | 0x140948010 | function | runs on the executing job's context (immediate or deferred): ID3D11DeviceContext::End(query) + ID3DUserDefinedAnnotation BeginEvent (vtbl+0x18)/EndEvent (+0x20); via RenderLayer::AddProfilingEvent 0x1408d3aa0 / AddProfilingEventImmediate 0x1408d3b70 | confirmed |
| `RenderLayer::BeginProfilingEvent` / `EndProfilingEvent` | 0x1408d3d40 / 0x1408d6a90 | function | no static callers | confirmed |
| string IID annotation | 0x141032bc0 | string | queried per context in EnableVideoAdapter | confirmed |
| 'Occluded swapchain' / 'Minimal feature level not supported...' / 'No ID3DUserDefinedAnnotation interface available.' | 0x141032cb0 / 0x141032d08 / 0x141032d98 | string | never printed in the session log | confirmed |
| 'Adapter %s %u %u %u' / 'Fullscreen: %s' / 'SCREENMODE %s -> %s' | 0x141032ab8 / 0x141032c58 / 0x141032ee8 | string | log formats (lines 21-29, 58) | confirmed |
| 'Updating current refreshrate value: %d -> %d' / 'Resizing SwapChain: [ %dx%d ] ; %d/%d' / 'Device Reseted: %d times' | 0x141032dd0 / 0x1410329e8 / 0x141032f68 | string | | confirmed |
| `PlayAll::Graphics::ResizeSwapChainTarget` (name string) | 0x141032a10 | string | | confirmed |
| `GameEngine::CreateAndConfigureVideoDevice` | 0x1404ad280 | function | | confirmed |
| `GameEngine::CreateVideoDevice` | 0x1404b11a0 | function | logs 'Creating video device' (line 20) | confirmed |
| `GameEngine::CreateRenderWindow` | 0x1404b09f0 | function | logs 'Creating render window' (line 19) | confirmed |
| `GameEngine::ConfigureVideoDevice` | 0x1404aa550 | function | reads Lua 'PossibleSplitMode' | confirmed |
| `CoreManagers create` (GameEngine::CreateCoreManagers) | (call sites 0x1404adba0 LoadSettings, 0x1404adfc5/0x1404adfd1 AC snapshot, 0x1404ae131 ImGuiInit) | function | | confirmed |
| `GameEngine::CreateCoreModule` | 0x1404ae350 | function | calls CoreModule::Initialize (0x1404ae3b2) | confirmed |

### 2a. VideoDevice / VideoDeviceDX11 / RenderLayer fields

| Our name | Offset | Kind | Notes | Confidence |
|---|---|---|---|---|
| VideoDevice.renderThreadId | +0x88 | field | written in VideoDevice::Create (main thread) | confirmed |
| VideoDeviceDX11Windows.width / height | +0x418 / +0x41c | field | swapchain desc source; refresh at +0x424/+0x428 | confirmed |
| VideoDeviceDX11.ctx[] | +0x468 | field | ID3D11DeviceContext*[N], index 0 = immediate | confirmed |
| VideoDeviceDX11.ctx1[] | +0x470 | field | ID3D11DeviceContext1*[N] | confirmed |
| VideoDeviceDX11.device | +0x460 | field | ppDevice of CreateDeviceAndSwapChain | confirmed |
| VideoDeviceDX11.swapchain | +0x478 | field | ppSwapChain target | confirmed |
| VideoDeviceDX11.annotation[] | +0x480 | field | ID3DUserDefinedAnnotation*[N] | confirmed |
| VideoDeviceDX11.contextCount | +0x488 | field | = enki TaskScheduler+0x14 (default 4 workers + main = 5; RC) | confirmed (RC) |
| VideoDeviceDX11.immediateMutex | +0x490 (HANDLE +0x4a0) | field | owner local tid +0x4a8 (-1 when free) | confirmed |
| VideoDeviceDX11.outputWindow | +0x4c0 | field | HWND for swapchain desc | confirmed |
| RenderLayer.renderId | +0x30 | field | +1 per BeginRender. A BeginRender/EndRender per eye bumps this twice per presented frame | confirmed |
| RenderLayer.flipId | +0x34 | field | +1 per Flip | confirmed |
| RenderLayer.asyncComputeSupported | +0xb8..+0xba | field | 0x101, 0 | confirmed |
| RenderLayer.executeNow | +0x268 | field | 1 while executing or in immediate mode | confirmed |
| RenderLayer.perContextState | +0x290 (count +0x298) | field | stride 0x2c0: +0 ctx, +8 ctx1, +0x10 annotation, +0x250 context id (i\|0x80) | confirmed |
| RenderLayer.gpuTimestampRing | +0x470 / +0x488 / +0x4a0, idx +0x4c0 | field | ring of 3 for smart vsync | confirmed |
| DrawPassDesc.flags | +0x4b4 | field | bit 0x10 -> Ansel markBufferBind; setter not found | confirmed (setter unknown) |
| DrawPass.cost | +0x96c | field | job balancing; queue index +0x978 | confirmed |
| `TLS+0xc070` enki thread index / `TLS+0xc074` lock recursion | | field | | confirmed |

---

## 3. Render loop and passes

Engine pipe: `DeferredRenderLoop` (assumed active, RC) with `ForwardRenderLoop` alternative. Render(RenderDesc&) is re-entrant within a frame (external scenes) and natively multi-view (split screen).

| Our name | Address | Kind | Notes | Confidence |
|---|---|---|---|---|
| `RenderLoop::Create` | 0x1408da440 | function | FXAA/SSAO/split/TAA/motion-blur setup; calls tonemap init FUN_1409126c0 at 0x1408dad36 | confirmed |
| RenderLoop vftables | Deferred 0x140f7bfc8, Forward 0x140f919a0, base 0x140f778f8 | vtable | slot 7 +0x38 OnSplitChanged 0x1408dc2b0; 8 +0x40 PreRender 0x1408dc620; 9 +0x48 Render; 10 +0x50 CopySplitRT 0x1408da340; 11 +0x58 PrepareRenderInputPostProcess 0x1408469b0; 12 +0x60 RenderPostProcess 0x1408ddd90; 13/14 +0x68/+0x70 GetDepthTexture/GetNormalTexture; 17 +0x88 FillSceneConstantBuffer; 18 +0x90 CopyHDRRenderTarget 0x140846180 | confirmed (Deferred address: likely) |
| `DeferredRenderLoop::Render` | 0x1408469c0 | function | per-scene culling/lights/CSM, then per-view loop over RenderLoop+0x1ea8 views | confirmed |
| `ForwardRenderLoop::Render` | 0x140862f70 | function | same 0x620 stride | confirmed |
| `RenderLoop::PreRender` | 0x1408dc620 | function | fills active-view array (+0x1ea8) from mask at RenderDesc+0xce8 | confirmed |
| `RenderLoop::RenderProbe` | 0x1408ddeb0 | function | per OnRender and per external scene; before Render | confirmed |
| `RenderLoop::RenderPostProcess` | 0x1408ddd90 | function | only caller of PostProcessManager::Render (call 0x1408dde8c) | confirmed |
| `PostProcessManager::Render` | 0x1408bed60 | function | data-driven chain from COMMON:/ENGINE/POSTPROCESS/POSTPROCESS.XML (0x140f7d018) | confirmed |
| `RenderLoop::RenderFog` | 0x1408dd330 | function | pass 'RenderFog' | confirmed |
| `RenderLoop::UpscaleDynamicResolution` | 0x1408df750 | function | runs after TAA; pass 'Dynamic resolution upscale' | confirmed |
| `RenderLoop::SetTime` | 0x1408dedd0 | function | sets dt (+0x20), avg dt (+0x24), `ViewContext::s_bMotionPause` | confirmed |
| `RenderLoop::PrepareForCulling` | 0x1408dc850 | function | widens projection (max fov, min near, max far) before AddCullingData | confirmed |
| `RenderLoop::RestoreAfterCulling` | 0x1408ded00 | function | rebuilds SYMMETRIC projection via SetProjection: erases an injected per-eye projection | confirmed (RC) |
| `RenderLoop::SetupZFrontViewContext` | 0x1408df070 | function | front-pass ViewContext at RenderLoop+0x1590; own projection from Camera+0x8c/0x90/0x94 | confirmed |
| `DeferredRenderer::RenderGBuffer` | 0x140842040 | function | ZPrepass, Opaque, Motion Vectors passes | confirmed |
| `DeferredRenderer::RenderLowResTransparency` | 0x140843f20 | function | | confirmed |
| `DeferredRenderer::RenderSSR` | 0x140844de0 | function | only consumer of Last Frame Depth/Buffer (0x140844e17/0x140844e36) | confirmed |
| `DeferredRenderer::SetupLights` | (call 0x140846cdd) | function | -> DeferredLighting::SetupLights 0x14083d2c0 -> CSM::SetupRenderContext (call 0x14083d427) with the whole view array | confirmed |
| `DeferredRenderer::RenderDirectionalCascadedShadowMap` / `RenderShadowMaps` | 0x140841d60 / 0x140844f00 | function | 12-byte thunks via `[this+0x200]` to `DeferredLighting::RenderDirectionalCascadedShadowMap` 0x14083a7b0 / `RenderShadowMaps` 0x14083cd50 (called per view) | confirmed |
| `DeferredLighting::LightFroxel` | 0x140839120 | function | per view; temporal froxel history | likely |
| `DeferredLighting::CullFroxelLight` | 0x140836300 | function | | confirmed |
| `DeferredLighting::PrepareLightingDispatchCommand` / `RenderLightDispatchCommand` | 0x14083a010 / 0x14083a840 | function | | confirmed |
| `DeferredLighting::Create` | 0x140833fa0 | function | csLightFroxelTemporal xref at 0x140835338 | confirmed |
| `DeferredLighting::FillViewVector` | 0x1408384c0 | function | rebuilds symmetric projection | confirmed |
| `DeferredLighting::GetFrontPassInvProj` | 0x140839010 | function | front-pass inverse proj | confirmed |
| `DeferredLighting::FillLightRenderInstanceCollection` | 0x140837cb0 | function | rebuilds projection | confirmed |
| `CascadedShadowMapRenderer::SetupRenderContext` | 0x140828320 | function | adds ViewContext+0x448 to a rebuilt projection; per-camera arrays at +0x25b0 (stride 0x11a0 per idx) | confirmed (RC: fitted per view or merged) |
| `CascadedShadowMapRenderer::RenderStaticShadow` | 0x140827240 | function | calls ShiftStaticShadowMap | confirmed |
| `CascadedShadowMapRenderer::ShiftStaticShadowMap` | 0x140829380 | function | rebases persistent static shadow cache on World Shift | confirmed |
| `SkyRenderer::Render` | 0x140909c30 | function | 'Render Sky Deferred' | confirmed |
| `HudRenderer::Render` | 0x14088b530 | function | see HUD/UI | confirmed |
| `IntelSSAO::Render` / `UpdateConstants` | 0x140927c50 / 0x140927e20 | function | no history buffers seen (spatial only); reads s_vResolutionScaling | likely (RC) |
| `LensFlareInstance::Update` | 0x140898df0 | function | per-camera-index fade (+0x64+idx*4) and occlusion queries (+0x90+idx*8) | confirmed |
| `RenderEntity::FillRenderInstances` | 0x1408d06c0 | function | virtual +0x60 UpdateRenderData; sets entity+0x70 = frameId | confirmed |
| `VisibilityCuller::AddCullingData` | 0x14091c850 | function | copies frustum (+0x3b0..+0x42f), cull pos, view ids; called at 0x140846c5f, 0x140846de2, 0x140846e5e, 0x140846edc | confirmed |
| `VisibilityCuller::StartUpdateVisibilityInfo` | 0x14091db50 | function | mask from +0x460/+0x464 | confirmed |
| `RenderContext::Prepare(Camera*, Viewport&)` | 0x1408cfae0 | function | InitFromCamera(this+0x130, zero jitter) | confirmed |
| `RenderContext::Prepare(const ViewContext&)` | 0x1408cfad0 | function | = ViewContext::operator= (copies jitter too); used in the view loop | confirmed |
| `RenderContext::GetViewContext` | 0x1404b3da0 | function | `lea rax,[rcx+0x130]` | confirmed |
| `RenderUtils::SetDeferredViewContext` / `SetImmediateViewContext` | 0x1408efdd0 / 0x1408efdf0 | function | debug-draw ViewContext copies (RenderUtils+0x40 / +0x125e00); NOT D3D deferred contexts | confirmed |
| `ProbeRenderer::Update` | 0x14056b3a0 | function | once per frame in OnRender | confirmed |
| `ProbeRenderer::RenderAt` | 0x140568410 | function | offline probe bake; own ViewContext (InitFromCamera at 0x140569163) | likely |
| `HHParticlesManager::UpdateFence` / `SceneManager::UpdatePopCornBound` | (in Render loop) | function | only when camera idx == 0 | confirmed (RC) |
| `HighResolutionScreenshot::MakeScreenshot` | 0x140873da0 | function | tiled; multi-frame, not in-frame re-entrant | likely |
| `HighResolutionScreenshot::MakeTemporalAntiAliasingScreenshot` | 0x140874710 | function | jittered accumulation | likely |
| `ToneMapper::Render` (FUN_140914680) | 0x140914680 | function | called per view at 0x140848768; luminance FUN_1409120b0 (call 0x140914865), FinalCompose FUN_1409131d0 (call 0x140914889), clamp FUN_140914500 (call 0x140914cd3) | likely |
| `FUN_1409126c0` tonemap init | 0x1409126c0 | function | 'Reset luminance' debug callback | confirmed |
| `FUN_140914e20` | 0x140914e20 | function | takes camIdx; separate water-drop state | likely |
| `RenderLoop::FillRenderInstancesJob::Execute` (FUN_1408d3370) | 0x1408d3370 | function | ours; vtbl 0x140f777b8 slot 2; frameId = `RenderLayer::s_pInstance+0x30` read at run time; per entity, calls vtbl +0x70 (`FillRenderInstances`) once per visible culler view (main views, cascades, static) with that view's RenderInstanceManager | confirmed |
| RenderLoop job set vtable / `QueueSubJobs` (FUN_1408d34e0) / `CopyRenderInstancesJob` (FUN_1408d3330) | 0x140f777d8 / 0x1408d34e0 / 0x1408d3330 | vtable / function | ours; `RenderLoop+0xd10` six job sets (stride 0x48), WaitGroup 'FillRenderInstance' at `+0xec0` | likely |
| `CascadedShadowMapRenderer::Render` | 0x140826230 | function | per view: 'Clear Shadow Map' on the shared atlas (`CSM+0x25b8`), `RenderStaticShadow` (idx, 0/1), 3 'ShadowMap' cascade passes, builds `CascadedShadowContext` -> `RenderContext::SetCascadedShadowContext` (renderer.md 3) | confirmed |
| `DeferredLighting::RenderDirectionalCascadedShadowMap` | 0x14083a7b0 | function | target of the DeferredRenderer thunk 0x140841d60; `CSM::Render` if `DL+idx*0x1b0+0x100 == 6` else `SetDefaultCascadedShadowContext` 0x140827fd0; then `RenderFarShadows` 0x140826a90 if `DL+0x460` is 5/6 | confirmed |
| `DeferredLighting::RenderShadowMaps` | 0x14083cd50 | function | target of thunk 0x140844f00 (spot/point shadows, own atlas) | confirmed |
| `RenderContext::SetCascadedShadowContext` | 0x1408cfb00 | function | builds the `cbPSCascadedShadows` data from `RenderContext+0x5d0..+0x794` | confirmed |
| `CascadedShadowMapRenderer::SetupStaticRenderContext` / `BuildStaticProjectionMatrix` | 0x140828eb0 / 0x1408239d0 | function | static map = scrolling cache per idx; centre from the scene-graph camera node; mode `StaticShadowMap+0x8b0` (1 none, 2 tiled refill, 3-6 strip) | confirmed |
| `ShadowClearPass` (FUN_140903040) / `ShadowSubmitCascade` (FUN_1409037d0) | 0x140903040 / 0x1409037d0 | function | ours; DrawPass 'Clear Shadow Map' / 'ShadowMap' (depth only) | confirmed |
| CSM fields | `+0x25b0+idx` (cascaded light valid), `+0x25b8` (shared atlas renderer, RT at +0x18), `+(idx+0x17)*0x10` (cascade RenderContext array, stride 0x830, ViewContext +0x130), `+0x25c2` (skip fit), `+0x2658` (static size), `+0x265c` (slice overlap) | field | | confirmed |
| `LensFlareManager::Update` / `Render` | 0x14089a770 / 0x14089a3a0 | function | Update: only caller DoUpdate (call 0x1405ae3a7), per mask bit, after InitFromCamera; Render: per view (call 0x140848cbd) | confirmed |
| `HHParticlesManager::CullParticles` | 0x1409a2a90 | function | call 0x1405affb9 after Render; union of both views' cull bits (`1 << (camIdx*13 + pass)`), not view 0 only | confirmed |
| `HHParticlesManager::SetViewContext` | 0x1409a4f80 | function | call 0x14048c44a before DoUpdate: copies last frame's ViewContexts (idx 0 -> camera slots 0/1/3, idx 1 -> slot 2) | confirmed |
| `HHParticlesManager::EnableMedium` | 0x1409a3100 | function | flags `+0x2c+i` (copied to `+0x28+i` in `BeforeParticleUpdate` 0x1409a1890); only caller RenderOneFrame: `EnableMedium(Renderer+0xdd8, 2, GameEngine+0x184 != 0)` each frame | confirmed |
| HHParticlesManager medium flags | `+0x28..+0x2b` active (copied from `+0x2c..+0x2f` requested in `BeforeParticleUpdate` 0x1409a1890; ctor 0x1409a0b40 sets all to 1) | field | read by `UpdateSpawn` 0x1409a7810 / `UpdateEvolve` 0x1409a7350 (collections 0..2), `UpdatePhysical*` (collection 3); rendering ignores them | confirmed |
| `HHParticlesComponent.updatePerSplit` / `HHParticleEffectInstance.mediumIndex` | component `+0x4a` / instance `+0x6c` | field | "Update Per Split" (`RegisterMembers` 0x1404c9e00); medium passed to `HHParticleEffectInstance::Load` 0x1409a8c10 | confirmed |
| `GameContext+0x23` | field | field | `EngineInitDesc` byte 0 (also creates `EditorSaveCallback`, `FUN_1404835a0`): editor/tool flag; 0 in a normal session | likely |
| `HHParticlesComponent::PostObjectInit` | 0x1405091f0 | function | creates one instance (medium 0) or a per-camera pair (mediums 1 and 2) if `component+0x4a` and `GameContext+0x23 == 0` | confirmed |
| `StartRenderParticles` / `KickRenderParticles` | 0x1409a6a10 / 0x1409a3e00 | function | per view; collections 0, 1 (idx 0) or 2 (idx 1), 3 | confirmed |
| `DeferredLighting::RenderProbeImpl` (FUN_1408c7270) | 0x1408c7270 | function | ours; probe relight state machine: nearest probe to any view, one cube face per call, 7-step cycle | confirmed (structure) |
| `SimpleForwardRenderer::Render` | 0x140904070 | function | call 0x140847957, once per frame in the view whose idx = `GetNextCameraIndex` 0x140903f60; renders `GameServices::SimpleForwardView` (Create 0x1405df0f0, RT 'SimpleForwardView') | confirmed |
| `MeshPainter::Update` | 0x1408a7920 | function | call 0x140847cff per view, unguarded prev <- cur latch (active if `this+0x38`) | likely (RC) |
| `DayTime::Manage` -> `EnvironmentSettingsMixer::LerpAndApply` -> `ApplyParams` | 0x14045e1e0 -> 0x14047a2d0 -> 0x140479000 | function | per frame; ApplyParams writes both camera-index slots of bloom, HDR, grading, wind, vignette, weather, fog (`Scene+0x138+idx*0x98`), ambient cube (`Scene+0x20+idx*4`), sky (renderer.md 6) | confirmed |
| `WaterDrop` camera motion update (FUN_1403be8e0) | 0x1403be8e0 | function | ours; loop idx < 2 over `g_pGameEngine vtbl+0x570(idx)` (player actor): slot 1 never fed without a second player. WaterDrop slot stride 0x68 from `+0x60` (+0xa0 rain, +0xa4 speed, +0xb0 accel, +0xc0 alive, +0xc1 enable) | likely |
| `KTUDof::OnManage` / `KTUDof::Activate` | 0x1409e6680 / 0x1409e5a90 | function | slot 1 focus only if `Renderer+0xe40 != 0` and `KTUDof+0x364 != -1` (target pushed with mask bit 2) | confirmed |
| `s_vLastViewPos0` (DAT_1413cdb14..1c) | 0x1413cdb14 | global | ours; last frame's ViewContext[0] position, copied at 0x14048c4ee; readers unknown | confirmed (write) |

### 3a. Pass name strings (DrawPassDesc.name; order inside one view)

Names are strings in the exe; the DX11 executor may not emit them as annotations (RC: check RenderDoc). Order is the call order in `DeferredRenderLoop::Render` (confirmed from decompile).

| Our name | Address | Kind | Notes | Confidence |
|---|---|---|---|---|
| GBuffer: 'ZPrepass', 'Opaque Front', 'AlphaTest Front', 'Opaque', 'Alpha test', 'PKFX-Opaque', 'Clear GBuffer', 'Before Ocean Decal blend', 'Ocean', 'Gbuffer blend', 'Post-Decal blend', 'Decal blend', 'Decal AO blend', 'Motion Vectors', 'Motion Vectors front pass' | in RenderGBuffer | string | 'Motion Vectors' 0x140fdc4a8, 'Motion Vectors front pass' 0x140fdc4b8 | confirmed |
| Lighting: 'Tile Min Max Depth CS', 'Clear Dispatch CS', 'Cull Light CS', 'Froxel Cull Light CS', 'Z Bin Light CS', 'Light Froxel CS', 'Volumetric accumulation CS', 'Main Lighting', 'Fog On Sky', 'Sum Lighting CS', 'SSR pass', 'IntelSSAOPrepareDepths/Normal/Generate/Blur' | DeferredLighting | string | | confirmed |
| Forward/transp: 'Render Sky Deferred', 'Render Forward Opaque', 'RenderFog', 'Render Transparency', 'Downscale Linear Depth', 'LowRes Transparency 125', 'Transparency Low Res', 'Final Blend pass', 'Render Opaque Front' (0x140fe1b58), 'Render AlphaTest Front' (0x140fe1b70), 'Render Transparent Front' (0x140fe1b38) | | string | | confirmed |
| HDR/post: 'HUD3D Renderer Frontpass', 'Generate last frame mips' (0x140fff170), 'Downscale', 'HDR_LuminanceInit/Downscale/Final', 'HDR_FinalCompose', 'TAA resolve' (0x140fff220), 'Render MotionBlur' (0x140fff128), 'Lens flares occlusions query', 'Lens flares render', 'HUD2D Renderer', 'Dynamic resolution upscale' (0x140fff140), 'upscale_simple' (0x140fff160), 'Split border' (0x140fdcb38), 'ImGui', 'PostProcess Read' | | string | | confirmed |
| string 'Engine.Renderer.Split' | 0x140ffefb0 | string | | confirmed |
| string 'Engine.Renderer.FXAA' | 0x140ffef50 | string | | confirmed |
| 'ExternalSceneRendering' (RT desc) / 'SceneRenderer' (draw pass) | in Renderer::OnRender | string | external scene path used by menu previews | confirmed |
| 'Render Movieplayer' | in Renderer::OnRender | string | fullscreen movie: scene render is skipped that frame | confirmed |
| string 'Wait graphics queue' | 0x140ffe770 | string | GPU fence label | confirmed |

### 3b. RenderDesc (Renderer+0x30) fields

| Our name | Offset | Kind | Notes | Confidence |
|---|---|---|---|---|
| RenderDesc.view[2] | +0 (stride 0x620) | field | ViewContext objects | confirmed |
| RenderDesc.csm | +0xc40 | field | CascadedShadowMapRenderer* | likely |
| RenderDesc.weather | +0xc48 | field | WeatherRenderer* | likely |
| RenderDesc.globalHudList | +0xc50 | field | HudItemList* (global / split HUD) | likely |
| RenderDesc.splitMode | +0xc60 | field | 0 -> 1 view; 1, 2, 4 -> 2 views; 3 -> 4 views | confirmed |
| RenderDesc.viewMask | +0xce8 | field | | confirmed |
| RenderDesc.shiftInfo / flag | +0xcec / +0xcf8 | field | World Shift vec3 for render side | confirmed |
| RenderDesc.debugBounds | +0xcfc | field | | likely |
| RenderDesc.viewport fractions (view 1) | +0xc44/+0xc4c/+0xc50 (offset 0.5, size 0.5/1.0 in mode 1) | field | **CONFLICT** with camera note (+0x5f4..+0x600 inside the ViewContext slot) and renderloop note (+0x624..+0x630). Unresolved | guess (RC) |
| RenderDesc.drawFirstView | per view `+0x510` (`this[view*0x620+0x510] = (iVar12==0)`) | field | external-scene loop marks the first | likely |

### 3c. RenderLoop / DeferredRenderLoop non-temporal fields

| Our name | Offset | Kind | Notes | Confidence |
|---|---|---|---|---|
| RenderLoop.frameDtMs / avgDtMs | +0x20 / +0x24 | field | from SetTime; dt==0 disables TAA and motion blur | confirmed |
| RenderLoop.hudRenderer | +0x410 | field | HudRenderer member | confirmed |
| RenderLoop.taaRTSize | +0xc60 | field | size used by InitializeTAA and last-frame buffers | confirmed |
| RenderLoop.motionVectorRT | +0xca8 | field | one shared RG16 RT; GetMotionVectorsRenderTarget 0x1404f89e0 | confirmed |
| RenderLoop.frontDepthSlice | +0x1118 | field | front-pass viewport MaxDepth = 1 - this; zero by ctor (0x1408d92ba) | likely (RC) |
| RenderLoop.hasFrontPass | +0x111c | field | HasFrontPass 0x1408db2d0 also needs Camera+0x98 | confirmed |
| RenderLoop.frontViewContext | +0x1590 (copy at +0x15a8) | field | front-pass ViewContext | confirmed |
| RenderLoop.frontLatchGate | +0x1e70 | field | gates the front-pass last-frame latch | likely |
| RenderLoop.activeViews | +0x1ea8 (count +0x1eac, array +0x1eb0) | field | built by PreRender | confirmed |
| RenderLoop.toneMapper | +0x1ed0 | field | tonemap object; byte +0x150 toggled by Ansel Start/StopAnselCapture (and gates the luminance chain per renderloop note) | likely |
| RenderLoop.hdrTarget holder | +0x1ef0 (RT at +0x1d8) | field | HUD flag 8 target | likely |
| RenderLoop.renderContext | +0x1f00 (embedded ViewContext +0x2030) | field | re-prepared per view; ResetProjectionOffset acts on +0x2030 | likely (RC) |
| RenderLoop.sceneList | +0x2738 | field | unique scenes | likely |

---

## 4. ViewContext and TransformMatrix

`ViewContext` size 0x620; one per view in `RenderDesc` and one embedded in `RenderContext+0x130`. Matrices are column-vector math (clip = M * v) stored row-major (element [r][c] at r*4+c; translation at 3,7,11), see `docs/conventions.md` for the authoritative convention.

| Our name | Address | Kind | Notes | Confidence |
|---|---|---|---|---|
| `ViewContext::InitFromCamera` | 0x14091aeb0 | function | (Camera*, Viewport&, Vector2& offset). Callers: DoUpdate x6 (0x1405ae0a7, 0ae0eb, 0ae13a, 0ae17e, 0ae20c, 0ae2d7), UpdateShift 0x1404d5a41, OnRender 0x1405afcb3, ProbeRenderer::RenderAt 0x140569163, Pick3dInList 0x1408780c5, RenderContext::Prepare | confirmed |
| `ViewContext::InitFromCameraStereo` | 0x14091b100 | function | DEAD (data refs 0x1411a9cf8, 0x1414c60bc only) | confirmed |
| `ViewContext::SetViewProjectionStereo` | 0x14091b850 | function | DEAD; SetViewProjection(+0x18,p1,p3), (+0x1a8,p2,p3) | confirmed |
| `ViewContext::IsStereoscopic` | 0x1404fd5c0 | function | `movzx eax, byte [rcx+0x466]`; no code callers | confirmed |
| `ViewContext::SetProjection` | 0x14091b7e0 | function | writes slot 0; slot 1 only if +0x466 | confirmed |
| `ViewContext::ResetProjectionOffset` | 0x14091b570 | function | subtracts +0x448/+0x44c from projection (both slots if stereo); called at 0x140848c72 and in SetDeferredViewContext 0x1408efdea | confirmed |
| `ViewContext::GetProjectionNoOffset` | 0x14091adb0 | function | proj with jitter removed (eye-index arg); used for last-frame VP | confirmed |
| `ViewContext::SetActive` | 0x14091b780 | function | fills per-view CB; reads only slot 0 | confirmed |
| `ViewContext::UpdatePerCameraCB` | 0x14091b910 | function | fills cbCameraStd: Proj * rotation-only view (camera-relative), cam pos separately | confirmed |
| `ViewContext::RebuildFrustum` | 0x14091b4e0 | function | Frustum from viewProj (slot 0) + clip bounds +0x438/+0x440; so asymmetric projections cull correctly if installed before AddCullingData | confirmed |
| `ViewContext::s_bMotionPause` | 0x14140bb30 | global | set from game dt==0; freezes last-frame latching | confirmed |
| `g_bLockCullingQuery` | 0x14140bcc0 | global | freezes cull pos (+0xc) and culling frustum updates | confirmed |
| shared per-view CB id | 0x14140bb48 | global | `RenderLayer::GetSharedCB(DAT_14140bb48)` -> ViewContext+0x458 | confirmed |
| `Math::MatrixPerspectiveFovRH` | 0x140813390 | function | (out, hFovRad, near, far, aspectScale). RH, view-space forward -Z, REVERSED-Z, finite far, [0,1]. m[0]=1/tan(fov/2), m[5]=m[0]*aspect, m[10]=n/(f-n), m[11]=nf/(f-n), m[14]=-1. ~20 callers; choke point | confirmed (RC) |
| `Math::MatrixPerspectiveFovLH` | 0x1408132f0 | function | near-twin of the RH variant (first byte difference at +0x33); patterns for the RH one must extend past it | confirmed |
| `Math::MatrixOrthoRH` | 0x140813260 | function | also reversed; param order odd (near=0 passed as arg 4); use by 3D scene passes unknown | likely |
| `Math::Matrix44_Inverse` (FUN_14046b6c0) | 0x14046b6c0 | function | general cofactor inverse (out, det|null, in) | confirmed |
| `Math::Matrix44_Mul` (FUN_14046baa0) | 0x14046baa0 | function | out = A*B, row-major indexing | confirmed |
| `Math::Matrix34_Mul` (FUN_14016cad0) | 0x14016cad0 | function | affine 3x4 A*B | confirmed |
| `QuaternionRotationMatrix` | 0x1408166a0 | function | | confirmed |
| `QuaternionRotationVectors` | 0x140816aa0 | function | | confirmed |
| `TransformMatrix::Get(i)` | 0x140914fe0 | function | `this + i*0x40` | confirmed |
| `TransformMatrix::RecomputeView` | 0x140914ff0 | function | view at +0, camera world +0x40, rot-only view +0x80 | confirmed |
| `TransformMatrix::SetProjection` | 0x1409150d0 | function | proj +0xc0, ViewProj = P*V +0x100, inverse +0x140, invProj terms +0x180 | confirmed |
| `TransformMatrix::SetView` | 0x1409151e0 | function | | confirmed |
| `TransformMatrix::SetViewProjection` | 0x140915260 | function | RecomputeView + SetProjection | confirmed |
| string 'cbCameraStd' layout | UpdatePerCameraCB | string | +0x00 Proj*RotOnlyView, +0x40 cam pos, +0x50 invProj data, +0x60 view forward, +0x70 cull pos + viewIdx mask, +0x80 yaw matrix, +0xC0 near/far, +0xD0 matrix (looks like camera-relative inverse VP), +0x110 projection offset. Copied WITHOUT transposing; shader packing unresolved | likely (RC) |

### 4a. ViewContext fields

| Our name | Offset | Kind | Notes | Confidence |
|---|---|---|---|---|
| ViewContext.camPos | +0x00 (float3) | field | column 3 of camera world matrix | confirmed |
| ViewContext.cullPos | +0x0c | field | frozen when g_bLockCullingQuery | confirmed |
| ViewContext.transform[0] | +0x18 | field | TransformMatrix, size 0x190 (the only one consumed) | confirmed |
| ViewContext.transform[1] | +0x1a8 | field | second eye block, only filled by dead stereo init; never consumed by named readers | confirmed |
| ViewContext.viewport | +0x338 (w +0x340, h +0x344) | field | | confirmed |
| ViewContext.frustum | +0x350 | field | | confirmed |
| ViewContext.simdFrustum | +0x3b0..+0x42f | field | copied by AddCullingData | confirmed |
| ViewContext.nearFar | +0x430 / +0x434 | field | from Camera+0x7c/+0x80 | confirmed |
| ViewContext.clipBounds | +0x438..+0x444 | field | | confirmed |
| ViewContext.projOffset | +0x448 / +0x44c | field | Vector2 added to proj[0][2], proj[1][2]; TAA jitter or Ansel offset. = RenderContext+0x578/0x57c | confirmed |
| ViewContext.camera | +0x450 | field | Camera* | confirmed |
| ViewContext.sharedCB | +0x458 | field | ConstantBuffer* | confirmed |
| ViewContext.viewIndexBase | +0x460 | field | viewIndex = cameraIndex*13 + base; 13 slots x 2 cameras in a 32-bit mask | confirmed |
| ViewContext.cameraIndex | +0x464 (ushort) | field | selects every per-slot history array (TAA, last-frame, froxel, exposure, CSM). = RenderContext+0x594 | confirmed |
| ViewContext.stereoFlag | +0x466 (byte) | field | set by InitFromCameraStereo; **CONFLICT** on readers, see conflicts | confirmed (readers unresolved) |
| ViewContext.cascadedShadowDesc | +0x470 | field | CascadedShadowRenderDesc | likely |
| ViewContext.lightingDesc | +0x4dc / +0x4e4 | field | | likely |
| ViewContext.finalRT | +0x590 (RT at ->+0x18) | field | HUD global-list target | likely |
| ViewContext.sceneGraph | +0x598 | field | | likely |
| ViewContext.hudItemList | +0x5e8 | field | per-view 2D HUD list | likely |
| ViewContext.cameraRenderDescOption | +0x608 | field | post options | likely |
| ViewContext.renderMode | +0x610 | field | visualizer mode | likely |
| ViewContext.viewportFractions | (see conflict) | field | | guess |
| RenderContext.viewContext | +0x130 | field | embedded; RenderContext+0x578/+0x57c jitter, +0x580 Camera*, +0x594 camera index | confirmed |

### 4b. TransformMatrix layout (size 0x190)

| Slot | Offset | Content | Confidence |
|---|---|---|---|
| Get(0) | +0x00 | view | confirmed |
| Get(1) | +0x40 | inverse view = camera world 4x4 | confirmed |
| Get(2) | +0x80 | rotation-only view (translation zeroed). Camera note calls it "inverse-view no translation"; UpdatePerCameraCB code zeroes the translation then inverts, matching rotation-only view | confirmed |
| Get(3) | +0xc0 | projection incl. jitter | confirmed |
| Get(4) | +0x100 | viewProj = P*V | confirmed |
| Get(5) | +0x140 | invViewProj | confirmed |
| extra | +0x180..+0x18f | invProj elements [10],[11],[14],[15] for depth linearisation: viewZ = (i10*z+i11)/(i14*z+i15) | confirmed |

---

## 5. Camera

Per-frame data flow (confirmed): `CameraManager::OnPostManage` -> `KTUCamera::Update` -> active mode `Manage` -> mode Matrix34 -> camera node local matrix -> FOV/clips copied to `Graphics::Camera` -> `GraphNode::Update` (world matrix final at 0x1409d94cf) -> later `Renderer::DoUpdate` builds ViewContexts. Hook ideas are in `camera.md`; this table names things.

| Our name | Address | Kind | Notes | Confidence |
|---|---|---|---|---|
| `KTUCamera::Update` | 0x1409d9060 | function | KTUCamera vtable 0x141047db0 slot 1; final write of camera TM/FOV/clips; Hor+ compensation beyond 16:9 (ref 1.7777778, ComputeTripleScreenAspectRatio 0x140945cb0) | confirmed |
| KTUCamera::Update: mode Manage call | 0x1409d9125 | const | `call [mode+0x30]`; override mode Manage at 0x1409d947c | confirmed |
| KTUCamera::Update: pool write EnableFlag(9) | 0x1409d923f | const | | confirmed |
| KTUCamera::Update: SetFov call | 0x1409d93e4 | const | main path; alt path 0x1409d929e | confirmed |
| KTUCamera::Update: SetFrontFov call | 0x1409d93f1 | const | | confirmed |
| KTUCamera::Update: SetClips call / SetFrontClips call | 0x1409d942e / 0x1409d9447 | const | | confirmed |
| KTUCamera::Update: GraphNode::Update call | 0x1409d94cf | const | after it the camera world matrix is final for the frame | confirmed |
| `KTUCamera::SwitchFreeDebug` | 0x1409d8d20 | function | toggles override mode 'FreeCam' at KTUCamera+0x68 (from KTUDebug::OnManage 0x1409e5368) | confirmed |
| `KTUCamera::EnterMode` / `GetMode` | 0x1409d7a10 / 0x1409d7b90 | function | | confirmed |
| `KTUCamera::AddShake` / `AddSmoothShake` / `AddBasicShake` / `ClearShakes` | 0x1409d6bd0 / 0x1409d6cf0 / 0x1409d6a70 / 0x1409d6ef0 | function | shake list at KTUCamera+0x80 | confirmed |
| `KTUCamera::Create` | 0x1409d6f50 | function | exported | confirmed |
| KTUCameraShake Manage variants | 0x1409d7d30 / 0x1409d7f10 / 0x1409d7f80 / 0x1409d83e0 | function | | confirmed |
| `KTUCameraTimedRGBShift` | 0x1409d7fe0 | function | post effect | confirmed |
| `KTUCamera vftable` | 0x141047db0 | vtable | slot 1 = Update; ctor region string 'Engine.Camera' 0x141047d80 | confirmed |
| `KTUCameraMode vftable` | 0x1410477e0 | vtable | slot 2 Enter 0x1409e0cb0, 5 Exit, 6 Manage 0x1409e1050 (+0x30), 9 OnShift 0x1409e1560, 10 FillListener, 15 GetDof (+0x78) | confirmed |
| `Game::Camera vftable` | 0x140f6ce70 | vtable | base of KTUCamera and AnselCam | confirmed |
| `Game::Camera::Create` | 0x140424690 | function | instantiates Graphics::Camera, LinkToNode(scene root), SetFov 1.5708 | confirmed |
| `Game::Camera::SetActive` | 0x140424890 | function | | confirmed |
| `Graphics::Camera vftable` | 0x140f82cc0 | vtable | derives GraphNode | confirmed |
| `Graphics::Camera ctor` | 0x140821be0 | function | defaults: fov 45 deg (0x3f490fdb), near 0.5, far 10000 (0x461c4000), aspect 1, ortho width 150, front fov 45 deg, front near 0.1, front far 10 | confirmed |
| `Camera::SetClips` / `SetFrontClips` / `SetAspectRatio` / `SetClipFar` / `SetClipNear` / `EnableFrontPass` | 0x140821cb0 / 0x140821ce0 / 0x140821c90 / 0x140821ca0 / 0x1403e7e90 / 0x1403e7de0 | function | | confirmed |
| `Camera::SetDofDatas` | 0x140821cc0 | function | | confirmed |
| `Camera::SetWidth` | 0x1405024d0 | function | ortho width | confirmed |
| `Camera::GetClipNear` / `GetClipFar` | 0x14012ddc0 / 0x14012ec40 | function | +0x7c / +0x80 | confirmed |
| `Camera::SetFov` / `GetFov` | 0x14048caf0 / 0x14011def0 | function | ICF aliases; see header | confirmed |
| `Camera::SetFrontFov` / `GetFrontFov` | 0x140501650 / 0x14012e630 | function | ICF aliases | confirmed |
| `CameraManager vftable` | 0x140f6dd98 | vtable | slot 8 OnPostManage 0x14042efe0; slots 11/12 Before/AfterPhysicsStep | confirmed |
| `CameraManager::OnPostManage` | 0x14042efe0 | function | iterates cameras -> camera vtbl+8 (KTUCamera::Update) | confirmed |
| `CameraManager::GetActiveGFXCamera` | 0x14042ee10 | function | `*(CameraManager+0x50+i*8)+0x10` | confirmed |
| `CameraManager::SetActiveCamera` | 0x14042f150 | function | swaps active camera, rebuilds CameraDesc, Renderer vtbl+0x68 SetCameraDesc | confirmed |
| `KTUGameEngine::GetCamera(int)` | 0x140651200 | function | player array +0x3b8 stride 0xc0 (count +0x3b4), +0x18 = KTUCamera* | confirmed |
| `GraphNode::Update` / `UpdateObjectTM` | 0x14086f1e0 / 0x14086f2b0 | function | jmp; world = local or parentWorld*local | confirmed |
| `GraphNode::SetWorldMatrix` | 0x14086ecd0 | function | single write point for the Ansel free camera | confirmed |
| `GraphNode::ShiftOrigin` | 0x14086eee0 | function | translates BOTH world-matrix arrays; flag 0x80 | confirmed |
| `GraphNode::GetLocalMatrix` / `GetWorldMatrix` / `GetWorldPosition` | 0x1403d2fa0 / 0x1403d47c0 / 0x1403f1550 | function | | confirmed |
| `GraphNodeChunkPool.localMatrices` | 0x1413cda80 | global | Matrix34 array base, stride 0x30, indexed by GraphNode+0x50 | confirmed |
| `GraphNodeChunkPool.worldMatrices` | 0x1413cda88 | global | camera world = base + node*0x30; read directly by InitFromCamera and Mesh::UpdateRenderData | confirmed |
| `GraphNodeChunkPool.nodeData` | 0x1413cda90 | global | stride 0x18, ushort flags at +4 (1 local dirty, 8 world updated, 0x40, 0x80 shifted) | confirmed |
| `_s_oInstance` (local pool) | (GraphNode pool) | global | shifted together with 0x1413cda88 | confirmed |
| `TTSpringCameraMode vftable` | 0x140f338f8 | vtable | slot 0 dtor FUN_14019b0e0, 2 Enter FUN_14019c0a0, 5 Exit FUN_14019d210, 6 Manage FUN_14019d700, 8 AfterPhysicsStep FUN_14019b200, 9 OnShift FUN_14019dc90 | confirmed |
| `TTSpringCameraMode::Manage` | 0x14019d700 | function | resolves vehicle, calls ComputeView | confirmed |
| `TTSpringCameraMode::ComputeView` (FUN_1401a11c0) | 0x1401a11c0 | function | FOV/front-FOV speed lerp, dispatch by CameraType, writes mode+8, +0x80, +0xa8 (deg->rad at 0x1401a15fb) | confirmed |
| `TTSpringCameraMode::UpdateSpringCamView` | 0x1401a34b0 | function | name string 0x140f33b08; 'Invalid Spring camera matrix' 0x140f33ae8; Matrix34::IsValid guard | confirmed |
| `TTSpringCameraMode::UpdateRunChaseCam` | 0x1401a1680 | function | | confirmed |
| `TTSpringCameraMode::PhysicsStepTarget` (FUN_1401a23e0) | 0x1401a23e0 | function | target from bike physics actor, adds lean/tuck, slerp (dt*70 via FUN_14019de80) into +0x3d4/+0x3e4 | likely |
| `TTCamera::LeanHead` | 0x14019b880 | function | settings +0xbc..+0xe0 | likely |
| `TTCamera::TuckInOut` | 0x1401a50e0 | function | settings +0x88..+0xb0; calls 0x14019b400 | likely |
| `TTCamera::LookAroundInput` | 0x1401a0a70 | function | stick/hat look; quaternion state mode+0x2c0 | likely |
| `TTCamera::HelmetSprings` | 0x1401a04c0 | function | outputs matrix at mode+0x4c4 (helmet/visor, not obviously the eye) | likely |
| TT2 player camera `AfterPhysicsStep` (FUN_1403afc80) | 0x1403afc80 | function | vtable slot at 0x140f60b00; places the HelmetView actor: `Actor::SetWorldMatrix(cam+0x160, mode+0x4c4)` when mode type 2 (camera-mode manager `DAT_14141e5c8`) | confirmed |
| TT2 camera.helmetViewActor / helmetNode / viewEnum / helmetVisible | +0x160 / +0x158 / +0x138 / +0x360 | field | HelmetView actor, `Helmet_01` mesh node, GameCameraViews value, visibility flag (`FUN_1403b20a0`, `FUN_1403ae6a0`, `FUN_1403af2b0`) | confirmed |
| TTSpringCameraMode.helmetMatrix | +0x4c4 | field | Matrix34 from HelmetSprings; origin read by `FUN_14019d4b0` | confirmed |
| TTSpringCameraMode.cameraMatrix | +0x8 | field | Matrix34 the mode outputs (copied from +0x3e4 in ComputeView) | confirmed |
| `FUN_1403b20a0` HelmetView creation | 0x1403b20a0 | function | creates `HelmetView_%d` from `ARCHETYPES:/GAMEPLAY/HELMETVIEW.PPF`, finds `Helmet_01` | confirmed |
| `FUN_1401a5570` | 0x1401a5570 | function | called after HelmetSprings | guess |
| `TTCameraType enum init` | 0x140056a70 | function | Fixed0 Spring1 Cockpit2 Helicopter3 PhotoCamera4; enumerators 0x1413824e0.. stride 0x20 | confirmed |
| reference-point enum init | 0x1400568e0 | function | CenterWiper / PilotHead | confirmed |
| `GameCameraViews enum init` | 0x14002c180 | function | FollowFar0 FollowNear1 Helmet2 HelmetNoMesh3 Dash4 Fork5 LeftWheel6 Saddle7 Cockpit_01 8 Bubble9 PitStop10; Lua registration FUN_140126cf0 / FUN_14010a9d0 | confirmed |
| CameraData loader | 0x140182f90 | function | Lua keys FovMin/Max, Front*, offsets, CameraType | confirmed |
| `GameViewCameras.cfg loader` | 0x14017ed10 | function | opens TUNING:/Bikes/BASE/GameViewCameras.cfg; caller FUN_140125f70 | confirmed |
| Cockpit settings loader | 0x1401808a0 | function | editor registration FUN_140194210 | confirmed |
| Helicopter settings loader / editor | 0x140182c60 / 0x14018e420 | function | FovReduceRate, OrientationSmoothing, noise belong here, NOT to the helmet camera | confirmed |
| PhotoCamera settings loader | 0x140181500 | function | | confirmed |
| settings object creators | 0x14017fea0 (0x118 B), 0x14017ffc0 (0x1a0), 0x14017fd80 (0x170), 0x140180000 (0x50), 0x14017fed0 (0x50) | function | | confirmed |
| `FUN_140184060` | 0x140184060 | function | returns CameraData for the mode (+0x428) | confirmed |
| `FUN_140184780`, `FUN_140184800`, `FUN_140185b70`, `FUN_140186a10` | | function | CameraData helpers: TempSpecificCameraParameters (0x140184800), ParentCameraFile / SharedCameraSettings (0x140185b70/0x140186a10) | likely |
| `FUN_14017c900` | 0x14017c900 | function | likely SpeedShake via AddSmoothShake (call 0x14017cc20) | guess |
| `FUN_14019dc90` OnShift / `FUN_14019dc10` | 0x14019dc90 / 0x14019dc10 | function | subtract shift from mode+0x37c/+0x388, translate +0x3e4 | confirmed |
| `TTGameCameraViews` / `TTCameraModeMgr` | 0x140f241e0 (name) / FUN_14017a7e0, FUN_14017a8a0 (vtable ~0x140f2ef20) | function | live 'current view' field not located | guess |
| string 'TUNING:/Bikes/BASE/GameViewCameras.cfg' | 0x140f308a0 | string | | confirmed |
| string 'ARCHETYPES:/GAMEPLAY/HELMETVIEW.PPF' / 'HelmetView_%d' | 0x140f60c30 / 0x140f60c58 | string | parsed at log lines 749, 1369, 4981 | confirmed |
| strings Helmet*/Fov*/Noise (HelmetPositionOffset 0x140f30588, HelmetPitchAngleOffset 0x140f305a0, HelmetPitchSpringDamping 0x140f305b8, HelmetYawSpringMaxExtent 0x140f30650, ShowHelmet 0x140f30370, FovMin 0x140f30808, FovMax 0x140f30810, FovReduceRate 0x140f30838, LowNoiseIntensity 0x140f30848, HighNoiseIntensity 0x140f30860, OrientationSmoothing 0x140f30888, EnableFovCompensation 0x140f30a90, FovCompensationRatio 0x140f30aa8) | | string | | confirmed |
| strings front pass (EnableFrontPass 0x140f30a50, FrontFovMin 0x140f30a10, FrontFovMax 0x140f30a20, FrontNearClip 0x140f30a30, FrontFarClip 0x140f30a40) | | string | | confirmed |
| strings 'RPM Noise' 0x140f33a88, 'Speed Noise' 0x140f33a98, 'Speed Roll Noise' 0x140f33aa8, 'Accel Noise' 0x140f33ac0, 'Enable helmet mesh' 0x140f60d48 | | string | | confirmed |
| strings 'eCameraDefaultHelmet' 0x140f52ee8, 'eCameraDefaultDynamicViewOff' 0x140f52fe8 | | string | TTOptionsManager camera options (profile, not CVars) | confirmed |
| strings 'Engine.Camera' 0x141047d80, 'VerticalFov' 0x141047d90, 'HorizontalFov' 0x141047da0 | | string | CVars (1.25 / 0.75 rad defaults) registered in FUN_1400ba090 / FUN_1400ba1d0; no readers outside the init functions, effectively dead | likely |
| string 'Releasing camera mode ID %s' 0x141048080; camera debug format 0x1410481b0 | | string | not printed in the log | confirmed |
| `GameEngine::CreateAnselCamera` | 0x1404ad4d0 | function | see Ansel | confirmed |
| `GameEngine::GetAnselProjectionOffset` | 0x1404b36a0 | function | returns AnselCam+0x54 | confirmed |

### 5a. Graphics::Camera fields (GraphNode-derived)

| Our name | Offset | Kind | Notes | Confidence |
|---|---|---|---|---|
| GraphNode.parent | +0x20 | field | | confirmed |
| GraphNode.poolIndex | +0x50 (int) | field | index into the three global pools | confirmed |
| Camera.projType | +0x58 (int) | field | 0 perspective, else orthographic | confirmed |
| Camera.dofDatas | +0x5c..+0x74 | field | | confirmed |
| Camera.fov | +0x78 (float, radians) | field | **HORIZONTAL** fov: MatrixPerspectiveFovRH m[5]=m[0]*aspect. **CONFLICT**: the camera note labelled it vertical; the projection decompile, Ansel conversion (fovType 0 = horizontal) and KTUCamera::Update Hor+ correction support horizontal | confirmed (RC) |
| Camera.near | +0x7c | field | GetClipNear | confirmed |
| Camera.far | +0x80 | field | GetClipFar | confirmed |
| Camera.aspectMul | +0x84 | field | final aspect = (vpW/vpH) * this | confirmed |
| Camera.orthoWidth | +0x88 | field | | confirmed |
| Camera.frontFov | +0x8c | field | front pass; also radians | confirmed |
| Camera.frontNear / frontFar | +0x90 / +0x94 | field | | confirmed |
| Camera.frontPassEnable | +0x98 (byte) | field | from KTUCameraMode+0xe2 | confirmed |

### 5b. Camera helper structs

| Our name | Offset | Kind | Notes | Confidence |
|---|---|---|---|---|
| Game::Camera.active | +0x08 | field | | confirmed |
| Game::Camera.gfxCamera | +0x10 | field | Graphics::Camera* (StrongPtr) | confirmed |
| Game::Camera.manager / name | +0x20 / +0x28 | field | | confirmed |
| KTUCamera.mode | +0x48 | field | current KTUCameraMode* | confirmed |
| KTUCamera.overrideMode | +0x68 | field | FreeCam etc.; replaces matrix, FOV, clips | confirmed |
| KTUCamera.modeRequest / shakeList / rgbShift | +0x74 / +0x80 / +0x98 | field | | confirmed |
| KTUCamera.modeArray count / ptr | +0xa4 / +0xa8 | field | | confirmed |
| KTUCamera.transitionTimer | +0xb4 / +0xb8 | field | | confirmed |
| KTUCamera mode stack (HUD note) | +0x78 (count +0x74), name at mode+0x38 | field | **CONFLICT** with the camera note (+0x74 mode-request, +0xa8 mode array): two different structures both addressed by +0x74/+0x78; unresolved | likely (RC) |
| KTUCameraMode.matrix | +0x08 (Matrix34, to +0x38) | field | | confirmed |
| KTUCameraMode.fov / near / far | +0x80 (rad) / +0xa0 / +0xa4 | field | | confirmed |
| KTUCameraMode.frontFov / frontNear / frontFar | +0xa8 / +0xac / +0xb0 | field | | confirmed |
| KTUCameraMode.shakeEnable | +0xcc | field | | confirmed |
| KTUCameraMode.flags | +0xe1 / +0xe2 | field | +0xe2 -> Camera+0x98 | confirmed |
| TTSpringCameraMode.fovDeg / frontFovDeg | +0x12c / +0x130 | field | speed lerp of CameraData FovMin..FovMax | confirmed |
| TTSpringCameraMode.lookQuat | +0x2c0 | field | | likely |
| TTSpringCameraMode.owner / slot | +0x138 / +0x140 | field | | confirmed |
| TTSpringCameraMode.smoothed targets | +0x3d4 / +0x3e4 | field | +0x3e4 is the matrix copied into mode+8 for non-spring types | likely |
| TTSpringCameraMode.helmetMatrix | +0x4c4 | field | | likely |
| CameraData.name / FovMin / FovMax | +0x18 / +0x20 / +0x24 (deg) | field | | confirmed |
| CameraData.NearClip | +0x28 | field | inferred | guess |
| CameraData.FrontFovMin / Max / FrontNear / FrontFar | +0x2c / +0x30 / +0x34 / +0x38 | field | | confirmed |
| CameraData.PositionOffset Min/Med/Max, TargetOffset Min/Med/Max | +0x48..+0x60?, +0x78/+0x84/+0x90 | field | ordering inferred from string order | guess |
| CameraData.EnableFrontPass | +0x9d | field | | confirmed |
| CameraData.cameraType / ReferencePoint / settings ptr | +0xb0 (value +0xb8) / +0xc8 / +0xc0 | field | | likely |
| CameraData.TargetOffset / PositionOffset / EnableFovCompensation | +0xd8 / +0xe4 / +0x100 | field | | likely |
| CockpitSettings.HelmetPositionOffset | +0xe4 (vec3) | field | positions helmet/visor; whether it moves the eye is open | likely |
| CockpitSettings.SpeedShakeEnable / CollisionShockEnable | +0x18 / +0x40 | field | zero these (plus ClearShakes) to kill bob/shock | likely |
| CockpitSettings.Tuck* | +0x64..+0xb0 | field | thresholds read at FUN_1401a50e0 | likely |
| CockpitSettings.Lean* | +0xbc..+0xe0 | field | read at FUN_14019b880 | likely |
| CockpitSettings.HelmetPitchAngleOffset / pitch+yaw springs / damping | +0xf0 / +0xf4..+0x108 / +0x10c,+0x110 | field | | likely |
| AnselCam fields | see Ansel | field | | |

---

## 6. Temporal (TAA, last-frame data, motion vectors, exposure, froxels)

Headline (confirmed): all view-dependent temporal state lives in fixed 2-slot arrays indexed by `ViewContext+0x464` (camera index). `DeferredRenderLoop::Render` runs the whole post chain once per ViewContext. Mapping eye k to camera index k would give per-eye history without state swapping (hypothesis, RC). Engine has NO camera-cut or teleport history reset.

| Our name | Address | Kind | Notes | Confidence |
|---|---|---|---|---|
| `RenderLoop::InitializeTAA` | 0x1408dbd40 | function | (create, count): RT pair per slot; format 0x41424752 ('RGBA', likely R8G8B8A8 LDR, RC) | confirmed |
| `RenderLoop::InitializeLastFrameBuffers` | 0x1408db790 | function | 'Last Frame Depth' FL32, 'Last Frame Buffer' format 0x464c4834 (likely RGBA16F, RC), 8 mips | confirmed |
| `RenderLoop::InitializeMotionBlur` | 0x1408db950 | function | one shared MV RT at +0xca8 | confirmed |
| `RenderLoop::OnSplitChanged` | 0x1408dc2b0 | function | InitializeTAA(true,p) + InitializeLastFrameBuffers(p); the only history reset on resize/split change | confirmed |
| `RenderLoop::RenderTAA` | 0x1408ddf70 | function | resolve + ping-pong by pointer swap; at motion pause swaps at entry and uses stored +0x1028.. data | confirmed |
| `RenderLoop::NoMotionVectorFixTAA` | 0x1408dc0e0 | function | call at 0x1408de874; stencil-gated overwrite of history for pixels with no MV | likely (RC) |
| `RenderLoop::ClearTAA` | 0x1404f5b10 | function | sets reset flag +0xd08; NO in-binary callers | confirmed |
| `RenderLoop::PreUpdateLastFrameCamera` | 0x1408dc6b0 | function | (this, ShiftInfo*, idx): if the flag (ShiftInfo+0xc) is set, subtracts the vector from slot idx's +0xf58 and +0x1000 positions only (lastVP is camera-relative); clears nothing, so both views of a frame rebase their own slot once. Call 0x140848bf4 is inside the per-view TAA/post branch (`local_728 != 0`): a view without it is not rebased | confirmed (2026-10-21) |
| `RenderLoop::PostUpdateLastFrameCamera` | 0x1408dc2f0 (export `?PostUpdateLastFrameCamera@RenderLoop@Graphics@PlayAll@@IEAAXAEBVRenderContext@23@@Z`) | function | (this, RenderContext& = RenderLoop+0x1f00): for idx = ctx+0x594, unless s_bMotionPause: camera world = inverse(view at ctx+0x148); lastCamPos = its translation; lastVP = GetProjectionNoOffset(ctx+0x130) x rotation-only view; lastJitter = (ctx+0x578, ctx+0x57c) x (0.5, -0.5); front-pass set (+0xf80/+0x1000/+0x1018, from the ViewContext at +0x1590) if `this[0x1e70]` or (`this[0x111c]` and Camera+0x98). Call 0x140848c58 after TAA + motion blur. First readers in a view: `UpdateLastFrameCameraCB` (MV passes in RenderGBuffer, 0x140843549 / 0x1408435b4), RenderSSR, RenderTAA, RenderMotionBlur. Calling it right after `RenderContext::Prepare(view)` latches previous = current for that slot (camera-motion reset) | confirmed |
| CVar `CreateSSR` / `EnableRaymarchingSSRNextFrame` | 0x1413cbda0 / 0x1413cc2f0[idx] | global | `DeferredLighting::IsUsingRaymarchingSSRNextFrame(idx)` 0x1404fd620; with `WaterDrop::HasAliveParticles(idx)` or view flag [0x61e] they gate `CopyBuffersForNextFrame` | confirmed |
| DeferredLighting.ssrOn[idx] | +0x21+idx | byte | gate of `DeferredLighting::RenderSSR` 0x14083bd50; written at the end of `DeferredRenderLoop::Render` (near 0x1408497d7) from last frame's CVars. No history-valid check: after a cut SSR samples the pre-cut last-frame buffers | confirmed |
| CVar `UseFroxelLighting` | 0x1413cc6c0 | global | 0 skips `LightFroxel` (index unchanged) | confirmed |
| `RenderLoop::UpdateLastFrameCameraCB` | 0x1408df3c0 | function | uploads 0x50 B (matrix + pos) into shared CB cbVSCameraLastFrameStd (StringId 0x1414075b0); flag picks front-pass set | confirmed |
| `RenderLoop::GetLastFrameViewProj` | 0x1404f8890 | function | `this+idx*0x40+0xed8`; no callers | confirmed |
| `RenderLoop::GetLastFrameBuffer` / `GetLastFrameDepth` | 0x1408db290 / 0x1408db2a0 | function | only caller RenderSSR | confirmed |
| `RenderLoop::CopyBuffersForNextFrame` | 0x1408d9ee0 | function | call 0x14084867f; conditional (water drops / SSR flag); copies depth + HDR colour (pre-tonemap), 7 mip passes | confirmed |
| `RenderLoop::RenderMotionBlur` | 0x1408dd5e0 | function | per view after TAA if DAT_141407830[idx] and dt != 0; consumes lastVP[idx] | confirmed |
| `DeferredRenderLoop::Render` TAA call sites | Pre 0x140848bf4, TAA 0x140848c03, Post 0x140848c58, ResetProjectionOffset 0x140848c72, tonemap call 0x140848768 | const | | confirmed |
| `Renderer::DoUpdate` (jitter origin) | 0x1405addd0 | function | see 6a | confirmed |
| `s_eAntiAliasingMode` | 0x141407dc8 | global | CVar Engine.Renderer.AntiAliasing: 0 none, 1 FXAA, 2 FXAA_TAA, 3 TAA; TAA active iff 2 or 3 and RenderLoop+0x20 != 0 | confirmed |
| `IsUsingTAA` | 0x1404fd660 | function | `mode - 2 < 2` | confirmed |
| `g_jitterParity` | 0x1413a88c4 | global | frame parity counter; +1 per DoUpdate, held when dt == 0 | confirmed |
| `DrawPass::s_vResolutionScaling` | 0x1413602d8 (y at 0x1413602dc) | global | dynamic-res scale; written by FUN_1405b1270 (0x1405b16b1/0x1405b16c7); read by TAA, FillCSLightCB, LightFroxel, RenderSSR, SkyRenderer, IntelSSAO, luminance | confirmed |
| `DrawPass::s_bDynamicResolutionEnabled` | 0x1414082ea (export) | global | written only by the controller `FUN_1405b1270`; gates `UpscaleDynamicResolution`, `GetDrawPassViewport`, `PrepareRenderInputPostProcess`, `FUN_1409120b0` | confirmed |
| dynamic-resolution controller | `FUN_1405b1270` (only call 0x1405af618 in `Renderer::OnRender`, after DoUpdate, before the render loop) | function | returns early (scale untouched) if `GameEngine+0x185`; `GameContext+0x23` -> enabled = false; else enabled = (mode != 0); mode 0 writes the scale 1.0 / 1.0, mode 1 fixed `DAT_1413a7f40`, mode 2 GPU-time controller clamped to [`DAT_1413a80e0`, `DAT_1413a8010`] | confirmed |
| CVar DynamicResolutionMode | object 0x1413a7d80, live value 0x1413a7e08 (object+0x88) | global | `ICVar("DynamicResolutionMode", "Engine.Renderer.DynamicResolution", 3)` + LoadValue (UserSettings); the controller reads the live value, not the per-slot values | confirmed |
| dynamic-res controller (FUN_1405b1270) | 0x1405b1270 | function | 10-sample GPU-time ring, clamps [DAT_1413a80e0, DAT_1413a8010]; modes DAT_1413a7e08 / DAT_1413a8568 | likely |
| `Renderer::EnableDynamicResolution` / `SetDynamicResolutionRange` | 0x1403de740 / 0x1405b03d0 | function | | confirmed |
| `UpscaleDynamicResolution` | 0x1408df750 | function | see Render loop | confirmed |
| `RenderLoop::s_bMotionBlurEnable` | (CVar init FUN_14009f910) | global | Engine.Renderer.MotionBlur.Enable | confirmed |
| per-slot MB flag array | 0x141407830 | global | `DAT_141407830[idx]` | likely |
| `Mesh::UpdateRenderData` | 0x1408a3f00 | function | frameId-guarded prev<-cur latch: advances only if frameId != Mesh+0x70 and not motion-paused; idempotent for a second view in the same frame; if frameId != last+1 prev=cur. frameId = `RenderLayer+0x30`, read by the fill job FUN_1408d3370 (renderer.md 4) | confirmed |
| `SkinMesh::UpdateRenderData` | 0x140906b90 | function | per-camera-index last frameId/LOD (+0x158+idx*4, +0x160+idx*2) | confirmed |
| SkinMesh motion state | `+0x68` (one EntityRenderData per mesh: `+0x30` motion valid, `+0x31` reset), `+0xc0` last camera idx, `+0xc2` last LOD | field | **two views latch the shared prev palette twice** (prev == cur in both views; renderer.md 4) | confirmed |
| `SkinEntityRenderData::Fill` (FUN_1409062b0) / `SkinDrawCaller::RecordDraw` (FUN_140905f70) | 0x1409062b0 (vtbl 0x141004678 slot 1) / 0x140905f70 | function | ours; Fill: if +0x30 prev <- cur, cur <- pose, if +0x31 prev <- cur; RecordDraw copies the palettes into the cur / prev CBs at record time | confirmed |
| `HHParticlesManager::OnShift` | (World Shift fan-out) | function | rebases particles | confirmed |
| `SceneManager::ShiftOrigin` (Game / Graphics) | 0x1405dad20 / 0x1408ffec0 | function | | confirmed |
| string 'DATA:/ENGINE/EFFECTS/POSTPROCESS/TAA.PFX' | 0x140fff1c0 | string | | confirmed |
| strings 'TAA render target 0' / '1' / 'TAA resolve' / 'No motion Vector Fix TAA' | 0x140fff1f0 / 0x140fff208 / 0x140fff220 / 0x140fff230 | string | | confirmed |
| strings 'g_jitter' 0x140ffe980, 'g_TAAParams' 0x140ffe990, 'ForceNoTAA' 0x140ffe970 (data xref 0x1400a0b63, semantics unresolved), 'FXAA_TAA' 0x140ffe798 | | string | | confirmed |
| strings 'g_viewProjectionLastFrame' 0x140ffe908, 'g_cameraPositionLastFrame' 0x140ffe928, 'g_viewProjLastFrame' 0x140ffe9b8 | | string | | confirmed |
| StringIds: g_viewProjectionLastFrame | 0x141408018 | global | RenderMotionBlur | confirmed |
| StringIds: g_viewProjectionInv / g_cameraPositionLastFrame | 0x141408178 / 0x1414080c8 | global | | confirmed |
| StringIds: g_viewProjLastFrame / g_invViewProj / g_jitter / g_cameraPosition | 0x141408288 / 0x141408278 / 0x141408268 / 0x141408280 | global | set in RenderTAA | confirmed |
| StringIds: g_TAAParams / TAA tech / ForceNoTAA tech / LinearInput | 0x141408270 / 0x141408258 / 0x141408260 / 0x141408250 | global | | confirmed |
| strings 'Generate last frame mips' 0x140fff170, 'Last Frame Depth' 0x140fff190, 'Last Frame Buffer' 0x140fff1a8 | | string | | confirmed |
| strings 'Motion Vectors' 0x140fdc4a8, 'Motion Vectors front pass' 0x140fdc4b8, 'DATA:/ENGINE/EFFECTS/VERTEXSHADERS/MOTIONVECTORSEFFECT.PFX' 0x140ffc198, 'GenerateMotionVectors' 0x140f66088 | | string | | confirmed |
| strings cbVSCameraLastFrameStd 0x140f603a8, cbVSObjectInstancedMotionVector 0x140f603d0, cbVSObjectSkinnedLastFrame 0x140f60468 | | string | per-object prev transforms, shared once per frame | confirmed |
| strings 'csLightFroxelTemporal' 0x140fdb570 / 'csLightFroxelTemporalNoShadow' 0x140fdb588 | | string | | confirmed |
| ToneMapper strings: 'DATA:/ENGINE/EFFECTS/HDR.PFX' 0x1410093d8, 'HDR_LuminanceInit' 0x1410097c0, 'HDR_LuminanceDownscale' 0x1410097d8, 'HDR_LuminanceFinal' 0x1410097f0, 'HDR_FinalCompose' 0x141009818, 'Scene Current Luminance: %.4f, Exposure Key: %.4f' 0x141009698, 'Reset luminance' 0x141009490, 'g_ExposureLimits' 0x141009348, ACES LUT 'DATA:/Engine/Textures/ACES/Lut_ACES_sRGB_100nits.PTX' 0x141009530 | | string | SDR ACES only; no HDR10 output strings | confirmed |
| motion-blur strings: 'Engine.Renderer.MotionBlur' 0x140ffe7a8, 'ShutterTime' 0x140ffe7c8, 'TargetMs' 0x140ffe818, 'DATA:/ENGINE/EFFECTS/POSTPROCESS/MOTIONBLUR.PFX' 0x140fff0f8, 'Render MotionBlur' 0x140fff128 | | string | | confirmed |
| 'SpeedRadialBlur' 0x140f91d48, 'RGBShift' 0x140f91d60, DOF 'Data:/Engine/PostProcess/DofDownCoc.PMA' 0x140fff008 / 'DofFinal.PMA' 0x140fff030 | | string | | confirmed |
| `HHParticlesManager::UpdateEvolve` | (OnRender) | function | once per frame, view-independent | confirmed |

### 6a. TAA jitter (confirmed, important)

- NOT Halton. `Renderer::DoUpdate` alternates by `g_jitterParity`: even frames offset = (1/viewportW, 0), odd frames (0, 1/viewportH) (half pixel in NDC after the 0.5 factor in g_jitter).
- The Vector2 is the 3rd argument of `ViewContext::InitFromCamera`; it is added to proj[0][2] and proj[1][2] (float indices 2 and 6) and stored at ViewContext+0x448/0x44c.
- Gate: AA mode 2 or 3, physics not paused, no external override, and split-mode condition (see conflict). With an Ansel session active (Renderer+0x19) it is replaced by `GameEngine::GetAnselProjectionOffset`.
- Removed at end of each view by `ResetProjectionOffset(this+0x2030)` after PostUpdateLastFrameCamera; excluded from last-frame matrices by `GetProjectionNoOffset`.
- `g_jitter` (written only in RenderTAA) = (cur.x*0.5, cur.y*-0.5, prev.x*0.5, prev.y*-0.5); cur = RenderContext+0x578/0x57c, prev = RenderLoop+0xf70+idx*8.
- All other InitFromCamera callers (UpdateShift, ProbeRenderer, OnRender secondary views, Pick3dInList, RenderContext::Prepare) pass `V_VECTOR2_0`.
- Do NOT reuse this slot for persistent per-eye lens shift (it is stripped after TAA). Per-eye off-axis terms belong in proj[0][2]/[1][2] installed outside this slot.
- **CONFLICT** on split mode: stereo_native says the same offset goes to both split views; temporal and renderloop notes say jitter is zero/disabled when a second view exists. Use the DoUpdate decompile gate (`bVar8` requires split mode 0 or +0xc60 == 0): jitter off in split mode, (RC).

### 6b. RenderLoop temporal arrays (2 slots each, index = ViewContext+0x464)

| Our name | Offset | Stride | Notes | Confidence |
|---|---|---|---|---|
| RenderLoop.taaEffect | +0xcd8 | | TAA.PFX effect | confirmed |
| RenderLoop.taaTechId / noMvFixTechId | +0xce0 / +0xce4 | | | confirmed |
| RenderLoop.taaHistoryA[idx] | +0xce8 | 0x10 | RT read as 'LastFrameBuffer'; swapped with B each frame | confirmed (RC) |
| RenderLoop.taaOutputB[idx] | +0xcf0 | 0x10 | draw target; copied to PostProcessManager current RT then swapped with A | confirmed |
| RenderLoop.taaResetFlag | +0xd08 (byte) | | ONE shared flag for both slots; set in ctor (0x1408d9120) and ClearTAA; delivered as g_TAAParams.z; cleared at end of RenderTAA. Mod using idx=eye would reset only the first slot that runs | confirmed (RC) |
| RenderLoop.lastVP[idx] | +0xed8 | 0x40 | unjittered, camera-relative (NoOffset proj * view with translation zeroed) | confirmed |
| RenderLoop.lastCamPos[idx] | +0xf58 | 0xc | float3; rebased by PreUpdateLastFrameCamera | confirmed |
| RenderLoop.lastJitter[idx] | +0xf70 | 8 | jitter*(0.5,-0.5) | confirmed |
| RenderLoop.lastVPFront[idx] | +0xf80 | 0x40 | front-pass ViewContext (+0x1590) set | confirmed |
| RenderLoop.lastCamPosFront[idx] | +0x1000 | 0xc | | confirmed |
| RenderLoop.lastJitterFront[idx] | +0x1018 | 8 | | confirmed |
| RenderLoop.taaStoredVP[idx] | +0x1028 | 0x40 | RenderTAA copy, used under motion pause | likely |
| RenderLoop.taaStoredCamPos[idx] | +0x10a8 | 0xc | | likely |
| RenderLoop.taaStoredJitter[idx] | +0x10c0 | 8 | | likely |
| RenderLoop.prevResScale[idx] | +0x10d0 | 8 | previous s_vResolutionScaling; fed to g_TAAParams, SSR, froxel | confirmed |
| RenderLoop.lastFrameDepthRT[idx] | +0x10e0 | 8 | 'Last Frame Depth' R32F | confirmed |
| RenderLoop.lastFrameColorRT[idx] | +0x10f0 | 8 | 'Last Frame Buffer', HDR pre-tonemap, 8 mips | confirmed |
| RenderLoop.motionVectorRT | +0xca8 | | single shared RT, reused transiently per view | confirmed |

### 6c. Other per-slot temporal state

| Our name | Offset | Stride | Notes | Confidence |
|---|---|---|---|---|
| DeferredLighting.froxelPingPong[idx] | +0x780 | 4 | -1 = no history -> non-temporal technique ids +0x5d8/+0x5dc, then set to 0; else (old+1)&1 per frame, technique ids +0x5e0/+0x5e4. Writing -1 gives exactly one non-temporal frame (the stale volume is not read). Skipped frames (CVar `UseFroxelLighting` 0x1413cc6c0 off) leave it unchanged. No previous matrix in `FillCSLightCB` 0x140836ab0 (only the previous resolution scale): likely a same-cell blend | confirmed (index, 2026-10-21); likely (no reprojection) |
| DeferredLighting.froxelVolumes[pp][idx] | +0x760 + (pp*2+idx)*8 | | 4 Texture3D 'LightedFroxel' (concrete class `Texture3DDX11`, ID3D11Texture3D* at +0x68, likely from its Destroy `FUN_140955210` / GetDesc use); written this frame: pp = the new index, read as previous: (pp+1)&1 | confirmed (layout); likely (+0x68) |
| Last-frame, tonemap and MV render targets | RenderLoop+0x10e0/+0x10f0+idx*8, tm+0x110/+0x130/+0x138+idx*8, RenderLoop+0xca8 | | all `RenderTarget*` (StrongPtr from `RenderTargetManager::CreateRenderTarget`): D3D texture via vtable +0x40 -> Texture2DDX11+0x68. Getters exported: `GetLastFrameBuffer` 0x1408db290, `GetLastFrameDepth` 0x1408db2a0, `GetMotionVectorsRenderTarget` 0x1404f89e0 | confirmed |
| DeferredLighting.froxelAccumulation | +0x890 | | single shared accumulation volume (GetFroxelScatteringAccumulation) | likely |
| DeferredLighting.froxelNoiseTime[idx] | +0x920 | 4 | GetFroxelNoiseTime 0x1408390a0; accumulates Wind::GetStrength | confirmed |
| `DeferredLighting::GetFroxelLightedVolume` | 0x1404f8550 | | | confirmed |
| Tonemap.params[idx] | +0x30 | 0x60 | key, bias +0x34, speed +0x38, mode +0x50 | likely |
| Tonemap.currentLum | +0x130 | | 1x1 RT, ping-pong with previous | confirmed |
| Tonemap.prevLum[idx] | +0x138 | 8 | bound as texture slot 3 in HDR_LuminanceFinal | confirmed |
| Tonemap.cpuLuminance | +0x148 | | single shared float (debug text, clamp FUN_140914500) | confirmed |
| Tonemap.firstFrame[idx] | +0x14e | 1 | set for both slots by init `FUN_1409126c0` and the debug "Reset luminance" callback 0x140914dd0 (`mov word [rcx+0x14e], 0x101`); cleared at the end of every `FUN_140914680` call for its idx; when set, `HDR_LuminanceFinal` gets history weight 0 (snap). No engine path sets it on level load or restart | confirmed (writers, readers); likely (shader meaning) |
| Tonemap.lumaChainEnable | +0x150 | | gate for FUN_1409120b0; Ansel capture clears it | guess |
| Tonemap staging | +0x128 | | staging copy mapped to CPU | confirmed |
| LensFlareInstance.fade[idx] / node[idx] / queries | +0x84+idx*4 / +0x90+idx*8 | | per camera index. 2026-10-21: +0x90+idx*8 is the per-camera flare HudNode that `FUN_140898a40` adds to (alpha > 0) or removes from that camera's HudItemList 2D root (`HudItemList+0x18`); earlier reading "+0x64 fade, +0x90 queries" superseded. The occlusion query and 'Lens flares render' draws come from `LensFlareManager::Render` 0x14089a3a0 (RT, RenderContext, camIdx) | confirmed |
| CSM per-camera arrays | CascadedShadowMapRenderer+0x25b0 | 0x11a0 | static shadow cache per camera index | confirmed |
| Mesh.curWorld / prevWorld / lastFrameId | +0xc4 / +0xf4 / +0x70 | | frame-id guarded; prev may be stale (old coordinates) for one frame at a World Shift | confirmed / guess for stale |

**World Shift (confirmed (RC))**: `UpdateShift` fans out to Physics/Scene/Terrain/GenericInstanceSpawner/HHParticles/PublicManager/World OnShift, rebuilds each ViewContext with zero jitter and sets Renderer+0xd1c..0xd28. Render-side fix-up is ONLY `PreUpdateLastFrameCamera` (stored camera positions). lastVP, TAA/SSR/froxel/exposure history and per-object prev transforms are not rebased (matrices are camera-relative so none is needed). Any per-eye copy must receive the same subtraction.

---

## 7. HUD, UI and ImGui

Pipeline (confirmed): one `HudRenderer` (RenderLoop+0x410) is called with flag bits 8 = 3D HUD before post (into HDR scene RT, after deferred transparency), 4 = 3D HUD after post (after TAA, MB, lens flare; stencil cleared), 2 = 2D HUD, 0x10 = no depth test for 3D, 0x20 = 2D after upscale. Deferred calls use flags 8, 0x14, 0x22 (per-view 2D list), then 0x22 for the global/split list into the final RT. Forward calls 4 then 2.

| Our name | Address | Kind | Notes | Confidence |
|---|---|---|---|---|
| `HudRenderer::Render` | 0x14088b530 | function | (RT*, HudItemList*, RenderLoop*, RenderContext*, CameraRenderDesc*, flags, bool); passes 'HUD3D Renderer', 'HUD3D Renderer Frontpass', 'HUD2D Renderer'; 3D uses ViewContext at RenderContext+0x130 | confirmed |
| `RenderLoop::GetHudRenderer` | 0x140283a00 | function | | likely |
| `HudRenderer::Render` export | `?Render@HudRenderer@Graphics@PlayAll@@QEAAXPEAVRenderTarget@23@PEAVHudItemList@23@PEAVRenderLoop@23@AEAVRenderContext@23@PEBUCameraRenderDesc@23@H_N@Z` | export | callers: `DeferredRenderLoop::Render` (0x14084861a, 0x140848ece, 0x140848f84, 0x140848fcc, global list 0x14084953a), `ForwardRenderLoop::Render` (3), `TextureBaker::Render` | confirmed |
| DeferredRenderLoop global 2D HUD call | 0x14084953a | const | `Render(RenderLoop+0x410, [rsp+0x70] final RT, [RenderDesc+0xc50], RenderLoop, RenderLoop+0x1f00, nullptr, 0x22, false)`; null list skips it (0x140849512) | confirmed |
| HudRenderer.hudSize | +0x4 / +0x8 | field | 2D layout width / height override; -1 = `HudNode::GetTargetInfo` | confirmed |
| `HudRenderer::SetHudSize` | 0x14088c320 | function | `?SetHudSize@HudRenderer@Graphics@PlayAll@@QEAAXHH@Z`; writes +4/+8 | confirmed |
| `HudNode::GetTargetInfo` | 0x140881ae0 | function | static; 1920x1080 layout widened to the current window's aspect (`Window+0x30/+0x34`), triple-screen aware | confirmed |
| `GameContext::SetHudSize` | 0x14048cb00 | function | `?SetHudSize@GameContext@Game@PlayAll@@QEAAXHH@Z`; forwards to the render loop's HudRenderer; caller `FUN_1409eca00` | likely |
| `TextureBaker::Render` / `Create` | 0x14090dd10 / 0x14090d190 | function | renders a HudItemList into 'TextureBakeSRGB' (sRGB RGBA engine RT) with its own HudRenderer: `SetHudSize` + `Render(..., 0x12, false)` | confirmed |
| `RenderTargetManager::CreateRenderTarget` | 0x1408e1a30 | function | `?CreateRenderTarget@RenderTargetManager@Graphics@PlayAll@@QEAAHAEAV?$StrongPtr@VRenderTarget@Graphics@PlayAll@@@Core@3@AEBUDesc@RenderTarget@23@@Z`; returns 0 on success | confirmed |
| `RenderTarget::Desc` | `Desc::Desc` 0x1408e13c0 | struct | 0x88 bytes: +0x30 count, +0x34 w, +0x38 h, +0x40 mips, +0x44 window ratio, +0x48 format[4] ('RGBA' 0x41424752), +0x5c depth format, +0x60 sRGB per target, +0x68 resize with window, +0x70 window, +0x78 name (`frame-anatomy.md` 8.6) | confirmed (+0x64: guess) |
| `RenderTarget::GetTexture2D` | 0x1408e16d0 | function | `this+0x30+i*8`, type-checked | confirmed |
| `Oculus::GetCurrentHUDRenderTarget` | 0x140702870 | function | data refs only (dead path) | confirmed |
| `HudItemList::Get3DBeforePostProcessList` / `Get3DList` / `Add3DList` | 0x1408797c0 / 0x140879820 / 0x140879730 | function | array at HudItemList+0x1b8, flag +0x1d8; error 'doesn't support 3D hud, are you trying to add hud3D in Camera -1 ?' | confirmed |
| `Renderer::GetHudItemList` | 0x1403de850 | function | `Renderer+0xdf8+idx*8` | confirmed |
| HudItemList layout (size 0x1e0, ctor 0x140878e70) | +0x18 2D root HudNode, +0xe8 second HudNode, +0x1b8 `Array<HudList3D*>` drawn with flag 4 / 0x14 (after post), +0x1c8 `Array<HudList3D*>` drawn with flag 8 (before post, so inside the TAA history), +0x1d8 supports-3D byte (both camera lists: true, `Renderer::Create` 0x1405ace80) | struct | `Add3DList` appends to +0x1c8 if `HudList3D+0x151 == 1`, else +0x1b8; raw pointers, no refcount; `Array` = {+4 count, +8 data}. A HudList3D in two lists is safe: `HudRenderer::Render` rebuilds node layout per call and draws with the RenderContext's own ViewContext (+0x130) | confirmed (2026-10-21) |
| `LensFlareManager::Update` | 0x14089a770 | function | call 0x1405ae3a7 in `DoUpdate`, once per camera bit of `Renderer+0xd18`: (ViewContext, camIdx, that camera's HudItemList, dt) -> `LensFlareInstance::Update` 0x140898df0 -> `FUN_140898a40` (add/remove the camera's flare HudNode in the list's 2D root). The flare sprites are drawn by `HudRenderer::Render` flag 0x22 from `ViewContext+0x5e8`: the mod's HUD-list swap (view 1 draws list 0) shows eye 0's flares in eye 1 | confirmed |
| `MeshPainter::Update` | 0x1408a7920 (call 0x140847cff, per view) | function | livery editor (singleton 0x141405600; RTs 'CarPainterRenderTarget' etc., `VehicleLivery.PFX`); body only if `this+0x38` (set while painting, nulled by `Stop` 0x1408a74d0); latches brush prev <- cur, consumes the stroke on the first view. Not a temporal effect; a second view paints nothing | likely (the +0x38 writer not found) |
| `GameContext::GetHudItemList` | 0x14048b4c0 | function | -1 -> GameContext+0x88 (global list) | confirmed |
| `GuiManager::CreateDefaultMaterials` | 0x140888ef0 | function | loads QUADTEXTURECOLOR3D.PFX (0x140fe9f00); error string 0x140fe9f30 | confirmed |
| `TTDashboardHUD::Create` (FUN_140202260) | 0x140202260 | function | Flash gauges (J_HUD_AnalogGauge, TXT_KMPH, TXT_Speed); registers a HudList3D via Add3DList: world-placed 3D HUD, gets per-eye ViewContext | likely (RC) |
| `TTHotSpot3D` | 0x140205930 / 0x140205ad0 | function | 3D hotspot | likely |
| TT2 source TUs (strings): tthud.cpp 0x140f3ee78, ttdashboardhud.cpp 0x140f3e270, tthotspot3d.cpp 0x140f3e5e8, hudlist3d.h 0x140f79d50, hudrenderer.cpp 0x140fe9fb0, flashrenderer.cpp 0x140fdf0c0 | | string | strings 'Engine.Renderer.Flash' 0x140fdf098, 'DATA:/GUI/FLASH/MAINMENU/MAINMENU.FLS' 0x140f42770, 'CockpitHud' 0x140f3c7f8, 'CockpitHudScale' 0x140f3c868 | confirmed |
| `KTUGuiManager::PauseGame` | 0x140a030c0 | function | sets +0xe8, pushes time factor 0.0 | confirmed |
| `KTUGuiManager::ShowHud` | 0x140a03720 | function | | confirmed |
| `GameContext::PushTimeFactor` / `PopTimeFactor` / Set | 0x14048bf10 / 0x14048bed0 / 0x14048cb40 | function | | confirmed |
| `GameContext::GetLastDeltaTime` | 0x14048b650 | function | clock+0x30 * ctx+0x60 * ctx+0x48 | confirmed |
| `GetCurrentMenu` / `GetBlockInGameMenu` | 0x1404b3df0 / (n/a) | function | KTUGuiManager+0xf0 / +0x361 | confirmed |
| `Renderer::IsPlayingMovie` | 0x1405af290 | function | | confirmed |
| `CutSceneManager::IsPlaying` | 0x140459830 | function | (+0xdd or +0xde) | confirmed |
| `LevelLoader::IsLoading` / `Level::IsLoading` | 0x1404bb020 / 0x1403d51c0 | function | (+0x44, +0x58) / (+0xfd) | confirmed |
| `TTReplayManager` update / debug | 0x140354f20 / 0x140352a90 | function | state int +0xa0 (0 idle, 1 record, 2 replay); instance likely TTGameEngine+0x770 | likely |
| `TTOptionsState Validate` (ttoptionsstate.cpp) | 0x140308dd0 | function | SettingsManager::Apply, SaveUserSettings, ProfileBase::AutoSave | confirmed |
| `RenderWindowWin32::DefaultWndProc` | 0x1407097c0 | function | game WndProc; `RegisterWindowClass` 0x14070a1a0, `CreateRenderWindow` 0x1407096d0 | confirmed |
| `Graphics::ImGuiNewFrame` / `ImGuiInit` / `ImGuiRenderDrawLists` | 0x140923440 / 0x140922590 / 0x1409239e0 | function | draws pass 'ImGui' onto the window back buffer AFTER scene and HUD, before flip; render-drawlists thunk 0x1409239d0 | confirmed |
| `ImGuiWindowManager::Begin` / `PostUpdate` / `CreateInstance` | 0x140512bf0 / 0x140513c50 / 0x140513350 | function | docking (ImwWindowManager::Update 0x1407f4170, Paint 0x1407f0e20) | confirmed |
| `ImGui::CreateContext` / `GetCurrentContext` | 0x1407a0220 / 0x1407a3b60 | function | old (1.4x-era) ImGui, statically linked and EXPORTED (345 ImGui:: names). Our overlay must use its own module-local context, IniFilename=NULL | confirmed |
| `MenuDebug::SetEnable` | 0x140767f20 | function | | confirmed |
| `MenuDebug::Create` | 0x1407645b0 | function | forces enable byte 0 | confirmed |
| MenuDebug enable / 'Use ImGui' | 0x141358540 / 0x141358541 | global | 'Use ImGui' default 1 | confirmed |
| `GameContextDebug::UpdateInput` | 0x14048e5c0 | function | ACTION_TOGGLE_MENU_DEBUG (0x14139aa20), ACTION_TOGGLE_PAD_MENU_DEBUG (0x14139aa28), ACTION_TOGGLE_PROFILER_WINDOW (0x14139aa18), ACTION_TOGGLE_STATS_WINDOW (0x14139aa10) | confirmed (RC) |
| `GameContextDebug::Create` | 0x14048d8f0 | function | Lua Engine.AllowMenuDebug -> +0x31 (default false), AllowLoadingInfoDebug -> +0x30, +0x32 default 1 | confirmed |
| imguiimpl.cpp TU | 0x14100dee0 | string | 'COMMON:/ENGINE/FONTS/Ubuntu-M.ttf' 0x14100df48, 'DATA:/ENGINE/Effects/Imgui.PFX' 0x14100df70; imguimenudebug.cpp 0x14100e640 ('CVar's' 0x14100e6d8, 'Camera Apply Option' 0x14100e6f0); imwindow.cpp 0x140fce2a0; imguiwindowmanager.cpp 0x140f86340; 'dear imgui, %s' 0x140fc4900 | string | | confirmed |
| `Camera Apply Option` (FUN_1409253e0) | 0x1409253e0 | function | per-camera-index CVar selector (flag 0x20) for split screen, NOT a camera override | confirmed |
| `imgui.ini` (game root) | file | const | 49 bytes `[Debug] Pos=60,60 Size=400,400`; written by the game's ImGui at the same time as the log (mtimes differ between notes: 13:04 vs 17:47) | confirmed (origin: likely) |
| GameContext.timeFactor | +0x48 (stack +0x50/+0x54) | field | 0.0 while paused | confirmed |
| KTUGuiManager fields | +0x08 hud hide counter, +0xe8 paused, +0xf0 current menu, +0x361 block in-game menu | field | | confirmed |
| GameCameraViews / menu-state strings | ttreplaystate.cpp 0x140f49f00, ttpausescreen.cpp 0x140f49230; log 'MenuState : PUSH TTxxxState' | string | | confirmed |

---

## 8. Ansel

Ansel is wired but inert unless the NVIDIA driver reports availability. The engine's own camera override path, not a hook anchor for HUD-less capture.

| Our name | Address | Kind | Notes | Confidence |
|---|---|---|---|---|
| ansel::setConfiguration call | 0x1405adba0 | const | in `Renderer::Create` | confirmed |
| Ansel Configuration struct (stack RBP+0x30) | Renderer::Create | field | right=(1,0,0) up=(0,0,1) forward=(0,1,0) (RH, Z-up, Y-forward); translationalSpeed 5.0 (+0x24), rotationalSpeed 45.0 (+0x28), captureLatency 0, settleLatency 1, metersInWorldUnit 1.0 (+0x34), four support bools 1 (+0x38..+0x3b: off-centre projection, translation, rotation, fov), title UTF-8 (+0x40, GameEngine vslot +0x398), +0x48 dword 0 (fovType candidate; 0 = horizontal), userPointer = Renderer (+0x50), HWND (+0x58), callbacks start/stop session FUN_1405b0d30 (+0x60) / FUN_1405b1100 (+0x68), start/stop capture LAB_1405b0b20 (+0x70) / LAB_1405b0f60 (+0x78), byte +0x80 = 1, +0x81 = 0, sdkVersion 0x100056e7c9960 = SDK 1.5 (+0x88) | confirmed values; field order/fovType likely (header unverified) |
| `AnselCam::vftable` | 0x140f672b0 | vtable | slot 0 FUN_1403f14c0 (Create + clips 0.1/1000), slot 1 FUN_1403f1a10, slot 10 (+0x50) FUN_1403f15e0 | confirmed |
| `AnselCam::UpdateFromAnsel` | 0x1403f1a10 | function | engine camera -> ansel::updateCamera (call 0x1403f1c37) -> back; fov deg*57.29578 / rad*0.017453292 | confirmed |
| `AnselCam::ApplyTransform` | 0x1403f15e0 | function | clamp to 10-unit sphere, physics raycast, GraphNode::SetWorldMatrix | confirmed |
| `AnselCam::ctor` | 0x1403f1210 | function | vftable 0x140f672b0, +0x5c=10.0, +0x60=1.333, +0x64/+0x65=1 | confirmed |
| `GameEngine::CreateAnselCamera` | 0x1404ad4d0 | function | stores at GameEngine+0x60; 'AnselCam' string 0x140f7db38 | confirmed |
| `GameEngine::StartAnselSession` | 0x1404d4dd0 | function | copies active camera into AnselCam, SetActiveCamera | confirmed |
| `Renderer::StartAnselSession` / `StopAnselSession` | 0x1405b0b30 / 0x1405b0f70 | function | disable Vignette/SpeedRadialBlur/Dof/RGBShift, force motion blur off, zero dynamic resolution (CVar DAT_1413a7e08), restore on stop | confirmed |
| `Renderer::StartAnselCapture` / `StopAnselCapture` | 0x1405b0950 / 0x1405b0e80 | function | SSAO quality 8 / SSAO CVar 2; captureType == 2 switches AA away from TAA; clears byte +0x150 of object at renderloop+0x1ed0 | confirmed |
| session callbacks start / stop | 0x1405b0d30 / 0x1405b1100 | function | start calls PauseApplication(true) (vslot +0x148) if isPauseAllowed; disasm 0x1405b0e2f..0x1405b0e3c | confirmed |
| `ansel::markBufferBind` wrapper | 0x140948b50 | function | call 0x140948dca, IAT 0x140ec8030; mangled `?markBufferBind@ansel@@YAXW4BufferType@1@W4HintType@1@_K@Z`; game always passes (0, 0, -1) when DrawPassDesc+0x4b4 & 0x10 and isAnselAvailable. BufferType/HintType numerics unverified | confirmed (enum values: guess) |
| `PlayAll::Game::Renderer` Ansel globals | 0x141407dc8 AA mode, 0x1413a7e08 dynamic-res mode | global | | likely |
| string 'UseAnsel' / 'Engine.Renderer' | 0x140f776d8 / 0x140f776c8 | string | CVar Engine.Renderer.UseAnsel flags 0x5 | confirmed |
| string 'Ansel could not be enabled' | 0x140f91c20 | string | not in log | confirmed |
| string 'AnselSDK64.dll' | 0x14134f878 | string | static import: isAnselAvailable, setConfiguration, updateCamera, markBufferBind (+2) | confirmed |
| ansel::Camera layout (inferred) | FUN_1403f1a10 stack | field | position float3 +0, quaternion xyzw (w last) +0x0c, fov deg +0x1c, projectionOffset X/Y +0x20/+0x24 | likely |
| Ansel camera rotation adapter | 0x1403f1a8b..0x1403f1ac5 | const | q_ansel = q_engine (x) q11 with q11 = RotationVectors((0,0,1)->(0,1,0)); return path uses inverse. Cross terms reversed vs Hamilton P*Q; hand-check with a captured matrix | likely (RC) |
| AnselCam.gfxCamera / name / center / projOffset / radius / flags | +0x10 / +0x28 / +0x48 / +0x54 / +0x5c / +0x64,+0x65 | field | | confirmed |
| `GraphNode::SetWorldMatrix` | 0x14086ecd0 | function | single write point of the free camera | confirmed |
| `AnselCam world-matrix pool` | 0x1413cda88 | global | | confirmed |
| `HighResolutionScreenshot` | 0x140873da0 / 0x140874710 | function | tiled grid ('Grid size: %i' 0x140fe5980; TAA screenshot string 0x140fe5a20); source file 0x140fe5910 | confirmed |
| deferred RL Ansel gate | DeferredRenderLoop+0x1ed0 object, bytes +0x14c / +0x150 | field | meaning unknown | guess |

---

## 9. Split screen (the engine's native multi-view path)

Split mode 1 = vertical (side by side), 2 = horizontal (top/bottom), 3 = 4 views, 4 = other 2-view. Live code; two cameras, two ViewContexts, per-view culling/CSM/TAA history, all in ONE `Render` call. Blocked in network sessions; gated by Lua `PossibleSplitMode`. Renderer-only activation without the gameplay side (Phase 4 deep-dive, 2026-10-16): `frame-anatomy.md` 5.1.

| Our name | Address | Kind | Notes | Confidence |
|---|---|---|---|---|
| `GameEngine::SetSplitMode` | 0x1404d4710 | function | stores GameEngine+0x178; calls RenderTargetManager::SetPossibleSplitMode and EffectorMapManager::SetSplitMode | confirmed |
| `GameEngine::SetSplitModeOn` | 0x1404d4760 | function | refuses in network sessions ("can't use SetSplitMode( true ) in a network session.") | confirmed |
| `GameEngine::SwitchSplit` | 0x1403d7c30 | function | Lua binding FUN_1409ecf50: toggles to mode 1, or 2 if already 1 | confirmed |
| `IsSplitVertical` Lua binding | 0x1409ebbd0 | function | mode == 1 | confirmed |
| GUI Lua binding table | 0x14104a620 | global | name/function pairs incl. SwitchSplit, IsSplitVertical | confirmed |
| `RenderTargetManager::SetSplitMode` | 0x1408e2090 | function | rejects any mode other than the 'possible' one; string 0x141000460 | confirmed |
| `RenderTargetManager::SetPossibleSplitMode` | 0x1408e2080 | function | (+8 allowed, recreates split RTs) | confirmed |
| `RenderTargetManager::ComputeSplitSize` | 0x1408e1960 | function | mode 1 halves width; **no callers** (data ref only), patching it does nothing | confirmed |
| `RenderTargetManager::TransformViewport` | 0x1408e20e0 | function | mode 2 halves height, mode 1 halves width | confirmed |
| `RenderTargetManager::CreateSplitRenderTarget` | 0x1408e1ca0 | function | 'Split' RT | confirmed |
| `RenderLoop::CopySplitRT` | 0x1408da340 | function | vslot 10 (+0x50), call 0x1408492db; queues `RenderCopyTexture` of the view's final RT into the back-buffer RT sub-rect (record time, not executed yet); bool arg 'view 0' clears the destination; 'Split border' pass afterwards if `DeferredRenderLoop+0x1e71 == 1` | confirmed |
| `RenderTargetManager::s_pSingletonInstance` | 0x141408500 | global | read directly by DoUpdate; `GetInstance` 0x140450e80 | confirmed |
| `RenderTargetManager::ChangeRenderTargetsSize` | 0x1408e1800 / 0x1408e1760 | function | window RT sizing; halves width when RTM+0xc == 1 and RT Desc+0x68 (split-resizable, default 1) | confirmed |
| `EffectorMapManager::s_pSingletonInstance` | 0x1413ccd28 | global | vegetation bend maps, 2 slots (+8, +0x10), 256x256, render-only | confirmed |
| `EffectorMapManager::SetSplitMode` | 0x14084e6c0 | function | slot 0 only for mode 0, both slots otherwise; called by DoUpdate on an RTM mode mismatch (0x1405adf71) | confirmed |
| `EffectorMapManager::SetCenterWorldPos` | 0x14084e650 | function | (em, &pos, idx); slot 1's centre is set only for a second game camera (FUN_140b455a0) | confirmed |
| `EffectorMapManager::GetConstantBufferData` | 0x14084e000 | function | (idx), per view at 0x140847867, no null check: index 1 with no slot 1 crashes | confirmed |
| Renderer.scenes[2] | +0xd80 / +0xd88 | field | per-view `Scene*`; DoUpdate copies lighting/fog and VC+0x598 from it; after a level load only [0] is the level scene | confirmed |
| ViewContext.viewType | +0x610 | field | int; non-zero only on the external-scene path; jitter gate reads it for views 0/1 (Renderer+0x640 / +0xc60) | confirmed |
| ViewContext.virtualViewport | +0x5f4 | field | (x, y, w, h) fractions; mode 1: view 0 (0,0,0.5,1), view 1 (0.5,0,0.5,1), written in DoUpdate | confirmed |
| DeferredRenderLoop.splitBorder | +0x1e71 | field | byte; 1 = draw 'Split border'; writer not found | likely |
| `GameEngine::SetSplitModeOn` side effects | 0x1404d4760 | function | FMOD listener count (`SetNum3DListeners` 0x140715150), GUI screens, `CVarManager::ChangeSplit` -> `s_bSplitEnabled` 0x1413ac808, `+0x184` readers (cloth sim, content density, FOV). Re-applied at level-load end (0x1404d54ff, 0x140521cbe, 0x1405248a7). Not render-only: never call from the mod | confirmed |
| RenderTargetManager fields | +8 possible mode, +0xc active mode | field | | confirmed |
| `Renderer::DoUpdate` split branches | iVar4 (+0xc90) == 1 or 2 | const | InitFromCamera(this+0x30, cam@+0xe48) and (this+0x650, cam@+0xe50) | confirmed |
| strings 'PossibleSplitMode' 0x140f7cd78, 'VERTICAL' 0x140f7cd90, 'HORIZONTAL' 0x140f7cda0 | | string | | confirmed |
| strings 'Split %u HUD ' 0x140f91af0, 'Split 0 Visualizer' 0x140f91b18, 'Split 1 Visualizer' 0x140f91b30, 'Solo Viewport' 0x140f91be8, 'Split border' 0x140fdcb38, 'Player1 DOF index : %i' 0x141048178 / 'Player2 ...' 0x141048190 | | string | | confirmed |
| per-slot history for 2 views | RenderLoop arrays (section 6b) | field | a 4-view mode would exceed the 2-slot arrays | confirmed |
| Hazards for per-eye reuse | | | ToneMapper/exposure, CSM fit, probes assume two views are two players (see conflicts) | likely (RC) |

---

## 10. Settings and CVars

| Our name | Address | Kind | Notes | Confidence |
|---|---|---|---|---|
| `GameEngine::LoadSettings` | 0x1404bbeb0 | function | executes COMMON:/SETTINGS/SETTINGS.cfg (absent) else obfuscated DUMMYS.DAT as Lua, then (unless `Budget::s_bBudgetPresetActive` 0x1413ac630 or param_2) `<GetUserSettingsPath>\UserSettings.cfg`, then CVarManager::InitManager. Called from CreateCoreManagers (0x1404adba0) | confirmed |
| `GameEngine::SaveUserSettings` | 0x1404d2a20 | function | writes CVarManager::SaveToString(true); only CVars with flag bits 0x1 and 0x2 serialise | confirmed |
| `GameEngine::GetUserSettingsPath` | 0x1404b9a70 | function | vslot 147; SHGetFolderPathW(CSIDL_PERSONAL=5) + `\My Games\` + GetName. On this machine `C:\Users\Balazs\OneDrive\Documents\My Games\TT Isle of Man 2\`, currently empty | confirmed |
| `CVarManager::InitManager` | 0x14074b010 | function | FUN_1407487a0 with metatable script 0x140fb4940 (auto-creating nested tables) | confirmed |
| `ICVar::LoadValue` | 0x14074b780 | function | applies base layer then user layer | confirmed |
| `ICVar::IsSerializable` | 0x14074b220 | function | | confirmed |
| `CVarManager::SaveToString` / `ChangeSplit` | (n/a) | function | split via per-CVar 'Split' sub-tables, not a plain CVar | likely |
| `SettingsManager::Apply` | 0x14077a120 | function | | confirmed |
| `SettingsManager::ParseSettings` | 0x14077b540 | function | TUNING:/OPTIONS/<name>.cfg | confirmed |
| `VirtualFile::EncryptDecrypt` | 0x140786f20 | function | static 32-byte XOR key 0x140fc0180 (pointer 0x141358638), offset-rotated; callers VirtualFile::Read 0x140787750 (0x1407877e3, 0x140787878), PhysicalFile::Write, MemoryFile::Write, FUN_1407807f0 | likely |
| `VirtualFile::OldEncryptDecrypt` | 0x1407871a0 | function | key 0x140fc0160 (pointer 0x141358630); FUN_140780c40 (call 0x140780cf7) | likely |
| `Budget preset loader` | 0x14077a190 | function | FUN_14077a190 | likely |
| CVar `Engine.Renderer.AntiAliasing` | init FUN_14009fb50, value 0x141407dc8 | global | flags 0x43; enum map `AntiAliasing::ms_EnumMap` 0x141407710 initialised by FUN_14009f630: off=0, FXAA=1, FXAA_TAA=2, TAA=3, 'The end'=4; clamp `CVarAttributes<AntiAliasing>::CheckAndSetValue` 0x1408d8c20 (string 0x140f91920) | confirmed |
| CVar `Engine.Renderer.MotionBlur.Enable` | init FUN_14009f910 | global | flags 0x63; also SampleCount, SampleDistance, SampleFactor, ShutterTime, TargetMs, BorderEpsilon | confirmed |
| CVar `Engine.Device.VSync` | init FUN_1400a70d0 (`VideoDevice::s_bVSync`) | global | flags 0x42; 'VSync Interval' string 0x14100c218 | confirmed |
| CVars `Engine.Device.ScreenMode/ResolutionWidth/ResolutionHeight/RefreshRate/ScreenIndex/Maximized/SmartVSync` | FUN_1400abf90 / 1400ac550 / 1400ac420 / 1400ac2c0 / 1400ac6b0 / 1400ac170 | global | flags 0x2 | confirmed |
| CVars `Engine.Renderer.DynamicResolution.*` | string 0x140f915b8 (mode enum 0x140f98a40) | global | Mode, ResolutionScale, Min/Max scale, OptimalFrameDuration_ms, ScalingAxis; 'Invalid dynamic resolution setting in settings.cfg : resetting to off' 0x140f91d70 | confirmed |
| CVar `Engine.Camera.HorizontalFov` / `VerticalFov` | init FUN_1400ba090 / FUN_1400ba1d0 | global | flags 0x3; defaults 0x3fa00000 (1.25) / 0x3f400000 (0.75); no readers | likely |
| CVar `Engine.Renderer.UseAnsel` | string 0x140f776d8 | global | flags 0x5 | confirmed |
| CVar `Engine.Renderer.SSAO.Quality` | | global | flags 0x43 | confirmed |
| CVars `Engine.Renderer.UseImmediateRenderPass` / `RenderJobCount` | 0x141407420 / 0x14140d220 | global | see Frame loop | confirmed |
| CVar RenderLoop `ForceNoTAA` | string 0x140ffe970 | string | semantics unresolved (CVar or shader define) | guess |
| options-menu loc keys: OPTIONS_VIDEO_ADVANCEDGRAPH_AA_MODE 0x140f12bc8, OPTIONS_VIDEO_GRAPHICS_VSYNC 0x140efd1d8, OPTIONS_VIDEO_GRAPHICS_RESOLUTION 0x140efd138, OPTIONS_VIDEO_ADVANCEDGRAPH_FXAA 0x140efd310, NEW_OPTIONS_MOTIONBLUR_TIPS 0x140f1c9e8, NEW_OPTIONS_DYNAMICRESOLUTION_TIPS 0x140f1caa0, NEW_OPTIONS_SHARPEN_TIPS 0x140f1c9b0 | | string | | confirmed |
| strings COMMON:/SETTINGS/SETTINGS.cfg 0x140f7c768, DUMMYS.DAT 0x140f7c788, u'\UserSettings.cfg' 0x140f7c7c8, u'UserSettings.cfg' 0x140f7cd50, 'TUNING:/GraphicPresetsSwitch.cfg' 0x140f7d638 | | string | DUMMYS.DAT 13,073 B in `<game>\Common\Settings\` | confirmed |
| `platformsettings.cpp` | 0x140f8c880 | string | 'ShadowQuality' 0x140f8c978, 'CascadeShadowMapSize' 0x140f8c9a0, 'CascadeCount' 0x140f8c9b8 | confirmed |
| strings 'Loading settings' 0x140f7c728, 'PlayAll::Game::GameEngine::LoadSettings' 0x140f7c740 | | string | | confirmed |
| `UserSettings.cfg` override mechanism | | | writing a UserSettings.cfg should force e.g. AntiAliasing / resolution without touching game files; exact Lua syntax (dotted vs `CVars={}`) and enum form (string vs int) unconfirmed: change an option in-game and read the file back | likely (RC) |
| other cfgs | TUNING:/CAREER/CAREERSETTINGS.CFG, TUNING:/OPPONENTSSETTINGS.CFG, Tuning:/Dof.cfg (KTUDof::Create) | string | | confirmed |
| gameplay/camera/HUD/audio options | `ttoptionsmanager.cpp` 0x140f52430 -> profile (profilewin32.cpp 0x140fa7970, \GAMEDATA\), not CVars | string | | confirmed |
| Steam cloud save | `userdata\102865206\1082180\remote\Save.dat` (2,444,163 B) | const | options likely stored here | guess |

---

## 11. Platform: AntiCheatManager, loader, CrashRpt

Bottom line (likely, RC): the anti-tamper stack exists but is inert for our purposes; no consumer of its result was found. Offline: no crash, no exit, no leaderboard block originates from it. Proxy DLL and hooks should be safe; verify at runtime (thread census, no 'ACSearch' threads).

| Our name | Address | Kind | Notes | Confidence |
|---|---|---|---|---|
| `AntiCheatManager::CreateInstance` | 0x14073a410 | function | called at 0x1407416bd from CoreModule::Initialize only if CoreModule::Desc byte +0x8e == 1 (value unresolved) | confirmed (RC) |
| `AntiCheatManager_s_pSingletonInstance` | 0x1413ac628 | global | non-null at runtime means it is enabled | confirmed |
| `AntiCheatManager::SearchThread` | 0x1404d33e0 | function | only starter of the AC threads; NO code callers (data refs 0x1411ae15c, 0x14148a4b8) | likely (RC) |
| `IsCheatFound` / `GetCodeCrc` / `SetValidCodeSections` / `SearchCheat` / `GetInstance` | 0x1404bae50 / 0x1404b38d0 / 0x1404d4b40 / (n/a) / (n/a) | function | data refs only | likely |
| `AntiCheat_StartSearchThreads` | 0x1408003f0 | function | only caller of the three thread creators; only caller is SearchThread | confirmed |
| thread creators 'ACSearch' / 'ACCodeChanged' / 'ACSearchProcess' | 0x1407fe060 / 0x1408017e0 / 0x1408019e0 | function | | confirmed |
| thread procs | 0x140800e30 (ACSearch), 0x140800d30 (ACCodeChanged), 0x140800dd0 (ACSearchProcess) | function | 50 ms loops | confirmed |
| `AntiCheat_DebuggerCheck_flag1` | 0x1407ff320 | function | IsDebuggerPresent, CheckRemoteDebuggerPresent, NtQueryInformationProcess, DR0-3; vtable 0x140fcef18 | confirmed |
| `AntiCheat_HideFromDebuggerCheck_flag2` | 0x1407ff1f0 | function | NtSetInformationThread class 0x11 | confirmed |
| `AntiCheat_ApiPrologueCheck_flag4` | 0x1407fefc0 | function | first 16 bytes of kernel32!QueryPerformanceCounter (FUN_1408009a0). Do not hook QPC or ntdll debug APIs | confirmed |
| `AntiCheat_CodeCrcModuleScan_flag8` | 0x140801190 | function | PEB Ldr walk, CRC32 of .text per module, change detection; excluded names d3d11/dxgi/d3d12/d3d10/d3d9 (CRC32 42250f79 / eda87793 / 058575a9 / 7f4526c9 / 1523eb75) + unidentified 0x7a61f9fa, 0xdc5c0ba6, 0xeae4a31c, 0x7fdb43b0 | confirmed |
| `AntiCheat_ProcessScan_flag0x10` | 0x1407ff540 | function | K32EnumProcesses + VerQueryValue + cert subject CRC32; 'Cheat Engine' hash 0x1388962f | confirmed |
| `AntiCheat_CheckThreadStartFlag20` (ThreadCreated) | 0x1404d4ff0 | function | thread start address outside snapshotted .text sets flag 0x20 (unread) | confirmed |
| `tls_callback_2_ThreadAttach` | 0x1407fcb60 | function | TLS callbacks: 0x140e51c7c, 0x140e51b40, 0x1407fcb60 | confirmed |
| `AntiCheat_SnapshotValidCodeSections` | 0x140800730 | function | from CreateCoreManagers 0x1404adfc5/0x1404adfd1 only if CoreInitDesc+0x30 == 0 | confirmed |
| `AntiCheat_SetResultFlag_Obfuscated` | 0x140800530 | function | XOR table 0x141357810..0x141357850; also FUN_1407fedf0 | likely |
| hash-list registration | FUN_1407fde40 / FUN_1407fe010 / FUN_1407fdf70 | function | CRC32 lists, no server, no data files | confirmed |
| `FUN_140bee6c0`, FreezeAllThreadsExceptThis / ResumeAllThreads | 0x140bee6c0 | function | Toolhelp thread suspension, no direct callers; unknown | guess |
| `DBox_VerifyTrust` | 0x140e542b0 | function | WinVerifyTrust (call 0x140e54401); verifies ONLY dbxLive64.dll; subject blob 0x1410a46e8 = 'd-box technologies'; returns 0x2b1 | confirmed |
| `DBox_LoadDbxLive64` | 0x140e54030 / 0x140e54130 | function | builds `%sD-BOX\Gaming\LiveMotion\%s\%s` | confirmed |
| `SafeLoadSystem32Library` | 0x140001660 | function | LoadLibraryExW (0x1400016e4) of System32\<name>; rejects path separators; for nvapi64.dll / nvpowerapi.dll (FUN_140001120; helper 0x140001560) | confirmed |
| `XInput_Load` | 0x1409d05f0 | function | LoadLibraryA 'xinput1_3.dll' with 'UWP\xinput1_3.dll' fallback table 0x141362070; strings 'xinput1_3.dll' 0x141046f38, 'LoadXInput' 0x141047058 | confirmed |
| Steam wrapper loader | 0x140bda120 | function | LoadLibraryA('steam_api64.dll'); GetProcAddress strings 0x14107cae8..0x14107cc00 ('SteamAPI_Init' xref 0x140bda14f); no 'SteamAPI_RestartAppIfNecessary' | confirmed |
| other loaders | LoadLibraryA: FUN_1408048a0, FUN_1409d05f0, FUN_140a6dfa0, FUN_140a6e720 (x2: lua51.dll / lua5.1.dll / luaplus51-1200.dll, GetModuleHandle first), FUN_140bda120; LoadLibraryW: FUN_140bdb9f0, FUN_140bdbc70 (wintrust+crypt32) | function | bare-name loads, app dir first | confirmed |
| `CrashRpt_Install` | 0x1409f7550 | function | crInstall (Ordinal_8/19/24/30); skipped when a debugger is attached; 'PlayAllCrashRptCreation' error; dxdiag via FUN_1409f47b0 | confirmed |
| proxy candidates | version, winmm, dinput8, hid, dxgi, d3d11 | const | all static imports, none a KnownDLL, all load app-dir first. dinput8 (DirectInput8Create only) is trivial; d3d11 (D3D11CreateDeviceAndSwapChain only) is the best device hook; hid is high-breakage | confirmed |
| `InputDriverPC::Initialize` | (DirectInput8Create caller) | function | | confirmed |
| `FUN_140bdd560` | 0x140bdd560 | function | HidD_GetHidGuid | confirmed |
| `LibOVR` loader | see Dead code | | | |
| SteamStub | `.bind` section, entry 0x141577310 | const | do not modify; scan after stub runs | confirmed |

---

## 12. Dead code: Oculus and engine-native stereo

Verdict (confirmed): `PlayAll::Game::Oculus` and the `ViewContext` stereo functions are DEAD in this build. Nothing constructs `Oculus`; all xrefs to its methods are `.pdata` (0x1411xxxxx) or the export table (0x1414a9xxx). No LibOVRRT DLL ships. Do not plan on reviving it. Use it only as a reference for the intended data layout.

| Our name | Address | Kind | Notes | Confidence |
|---|---|---|---|---|
| `Oculus::vftable` | 0x140fa7890 | vtable | referenced only by the three ctors; slots Intrusive::Delete, FUN_1407020f0 (dtor), Intrusive::GetTypeInfo; class methods are non-virtual | confirmed |
| `Oculus::ctors` | 0x140701480 / 0x140701700 / 0x1407019b0 | function | no callers | confirmed |
| `Oculus::Create` | 0x140702230 | function | symmetrizes both eyes' ovrFovPort (per-field max) into this+0x220..0x22c, aspect +0x230; calls ovr_Initialize wrapper at 0x140702250 | confirmed |
| `Oculus::Update` | 0x140703520 | function | tracking state -> eye poses at this+0x1e8 (28 B per eye) | confirmed |
| `Oculus::Flip` | 0x140702670 | function | ovr_SubmitFrame via FUN_140bdc730; EyeFov layer + HUD Quad layer (z=-0.4, scale 0.001013 / 0.00146*0.28) | likely |
| `Oculus::GetEyePose` | 0x140702900 | function | quaternion+position -> Matrix34 | confirmed |
| `Oculus::GetFov` | 0x140702a30 | function | atan(a)+atan(b) of the symmetrized ports | confirmed |
| `Oculus::GetCurrentHUDRenderTarget` | 0x140702870 | function | HUD swapchain RT (index from FUN_140bdc3e0) | confirmed |
| `Oculus::GetCurrentWindow(int)` | 0x1407028b0 | function | per-eye window over LibOVR swapchain | confirmed |
| `Oculus::InitializeGraphicsResources` | 0x140702aa0 | function | 2 eye swapchains (this+0x1d0.., format 5) + HUD swapchain (this+0x1e0, width 0.7x eye width) | confirmed |
| `Oculus::RenderMirror` / `RecenterPose` | 0x140702ff0 / 0x140702fe0 | function | 'Oculus render mirror' string 0x140fa7908 | confirmed |
| oculus.cpp __FILE__ | 0x140fa78c0 | string | | confirmed |
| `LibOVR_proc_loader` | 0x140bdb060 | function | 58 GetProcAddress calls filling table 0x141427630..0x1414277f8 | confirmed |
| `g_ovr_fnptr_table` | 0x141427630 | global | ovr_Initialize 0x141427638, ovr_GetTrackingState 0x1414276a8, ovr_SubmitFrame 0x1414276d8, 'ovr_Lookup' 0x1414277f8, HMODULE 0x141427800 | confirmed |
| `LibOVR_load_library` | 0x140bdae10 | function | builds `%lsLibOVRRT%hs_%d.dll` (0x14107d170), honours LIBOVR_DLL_DIR (0x14107d158); uses LoadLibraryW FUN_140bdb9f0 / FUN_140bdbc70 | likely |
| `ovr_Initialize_wrapper` | 0x140bdc590 | function | only caller Oculus::Create | confirmed |
| LibOVR strings: 'ovr_Initialize' 0x14107d1c8, 'ovr_GetTrackingState' 0x14107d310, 'ovr_GetFovTextureSize' 0x14107d398, 'ovr_SubmitFrame' 0x14107d3b0, 'ovr_GetRenderDesc' 0x14107d3c0, 'ovr_CreateTextureSwapChainDX' 0x14107d4d8 | | string | | confirmed |
| `ViewContext::InitFromCameraStereo` | 0x14091b100 | function | sets +0x466=1; builds two view matrices from two Matrix34; ONE symmetric projection (Camera+0x78 fov, +0x7c, +0x80, aspect*+0x84) for both slots; no per-eye FOV; culling origin = camera position | confirmed |
| `ViewContext::SetViewProjectionStereo` | 0x14091b850 | function | | confirmed |
| `ViewContext::IsStereoscopic` | 0x1404fd5c0 | function | | confirmed |
| stereo byte readers | `DeferredRenderLoop::Render` (disables lighting path / lens flare when +0x466 != 0) | const | **CONFLICT**: stereo_native's disp32 0x466 scan found no reader outside ViewContext functions; the renderloop and hud notes report a read in DeferredRenderLoop::Render. No caller sets the flag, so it is moot while dead | confirmed that none sets it |
| no single-pass stereo | | | no SV_ViewportArrayIndex or eye-index strings; only DDERR_NOSTEREOHARDWARE and 'CFH L Eye' (KTUVoicePlayer, unrelated) | likely (RC) |
| External scene renderers | `Renderer::RegisterExternalSceneRenderer` 0x1405b01e0 | function | no callers (export only) | likely |

---

## Conflicts and open questions (for the reader of this file)

| # | Topic | Notes disagree | Better-supported reading | Needs |
|---|---|---|---|---|
| 1 | Camera+0x78 FOV type | camera note: vertical radians; projection_depth, conventions_ansel, temporal: horizontal | Horizontal. `MatrixPerspectiveFovRH` m[5]=m[0]*aspect; Ansel fovType 0 = horizontal; KTUCamera::Update applies Hor+ correction | RC: dump a live Camera and a captured projection (m[0] vs m[5]) |
| 2 | ViewContext+0x466 readers | stereo_native: only ViewContext functions read it; renderloop/hud: DeferredRenderLoop::Render reads it | Moot (nothing writes it); treat as one unresolved read | hardware read-watch |
| 3 | Jitter in split mode | stereo_native: same offset to both views; temporal/renderloop: disabled when second view exists | **Resolved 2026-10-16 for stereo_native**: disasm 0x1405adfc6 gate is `[+0x640]==0 && ([+0xc90]==0 \|\| [+0xc60]==0)` with +0xc60 = view 1's view type (0), so jitter is on in split mode, same vector for both views (`frame-anatomy.md` 5.1) | confirmed (static) |
| 4 | Auto-exposure state | renderloop: one object, no camera key, would double-step; temporal: per-idx ping-pong (tm+0x138+idx*8, first-frame flags tm+0x14e+idx, params tm+0x30+idx*0x60) | Per-idx (field-level evidence), plus a single shared CPU readback float +0x148 | RC: two views in one frame |
| 5 | RendererWin32 vftable | 0x140f7bda8 (threading) vs 0x140f7bdb0 (renderloop) | Slot numbering identical; 8-byte difference is probably RTTI locator vs slot 0 | none |
| 6 | RenderDesc viewport fractions | camera note +0x5f4..0x600 in slot; renderloop +0x624..0x630; stereo_native Renderer-relative floats [0xc44],[0xc4c],[0xc50] | Unresolved; use RenderDoc/debugger on split mode | RC |
| 7 | KTUCamera +0x74/+0x78 | camera note: +0x74 mode-request int, +0xa4/+0xa8 mode array; hud note: mode stack at +0x78 count +0x74 | Unresolved; structures may differ in meaning | RC |
| 8 | Renderer+0xc60 jitter gate | temporal cites Renderer+0xc60; renderloop says RenderDesc+0xc60 = Renderer+0xc90 | **Resolved 2026-10-16**: Renderer+0xc60 is ViewContext[1]+0x610 (view type), distinct from RenderDesc+0xc60 | confirmed (static) |
| 9 | Render thread | wf1: "cannot be excluded"; threading: none | None. VideoDevice+0x88 is set in VideoDevice::Create from the creating (main) thread | thread census |
| 10 | Early guesses superseded | "EnableVideoAdapter likely D3D11CreateDeviceAndSwapChain caller" (wf1 likely) | confirmed by call sites 0x140959180/0x1409591e9 | none |
| 11 | imgui.ini mtime | 13:04 (wf1) vs 17:47 (hud) on the same day | file rewritten again; ImGui is the game's own | none |
| 12 | Ansel Configuration fields | field order and fovType position unverified (SDK header unavailable) | values at the stated offsets are confirmed; names are not | SDK 1.5 header |

Open questions kept for later (no better reading yet): which CameraType/view is the real helmet camera; where `Mesh` frameId originates and whether it is identical for two eyes of one frame; whether froxel temporal reprojects with a matrix; main depth format; whether the 'ForceNoTAA' string is a CVar; who enables `AntiCheatManager` (Desc+0x8e); whether CSM cascades are fitted per view or merged; how shader-side matrices are packed (row_major vs column_major) for cbCameraStd.
