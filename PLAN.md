# PLAN.md — tt2vr

## Goal: v1 definition of done

v1 is done when all of these hold during a 30-minute offline session on the Snaefell Mountain Course:

- **Synchronous stereo:** both eyes are rendered from the same simulation frame, each with its own OpenXR view and asymmetric projection.
- **6DoF head tracking** drives the first-person (helmet) camera; the world stays locked when I move my head.
- **All temporal effects on**, each eye with its own history. The cross-eye leak test passes (see "Temporal design").
- HUD on a quad layer; menus usable in a theater quad.
- Comfort options: horizon lock (0–100% of bike roll), recenter.
- Holds the headset refresh rate at the resolution scale chosen in Phase 9.
- No crashes. If any signature fails, the game runs flat with an on-screen notice.

## Status

- [x] Phase 0 — Recon & setup
- [x] Phase 1 — Skeleton mod (flat)
- [x] Phase 2 — Camera control (flat)
- [x] Phase 3 — OpenXR, left eye head-tracked
- [x] Phase 4 — Dual-view stereo via the split-screen path (milestone)
- [ ] Phase 5 — Synchronous stereo: shared per-frame work
- [ ] Phase 6 — Temporal completeness
- [ ] Phase 7 — HUD, menus, desktop mirror
- [ ] Phase 8 — Bike comfort & camera
- [ ] Phase 9 — Performance
- [ ] Phase 10 — Robustness & release

**Active phase: Phase 5** (Phase 4 completed 2026-10-21; Phase 3 completed 2026-10-16; Phase 2 completed 2026-10-16; Phase 1 completed 2026-10-16; Phase 0 completed 2026-10-16)

---

## Frame model (target architecture, Phase 5 onward)

Rules for one simulation frame N:

1. The game thread runs simulation and the camera update **once**, producing base camera `C` (helmet camera, world space).
2. Our hook runs after the camera update and before scene rendering:
   - `xrWaitFrame` gives `predictedDisplayTime`; then `xrBeginFrame`; `xrLocateViews` gives eye poses `P_L`, `P_R` and FOVs `F_L`, `F_R`.
   - `C'` = `C` with horizon lock applied (Phase 8).
3. Per-frame passes run **exactly once**: shadow maps, GPU simulations, exposure update, probes. With the engine's two-view path (mechanism C) the engine already runs them once around its view loop; Phase 5 verifies each one.
4. For eye `E` in `[L, R]`:
   1. Bind `ViewState[E]`: history resources, previous matrices, jitter index.
   2. View = inverse(`C'` · `Recenter` · `P_E` converted to engine space). Projection built from `F_E` with the engine's depth convention.
   3. The engine renders view `E` as camera index `E` inside one `RenderLoop::Render` call (mechanism C), or we call the per-view render once per eye (fallback).
   4. Copy pre-HUD scene color into eye `E`'s swapchain image.
   5. Store this frame's matrices in `ViewState[E]` as "previous".
5. HUD renders once into a UI texture, submitted as a quad layer.
6. Present hook: `xrEndFrame` (projection layer + quad layer), then present the desktop mirror.

Phase 0: there is no separate render thread; steps 1–6 all run on the main thread inside `GameContext::RenderOneFrame` 0x14048bf80. Pass execution is spread over deferred contexts, so D3D hooks must cover them.

---

## Temporal design: per-eye history (required)

Temporal effects stay on. Each eye keeps its own history; view-independent state is computed once and shared.

### Invariants

- **T1.** When eye E renders, every "previous" input (history color, previous depth, previous view/projection, jitter state) is eye E's own previous value. Synchronous stereo: frame N−1. Alternate-eye: E's last rendered frame, N−2.
- **T2.** Nothing written during eye E's render is read as history by the other eye.
- **T3.** View-independent temporal state is updated exactly once per simulation frame and read identically by both eyes.
- **T4.** On any discontinuity (trigger list in `CLAUDE.md`), history is reset for both eyes.

### Inventory

Filled in from Phase 0 findings. Every row must reach `confirmed` in Phase 6, with evidence in `docs/re/temporal.md`.

| State | Scope | Handling | Status |
|---|---|---|---|
Phase 0 first pass (static only; details and addresses in `docs/re/temporal.md`). Key finding: the engine already stores view-dependent temporal state in **2-slot arrays indexed by camera index** (`ViewContext+0x464`, the split-screen player slot), and `DeferredRenderLoop::Render` runs the full pipeline once per view. Candidate **mechanism C**: render eye k as engine camera index k through the split-screen two-view path.

| State | Scope | Handling | Status |
|---|---|---|---|
| TAA history color (including engine ping-pong) | per-eye | engine slot: RT pair at `RenderLoop+0xce8+idx*0x10`, swapped in `RenderTAA` 0x1408ddf70; mechanism C or A | candidate |
| TAA jitter index / offset | per-eye | 2-phase half-pixel offset from `Renderer::DoUpdate` (global parity counter), added to proj [0][2]/[1][2]; **disabled when a second view exists** — must be re-enabled per eye | candidate |
| Previous view / projection / viewProj (jittered + unjittered) | per-eye | engine slot: `lastVP` `RenderLoop+0xed8+idx*0x40`, camera pos `+0xf58+idx*0xc`, latched in `PostUpdateLastFrameCamera` 0x1408dc2f0 (unjittered, camera-relative) | candidate |
| Previous depth / Hi-Z (disocclusion, occlusion culling) | per-eye | engine slot: `Last Frame Depth`/`Last Frame Buffer` `RenderLoop+0x10e0/+0x10f0+idx*8` (SSR, water drops) | candidate |
| Temporal AO / SSR / volumetric accumulation | per-eye | froxel history per slot (`DeferredLighting+0x760/+0x780`); SSR uses last-frame buffers; IntelSSAO looks stateless | candidate |
| Motion blur camera matrices | per-eye | uses the same lastVP slot; motion-vector RT is a single per-view transient | candidate |
| Per-object previous world transforms | shared | `Mesh::UpdateRenderData` 0x1408a3f00 advances prev only when frameId changes (idempotent for a 2nd view) | candidate |
| Auto-exposure / eye adaptation | shared | engine keeps **per-slot** adaptation (tonemapper `+0x138+idx*8`) — the eyes would drift; mod must force one shared value | candidate |
| GPU particles / simulations | shared | PopcornFX evolve once in `OnRender`; render/kick per view | candidate |
| Shadow maps / cascades | shared | CSM per camera index; whether fitted per view or merged is open | candidate |
| Reflection / environment probes | shared | `RenderLoop::RenderProbe` once per frame, outside the view loop | candidate |
| Dynamic resolution (new) | shared | global GPU-time controller; must be off/fixed in VR | candidate |
| History reset (new) | both eyes | engine has no camera-cut reset; shared reset flag `RenderLoop+0xd08` (`ClearTAA`) | candidate |

Why exposure is shared: two separate adaptation histories drift apart and give the eyes different brightness, which causes binocular rivalry. Exposure still adapts over time; there is one adaptation state for the head.

Why per-object previous transforms are shared: if the engine updates each object's "previous world matrix" during rendering, the second eye's render would see zero object motion. The update must happen once per frame.

### Mechanisms

C is the first choice; A, B1 and B2 are fallbacks per state. Chosen per state in Phases 4 and 6 and recorded in `docs/re/temporal.md`.

- **A. Engine state swap (preferred where the state can be found).** Locate the engine's per-view temporal state: history texture pointers, previous matrices, jitter index, "history valid" flag. Keep one copy per eye; swap it in before eye E renders and save it afterwards. The engine's own ping-pong then runs inside each eye's state, and previous-matrix/jitter constants come out right automatically.
- **B1. Copy-in / copy-out (fallback, simplest).** Identify the temporal pass by shader hash. Immediately before its draw/dispatch: `CopyResource(ViewState[E].history → texture bound as history SRV)`. Immediately after: `CopyResource(pass output → ViewState[E].history)`. This works with engine ping-pong because it copies whatever is bound at that draw. Cost: two full-resolution copies per eye per temporal pass.
- **B2. Binding substitution (optimization of B1, only after B1 works).** Replace the history SRV and output RTV/UAV bindings with ViewState[E]'s views; no copies.
- **C. Engine dual-view slots (preferred, Phase 0 finding).** Render the two eyes as the engine's two split-screen views; the engine keeps TAA, last-frame, froxel and exposure history per camera index, so most per-eye state needs no copying. Our work: second camera, per-eye projection and jitter, shared exposure, full-size targets (Phase 4).
- **Constants with B1/B2:** patch the previous-matrix and jitter fields of the relevant constant buffer (Map/Unmap or UpdateSubresource hook) with ViewState[E] values. Field offsets are documented in `docs/re/temporal.md`.

Per-eye resources are created by us with the same `D3D11_TEXTURE2D_DESC` as the engine's texture (copied when first seen), and recreated on resize or format change.

### Tests

Each must pass in Phase 4 for TAA, and in Phase 6 for every per-eye row.

- **Cross-eye leak test.** A `[debug]` option tints eye L's pre-TAA scene color pure red for exactly one frame. Pass: red appears in eye L and decays over the following L frames; eye R's output has zero red-dominant pixels in every frame (checked with a dump histogram).
- **Static convergence test.** Head and bike still. Pass: each eye's TAA output converges; consecutive same-eye frames differ by less than a threshold (set in Phase 4).
- **Head-shake test.** Fast head yaw of ±45°. Pass: no ghost of the other eye's viewpoint; trails comparable to flat-screen TAA under equal camera motion.
- **Cut test.** Restart the race or switch camera. Pass: the first frame after the cut shows no smear from before the cut in either eye.
- **Budget.** The overlay shows VRAM used by ViewState resources.

### Alternate-eye note

In alternate-eye mode, eye E's history is from frame N−2 while per-object previous transforms are from N−1, so moving objects get a small motion-vector error. Acceptable for the milestone; synchronous stereo removes it.

---

## Phase 0 — Recon & setup (no mod code)

Goal: know exactly what we're dealing with before writing hooks.

**Workspace**
- [x] Create `C:\dev\tt2vr\` (git, private remote) and `C:\dev\tt2vr-re\` (no git, back up locally). Set `TT2_GAME_DIR` to the game's install folder.
- [x] Create `.claude/settings.local.json` (gitignored) with `permissions.additionalDirectories` for `<TT2_GAME_DIR>\tt2vr` and `C:\dev\tt2vr-re`, so Claude Code can read logs, dumps and captures.

**Toolchain**
- [x] VS 2022 (Desktop C++), CMake ≥ 3.28, vcpkg, Git. VS Community 17.14, CMake 4.4.4, vcpkg at `C:/tools/vcpkg` (`VCPKG_ROOT`).
- [x] RenderDoc, x64dbg, and a PE inspector (PE-bear or Detect It Easy). RenderDoc 1.46; x64dbg, PE-bear, DIE in `C:/tools/`.
- [x] RenderDoc MCP server for capture analysis (vetted community server with a qrenderdoc bridge), registered with Claude Code. LifeSugar/RenderDoc_MCP @359e4e3, hardened locally (program launch removed, captures limited to `C:\dev\tt2vr-re\rdc`); RenderDoc upgraded to 1.46.
- [x] Ghidra 12.1.4 and JDK 21, then REA (REA times out on TT2.exe at the default; user env `REA_GHIDRA_STARTUP_TIMEOUT_MS=2700000` + `GHIDRA_HEADLESS_MAXMEM=12G` set, effective after a restart; meanwhile use the `tools/ghidra/` query server) (`npm install --global rea-agents`). On Windows `rea setup` doesn't run, so:
  - set `GHIDRA_INSTALL_DIR`, `JAVA_HOME` and `REA_ANALYSIS_PROVIDER=ghidra`, then check with `rea doctor --json`;
  - register REA's MCP server with Claude Code manually (REA installation docs, MCP registration section);
  - raise `REA_GHIDRA_STARTUP_TIMEOUT_MS` if the exe times out;
  - fallback if Windows support is too limited: run REA + Ghidra under WSL2 (Linux x64 is REA's main platform) on the snapshot.

**Game facts → `docs/re/README.md`**
- [x] Steam build ID, exe name, SHA-256. Copy the exe into `tt2vr-re/bin/<build-id>/`.
- [x] Bitness and imports (d3d11/dxgi, input DLLs).
- [x] Protection status (SteamStub only, code not encrypted; engine AntiCheatManager inert): section names, entropy, Steam DRM wrapper, Denuvo, anti-cheat folders. **If the exe is packed or encrypted on disk, stop and discuss with me. Don't remove DRM.**
- [x] In-game graphics options (AA modes including TAA, motion blur, resolution, FOV) and the config file location. Use the TAA setting for all captures. Menu: AA Off / FXAA / TAA (menu "TAA" is stored as `FXAA_TAA` = mode 2), motion blur Off / Low / Normal, dynamic resolution On/Off, no FOV option. Config: `Documents/My Games/TT Isle of Man 2/UserSettings.cfg`, plain `Engine.X.Y = value;` lines.

**Frame anatomy (RenderDoc) → `docs/re/frame-anatomy.md`**
- [x] Capture helmet-cam frames: daytime on the TT course, plus one in a menu. Done 2026-10-16 with the Phase 1 first slice (3 captures in `tt2vr-re/rdc/`). Background: RenderDoc cannot capture the unmodified game. `EnableVideoAdapter` passes `Flags = 0x80` (`D3D11_CREATE_DEVICE_PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY`) to `D3D11CreateDeviceAndSwapChain`; RenderDoc treats that flag as "application requested not to be hooked" (its log, `d3d11_hooks.cpp(163)`), skips wrapping the device while DXGI is already wrapped, and the call returns S_OK with a null immediate context. The game then crashes at `TT2.exe+0x9592dd` (`mov r9,[rcx]`, QueryInterface on context slot 0). Confirmed 2026-10-16 from 3 CrashRpt minidumps (access violation reading 0, rcx = 0, loop index 0, returned feature level 11_1) and the RenderDoc log; NVIDIA NvCamera and the Steam overlay were ruled out. Fix: our proxy hooks `D3D11CreateDeviceAndSwapChain` and clears bit 0x80 before the call goes on (debug-layer registry behaviour only; no gameplay effect).
- [x] Pass table (runtime table from capture f8323 in `frame-anatomy.md` 4.1; open items in 4.2): event range, guessed name, RT formats and sizes, inputs, outputs, `per-frame` or `per-view`, temporal inputs.
- [x] Threading (static: no render thread; pass execution on deferred contexts per enki worker): are deferred contexts / `ExecuteCommandList` used? Is there a separate render thread?
- [x] Identify (TAA 36731, motion vectors 20321..20633, exposure 36541/36606, CSM atlas, 2D HUD from 37099; see `frame-anatomy.md`): TAA pass (jittered projection, history SRV slot, output), motion-vector pass, exposure pass, shadow passes, HUD start event.
- [x] First pass at the temporal inventory table above, with real passes and resources.

**Conventions → `docs/conventions.md`**
- [x] Depth (static; RenderDoc check pending): clear value, depth function (reversed-Z?), infinite far plane?
- [x] Matrices (static; RenderDoc check pending): row or column major in constant buffers, handedness, units, up axis, camera-relative rendering?
- [x] Back buffer format and color space (static).

**Loader decision → Decisions log**
- [x] Requirement: our D3D11 device hooks must exist before the game creates its device, because shader bytecode is only visible at creation time.
- [x] Option (a): proxy `d3d11.dll` or `dxgi.dll`. Simplest, but conflicts with ReShade-style overlays.
- [x] Option (b): a neutral proxy from the import list (e.g. `version.dll`, `winmm.dll`, `dinput8.dll`) plus `LdrRegisterDllNotification` to hook `D3D11CreateDevice*` when `d3d11.dll` loads.
- [x] Pick one and record why. **Chosen: (b) with `dinput8.dll`** (Decisions log, 2026-10-16).

**Initial RE (Ghidra/REA) → `docs/re/symbols.md`**
- [x] Strings and xrefs for: camera modes (helmet / first person), TAA, jitter, motion blur, exposure, render pass names, config keys.

**Done when:** `frame-anatomy.md` has a classified pass table; `conventions.md` is filled in with evidence; protection status is known; the loader is decided; the temporal inventory has a first pass.

## Phase 1 — Skeleton mod (flat, no VR)

**First slice (unblocks Phase 0 captures):** CMake/vcpkg skeleton, the `dinput8.dll` proxy (5 forwarded exports, loads `tt2vr\tt2vr.dll`), a MinHook hook on `D3D11CreateDeviceAndSwapChain` that clears flag `0x80` and logs the call, and the `deploy` target. Then capture through RenderDoc as in `docs/re/frame-anatomy.md`. The rest of Phase 1 follows.
- [x] First slice verified 2026-10-16: game runs with the proxy; under RenderDoc the log shows `flags 0x80 -> 0x0, hr=0, featureLevel=0xb100, context=ok, next=renderdoc.dll`. RenderDoc re-hooks the import slot on every DLL load (25 re-applies in the first 1.4 s), so the watchdog is required. Captures: `tt2vr-re/rdc/tt2_snaefell_helmet_moving_taa_f8323.rdc`, `..._stationary_taa_f8956.rdc`, `tt2_mainmenu_f10838.rdc`.

- [x] `CMakeLists.txt`, `CMakePresets.json` (VS 2022 generator, `x64-debug` / `x64-release`, test presets, `deploy` target), `vcpkg.json` with: minhook, spdlog, imgui (dx11-binding, win32-binding), tomlplusplus, directxtk (ScreenGrab), openxr-loader, xxhash, doctest.
- [x] `.gitignore`: build output, `*.rdc`, dumps, `CLAUDE.local.md`, `.claude/settings.local.json`.
- [x] Proxy DLL (per the Phase 0 decision): forwards all exports and loads `tt2vr\tt2vr.dll`. No heavy work in `DllMain` (loader lock); everything else happens on an init thread or the load notification.
- [x] `core`: logger; TOML config including hotkeys; MinHook wrapper (install / enable / disable all); IDA-style pattern scanner with `??` wildcards; exe SHA-256 check. Unknown version → D3D layer only, engine hooks off, overlay notice.
- [x] `d3d`: device and swapchain creation hooks, `Present` / `ResizeBuffers`, `Create*Shader` hooks feeding a shader registry (xxh64 of bytecode → stage, size, our name from `src/d3d/shader_names.inc`).
  - Verified 2026-10-16 (30 min Snaefell session, no crash): 53 context hooks, deferred vtable shared with the immediate one (under RenderDoc), 1953 unique shaders in `shader_registry.txt`.
- [x] Frame trace: on hotkey, log every relevant context call for one frame (shader binds with names, `OMSetRenderTargets`, SRV/UAV binds, viewports, draws/dispatches, copies, clears, constant buffer updates) to `captures/frametrace_<frame>.txt`.
  - Verified 2026-10-16: `frametrace_10866.txt`, ~36k calls, imm + 4 deferred contexts, 9 command lists spliced, CB data with matrices.
- [x] Texture dump: on hotkey, save tracked textures (PNG for UNORM, DDS for float formats).
  - Verified 2026-10-16: `11318_dump.txt`, 61 snapshots, 0 failed; back buffer, reversed-Z depth and HDR previews look correct.
- [x] Remote requests: Claude writes a command file into `tt2vr/commands/`; the mod checks it once per frame and runs a frame trace, a texture dump, or — when the game was launched under RenderDoc (`renderdoc.dll` loaded) — a RenderDoc capture through the in-application API (`renderdoc_app.h`, `TriggerCapture`), written to the capture folder set in `tt2vr.toml`. Balazs only holds the game in the wanted state.
  - Verified 2026-10-16: `tools/tt2vr-request.ps1 frametrace shaders` returned `frametrace_5140.txt` and `shader_registry.txt` within one poll. RenderDoc capture by request not yet exercised.
- [x] ImGui overlay on the desktop window: FPS, hook status, version status, toggles.
  - Verified 2026-10-16: panel, hotkeys and the red unknown-version text seen by Balazs.
- [x] Unit tests: pattern scanner, config parsing (also hotkeys, SHA-256, version table, command files): 30 cases, `ctest --preset x64-debug` passes.

Implementation notes (2026-10-16):
- `core` ships config + hotkeys (`core/config.*`), MinHook wrapper (`core/hooks.*`), pattern scanner (`core/pattern.*`), SHA-256 via CNG (`core/sha256.*`), remote command files (`core/commands.*`); `game/version.*` holds the known-build table (build 4989319) and `game/signatures.hpp` the (still empty) signature table. A non-empty `[debug] fake_exe_sha256` fakes the hash for the unknown-version test.
- D3D layer: vtable slots in `d3d/vtable.hpp` are checked at compile time against the SDK's C layout (`vtable_check.cpp`). Context hooks cover the immediate and a deferred context vtable; deferred calls are buffered per context and spliced into the trace at `ExecuteCommandList` (GPU order). Shader objects carry their xxh64 as private data.
- Texture dump rule: a written target is snapshotted on the immediate context when the immediate context explicitly rebinds its OM/CS outputs without it, and at frame end; float formats get a DDS plus a `.preview.png`. Index: `captures/<frame>_dump.txt`.
- Remote requests: `tools/tt2vr-request.ps1 frametrace dump "capture <name>" shaders` writes `tt2vr/commands/<id>.txt` and prints `<id>.done`.
- Overlay never subclasses the game window: it polls the mouse buttons, so game input is untouched (clicks also reach the game).

**Done when:** 30 minutes of offline riding with the mod loaded, no crash; overlay works; shader registry and frame trace are produced; the unknown-version path is tested by faking the hash.
- 2026-10-16: 30 min ride, trace, dump and registry done. Fake-hash test showed no notice; the log was overwritten, so the cause is unknown (most likely the value was rejected by the 64-hex check). Since then: config warnings are shown in the overlay, and the previous log is kept as `tt2vr.prev.log`. Retest 2026-10-16: with 64 zeros the overlay showed the red unknown-version text; `tt2vr.prev.log`: `exe sha256 0000…0000 (faked) -> UNKNOWN version: engine hooks off, D3D layer only`. **Phase 1 done.**

## Phase 2 — Camera control (flat)

- [x] Find the view matrix:
  - search constant buffer updates for a 4×4 whose upper 3×3 is orthonormal and whose translation changes as the bike moves;
  - put a hardware write breakpoint (x64dbg) on its CPU-side source to find the writer function, then walk up the call chain in Ghidra/REA;
  - cross-check with the strings route from Phase 0.
  - 2026-10-16: found statically instead (Phase 0 strings route + Ghidra): pool matrix written by `KTUCamera::Update` → read by `ViewContext::InitFromCamera` 0x14091aeb0 → `cbCameraStd` (runtime f8323). Runtime check done 2026-10-16 from our log (`docs/re/camera.md` section 8, checks 1–2): world columns (right, up, back), Z up, det +1; helmet cam hfov 91°, near 0.01, far 25000, front pass off.
- [x] Document in `docs/re/camera.md`: the camera object fields we use; where world→view and projection are built (fovY, aspect, near, far, jitter); where the culling frustum comes from. Done 2026-10-16 (fov is horizontal; runtime checks in its section 8).
- [x] `game::CameraOverride`: hook after the camera update; overlay sliders for yaw, pitch, roll and position offset.
  - Code done 2026-10-16 (`src/game/camera.*`): `DoUpdate` pre + `InitFromCamera` post-fix-up of the main ViewContext; the scene-graph pool is never written. Verified 2026-10-16 by Balazs: yaw/pitch/roll/offset all work in the helmet cam.
- [x] Projection override: inject an asymmetric frustum (e.g. shifted 20% right) and verify both the image and culling.
  - Code done 2026-10-16: `MatrixPerspectiveFovRH` choke point, reshapes calls whose fov is the main camera's fov or front fov inside the DoUpdate→Present window. Verified 2026-10-16 by Balazs.
- [x] Unit tests: projection-from-FOV builder against hand-computed values; XR→engine basis conversion. `tests/test_math.cpp` (11 cases) + signature table test; 42 cases pass.
- [x] Helmet mesh follows the view override (found 2026-10-16: the HelmetView actor is placed by game code in `FUN_1403afc80` from `mode+0x4c4`, not from the render view; `docs/re/camera.md` section 7). Chosen 2026-10-16 (Balazs): physics-step hook (`TTCamera::AfterPhysicsStep` + `Actor::SetWorldMatrix`, helmet' = M·local·M⁻¹·H with M = mode+0x8), switchable via `[camera] helmet_follows_view` / overlay; the game's Helmet No Mesh view stays supported. Verified 2026-10-16 by Balazs (log 15:19: `camera: helmet hooks installed`).

**Done when:** I can turn the helmet camera 360° from the overlay with no culling holes and correct depth, and an asymmetric projection renders correctly at the frustum edges.
- 2026-10-16: verified by Balazs (view override through ±180° yaw, projection shift, helmet follows, Helmet No Mesh view). Also added: F8 software mouse cursor for the overlay. **Phase 2 done.**

## Phase 3 — OpenXR, left eye head-tracked

- [x] `xr`: instance with `XR_KHR_D3D11_enable`; log the active runtime name and version (`xrGetInstanceProperties`), since Meta and SteamVR runtimes behave differently.
- [x] `xrGetD3D11GraphicsRequirementsKHR`: if the adapter LUID differs from the game device's adapter, log it and stay flat.
- [x] Session on the game's device; `LOCAL` reference space; per-eye swapchains (format per the sRGB rule); a session state machine (idle, focus loss, stopping) that never blocks the game.
- [x] Frame loop: `xrWaitFrame` / `xrBeginFrame` / `xrEndFrame` integrated with `Present`. Place `xrWaitFrame` so it throttles the start of the frame rather than stalling mid-render; measure.
- [x] Force the game to render at the recommended per-eye resolution × config scale (cleanest route: game config file, window size, or `ResizeBuffers`).
- [x] Left-eye-only mode: camera from `P_L`, projection from `F_L`, right eye black. This validates pose, projection and conversion exactly; a mono image shown to both eyes would match neither.
- [x] Recenter via hotkey and overlay.

Implementation notes (2026-10-16, code done, runtime test pending; all boxes above stay open until Balazs reports):
- `src/xr/session.*`: instance (`XR_KHR_D3D11_enable`, logs `xr: instance created, runtime ...`), `xrGetSystem` retried every 2 s while no headset, LUID + feature-level check (`xr: game adapter ... -> match`; mismatch = flat), session on the game device, `LOCAL` space, state machine (READY begin, STOPPING end, LOSS_PENDING retry after 3 s, EXITING stays flat until overlay "Restart XR", instance loss retry after 10 s).
- Frame loop on the main thread: Present post-hook (last) `xrWaitFrame` + `xrBeginFrame`; `Renderer::DoUpdate` pre `xrLocateViews` once per XR frame; Present pre-hook (before the overlay) eye copy + `xrEndFrame` with the located poses/FOVs. No head-tracked render that frame = no layers. Overlay shows the smoothed `xrWaitFrame` time and the display period.
- `src/xr/swapchain.*`: per-eye swapchains at recommended × `[xr] resolution_scale`, format order R8G8B8A8_SRGB, B8G8R8A8_SRGB, then UNORM. Same size and family as the back buffer: raw `CopySubresourceRegion`; otherwise `d3d::Blitter` stretch blit (raw gamma bits, or sRGB decode when the image only has a typed sRGB view or the swapchain is linear). Right eye cleared to black.
- Resolution: the Windowed `UserSettings.cfg` route failed at runtime (2026-10-16: Windows clamped the window to 2528x1060, then the game fell back to 1024x768; settings restored, script removed). Replaced by `game::RenderSize` (`[xr] force_render_size`, default on): a hook on the engine's client-size reader `FUN_140959f80` reports the eye size for the main window, and `SetDeviceResetNeeded` makes the engine resize the swap chain and every window RT itself (`docs/re/frame-anatomy.md` 8.5). Geometry is correct even when stretched, because m00/m11 come from `XrFovf`, not the back-buffer aspect.
- Head tracking: `game::CameraOverride` view = world · sliders · `EyeLocal(recenter, P_L)`; projection = `F_L` tangents at the `MatrixPerspectiveFovRH` choke point (engine still adds TAA jitter). Recenter (yaw-only, eye midpoint) on session start, hotkey F7, overlay button, and after a runtime LOCAL recenter event.
- Desktop: `[xr] desktop_vsync_off` (default on) presents with sync 0 while the session runs; `[xr] desktop_mirror = false` skips the desktop Present (the game still renders once per frame; see Decisions log).
- Known: HUD is baked into the eye image (Phase 7); TAA smears one frame after a recenter (history reset is Phase 6).
- Runtime 2026-10-16 (Balazs, SteamVR "in Meta compatibility mode 2.17.10", Quest 3 over Link): head-tracking test good; session focused, 72 Hz (period 13.89 ms), `xrWaitFrame` 3.72 ms, 10036 frames submitted, recommended eye 2528x2704; back buffer 1920x1080 -> blit (stretched). Recenter (F7) and colour match confirmed by Balazs 2026-10-16. Resolution: render size hook verified 2026-10-16 (log `render size: forcing 2528x2704` then `ResizeBuffers 2528x2704`; Balazs: image fine, more resolution would be nice but GPU-bound, RTX 3060). **Phase 3 done** (done-when: sign fixed under head motion, colours match). Interim menu screen verified 2026-10-16 (Balazs: menus, loading and the TT2 pause menu on the screen; riding head-tracked; signal = menu flags || game time factor 0 || player camera not TTSpringCameraMode-and-rendered). Lost-steering report (Moza R9 wheel + Fanatec pedals, DirectInput; TT2 logs "Wheel family not handled" for them): `[debug] input_log` 2026-10-16 16:51-16:52 shows both devices attached and reporting steering/throttle/brake every second through the failure (second hard braking zone ~16:52:21, full lock + throttle 16:52:28-30), game window focused, no detach. The input reaches DirectInput; the game ignored it (crash in progress). Open: A/B with the mod disabled and with `[camera] helmet_follows_view = false` (our only physics-step hook) to rule the mod out. Perf without RenderDoc: ~55 fps riding at 2528x2704, ~120 ms spikes in most 10 s windows.
- Interim menu screen (2026-10-16, Balazs chose "interim screen now"; code done, runtime test pending): while `KTUGuiManager::IsMenuOpen` or `IsGamePaused` is true (exports, read via `g_pGuiManager`; `src/game/menu_state.*`), the camera is not head-tracked, and any frame without a head-tracked render (menu, pause, loading, movie, unknown exe) goes to a quad layer fixed in LOCAL space, `[xr] screen_distance` ahead of the recentered head (`screen_width`, aspect from the back buffer). Runtime 2026-10-16: the flags alone missed the main menu (`IsMenuOpen` false there; `paused` only during intros), so riding is now "player camera in `TTSpringCameraMode`" (`docs/re/frame-anatomy.md` 8.4); runtime check pending. The full frozen-3D-world menu stays in Phase 7.

**Done when:** in the headset, a roadside sign I fixate on stays fixed while I rotate and lean my head (no swimming), and colors match the desktop.

## Phase 4 — Dual-view stereo via the engine's split-screen path (milestone)

Approach (proposed 2026-10-16, see "Phase 0 findings"): the engine renders up to two views per frame in one `DeferredRenderLoop::Render` call (`Renderer::DoUpdate` 0x1405addd0 builds ViewContexts from cameras at `Renderer+0xe48/+0xe50`) and keeps TAA, last-frame, froxel and exposure history per camera index. Eye L = camera index 0, eye R = camera index 1. Both eyes come from the same simulation frame, so this milestone is already synchronous. The original alternate-eye plan is kept as the fallback below.

- [x] Static deep-dive before hooking (Ghidra + a split-screen RenderDoc capture): how split mode 1 is switched on at the **renderer** level only (view mask `RenderDesc+0xce8`, `Renderer+0xc90`, `RenderLoop::OnSplitChanged`) without `GameEngine::SetSplitMode`'s gameplay side effects (second player, `EffectorMapManager`, per-split CVar tables). Rule 5: visual only — if the renderer cannot be put in two-view mode without gameplay changes, stop and decide together.
  - 2026-10-16, static (no split capture: the game never enables split in single player): **viable renderer-only**, `docs/re/frame-anatomy.md` 5.1. Recipe: `RenderTargetManager::SetPossibleSplitMode(1)` once; every frame in the DoUpdate pre-hook `CameraDesc {1, camL, camR, mask 3}` into `Renderer+0xe40..+0xe58` (re-asserted, level loads push mode 0) and `Renderer+0xd88 = +0xd80` (view 1's scene); copy the effector-map centre to slot 1. `GameEngine::SetSplitMode` is **not** render-only (FMOD listeners, GUI screens, `s_bSplitEnabled` CVar caps, cloth sim, content density, FOV) and is never called. Findings that change the tasks below: (1) every window RT is half the window width in mode 1, so a (2 x eyeW) x eyeH back buffer gives two full eyes with no patch (`ComputeSplitSize` has no callers); (2) **jitter is not disabled in split mode** (gate re-read from disasm; old notes corrected); (3) `CopySplitRT` only queues a copy, so the eyes are taken from the back-buffer halves at Present; (4) the global 2D HUD list is drawn across the whole back buffer after the views (where the race HUD lands is a runtime check).
- [x] Second camera: drive `Renderer+0xe50` with our eye-R `Graphics::Camera` (a copy of the helmet camera node, moved by `P_R`); eye L uses the real camera moved by `P_L`.
  - Done differently: both views use the game camera (`{1, cam, cam, 3}`), each ViewContext moved by its eye pose after `InitFromCamera` (4b). Verified by Balazs 2026-10-20 (stereo, dash and bike look right).
- [x] Full-size eye images. Done 2026-10-20: back buffer 2 x eyeW, eye k from half k at Present (`CopySplitRT` only queues). Preferred: render at width `2 × eyeWidth` with vertical split, so each half is one full-resolution eye; grab each view after `CopySplitRT` (RenderLoop vf+0x50). Alternative if the back buffer can't be that wide: patch `ComputeSplitSize` so each view gets a full-size target.
- [x] Per-eye asymmetric projection from `F_E` for each ViewContext (done 2026-10-20 via the choke point with a current-eye context, union frustum elsewhere; frame trace 7170 shows mirrored m02 per view), using the reversed-Z RH form in `docs/conventions.md`. Cover every place that rebuilds a symmetric projection from camera FOV (`PrepareForCulling`/`RestoreAfterCulling`, `SetupZFrontViewContext` front pass, `FillViewVector`, `GetFrontPassInvProj`, CSM setup, sky, 3D HUD) — or hook `MatrixPerspectiveFovRH` 0x140813390 with the current view's eye.
- [x] Per-eye TAA jitter: ~~the engine disables jitter when two views exist~~ (refuted statically 2026-10-16; runtime: RenderDoc `shadows_rider_frame3561` both views `projectionOffset.x = 1/1770`); confirm at runtime that both views' 'TAA resolve' get a non-zero `g_jitter` that alternates per frame, and that our projection reshape keeps the offset in proj [0][2]/[1][2].
- [x] Shared exposure (verified 2026-10-20, frame trace 4400: both views' `HDR_FinalCompose` bind the same 1x1 luminance `srv1718`, same PS): slot 1's adaptation must use slot 0's luminance (tonemapper per-slot history `+0x138+idx*8`); verify with identical exposure in both eyes.
- [x] Remove split-only visuals: `Split border` pass, per-split 2D HUD duplication (HUD handled in Phase 7). Border hidden; split quality caps lifted (cascades, static shadow); per-camera-index player state mirrored into view 1 (rider meshes, dashboard HUD list).
- [x] Tint injection for the cross-eye leak test (remote `leaktest` command instead of a `[debug]` option; passed 2026-10-21).

Implementation steps (proposed 2026-10-16, **waiting for Balazs's OK**; each step is built, deployed and runtime-tested before the next):

1. **4a Two views, both from eye L.** New `game::SplitView` (`src/game/split_view.*`): signatures for `Renderer::SetCameraDesc`, `RenderTargetManager::SetPossibleSplitMode` + its singleton, `EffectorMapManager::SetCenterWorldPos` + its singleton. Hook `SetCameraDesc` to remember the game's own desc. DoUpdate pre: while stereo is wanted (XR running, riding, `[xr] stereo = true`), write `{1, cam, cam, 3}` + view-1 scene + effector centre, hide 'Split border' (`DeferredRenderLoop+0x1e71 = 0`); otherwise write the game's desc back. Render size hook: (2 x eyeW) x eyeH while stereo is wanted. `xr`: eye k <- back-buffer half k (raw `CopySubresourceRegion`); the menu screen shows the full back buffer, or the left half on a split frame. Any missing signature = Phase 3 left-eye mode. Test: both eyes show the same image, no crash, one `OnSplitChanged` per riding-mode entry, fps.
2. **4b Per-eye view and projection.** `InitFromCamera` post-hook picks eye k by `ViewContext == Renderer+0x30 / +0x650` and applies `P_k`. The `MatrixPerspectiveFovRH` choke point needs the current view: a per-view hook (candidate `RenderContext::Prepare(const ViewContext&)` 0x1408cfad0, camera index `ViewContext+0x464`) sets it on the main thread; calls with no current view are counted per frame and must be shown harmless (CSM setup, render jobs). `xr` submits both eyes with their own pose/FOV. Test: the Phase 3 sign test in stereo, depth correct.
   - 2026-10-20: **4b code done, runtime test pending.** View k renders eye k (pose `P_k`, tangents `F_k`), xr submits each eye with its own located view. Every `MatrixPerspectiveFovRH` rebuild picks its eye from a thread-local current eye: set while `InitFromCamera` builds view k, while `PrepareForCulling`/`RestoreAfterCulling` handle a view (camera index `+0x464`), and from `RenderContext::Prepare(view)` on `RenderLoop+0x1f00` until the next view; reset around `DeferredRenderLoop::Render`. Without a current eye (scene phase: CSM cascade fitting; job threads) the union of both eye frusta is used, so cascades cover both eyes alike. The helmet mesh follows the head (eye midpoint). Dashboard: view 1 draws view 0's HUD list (`ViewContext+0x5e8`; caveat: eye 1 gets eye 0's lens flares until Phase 7).
3. **4c Shared exposure.** Short RE of the tonemap entry `FUN_140914680` / `FUN_1409120b0` (adaptation gate `tm+0x150`, the `+0x130` / `+0x138+idx*8` swap), then for camera index 1: skip the adaptation step and compose from slot 0's 1x1 luminance, restoring slot 1's pointers afterwards. Source = eye L. Test: dump shows the same 1x1 luminance texture for both 'HDR_FinalCompose' draws.
   - 2026-10-16: **4a code done, runtime test pending** (`src/game/split_view.*`, `[xr] stereo`). Deviation from the plan: no `SetCameraDesc` hook; the game's own desc writes are detected by comparing the Renderer fields with what we wrote last frame (a `SetActiveCamera` call under our override keeps our mode/mask and only swaps camera 0). The width stays doubled in menus too, so riding <-> menu never resizes the swap chain; only the split toggle reallocates RTs (`OnSplitChanged`, a history reset of both slots).
   - 2026-10-20 runtime fixes from 4a testing: rider meshes per camera index mirrored into view 1 (`frame-anatomy.md` 5.2); split-screen quality caps lifted (CSM cascade count); VR shadow maps at `[xr] shadow_scale` x the game size (the power-of-two cascade crop halves density with the wide eye frustum, 5.3); AO view reconstruction fixed for off-centre projections (`src/game/ssao_fix.*`: dark patches that moved with the head); Windowed-mode window no longer resized to the render size; opaque eye images; startup deadlock (exe hash vs SteamStub) fixed. **Lost steering/braking resolved by Balazs 2026-10-20: not the mod** - braking switched the game to controller prompts (a second mapping on the pedal); deleting the extra controller mappings fixed it. Open: SteamVR background faintly visible while riding (OPAQUE is the only blend mode offered; probe pending).
   - 2026-10-20: **4c code done, runtime check pending** (`src/game/shared_exposure.*`): hook on the tonemap entry `FUN_140914680` (pattern signature); view 1 composes with view 0's same-frame luminance (`tm+0x130 = tm+0x138`, gate `tm+0x150 = 0` for the call, slot 1 restored after). The view-1 `HDR_FinalCompose` permutation difference (SSR bit `tm[0xca]` from `DeferredLighting[0x1e+idx]`) disappeared once the rider-state mirror copied the first-person SSR byte (traces 13970/7170 use the same PS in both views). Check: both views' `HDR_FinalCompose` bind the same 1x1 luminance texture (frame trace).
4. **4d Tests and resets.** `[debug] leak_tint` (default off): eye L's 'TAA resolve' input tinted red for one frame (shader hash, first resolve of the frame = view 0). TAA reset for both slots on recenter and a hotkey: set `RenderLoop+0xd08` before each view's `RenderTAA`. Static convergence threshold set from dumps. Frame trace: one `DoUpdate`, two views per frame.

   - 2026-10-20: **4d code done, runtime pending** (`src/game/taa_control.*`): `RenderTAA` pre-hook writes the shared reset flag `RenderLoop+0xd08` before each view while a reset is pending (recenter, `taareset` command); `leaktest` command queues `RenderLayer::ClearRT(red)` on the post-process current RT before eye 0's TAA for one frame and reads both back-buffer halves at Present (baseline, tint frame, 6 after; pass = left jumps, right within 1% of its baseline).
   - 2026-10-21: **Leak test passed** (left eye 0.0% -> 99.7% red on the tint frame, back to 0.06% one frame later; right eye at its 0.15% baseline, max +0.01%). The engine-queued `ClearRT` was invisible (its pass queue executes before the main queue), so the tint is now a D3D clear: eye 0's history texture is resolved at record time (`RenderLoop+0xce8` -> RenderTarget vtbl+0x40 -> `Texture2DDX11+0x68`) and a pre-draw callback clears `t1` of the draw that reads it in `t2`. The red is gone after one frame because the resolve clamps history to the current neighbourhood. **Recenter reset verified** by Balazs (three F7 presses while riding, each logged "history reset for both eyes (recenter)", no smear). **Camera-cut reset added** (the engine has none, `temporal.md` N6): the game camera jumping more than 15 m or 45 deg in one frame resets both eyes, this frame and the next. **Not run (waived by Balazs 2026-10-21 when closing the phase):** the race-restart cut test and the `convergence` measurement (per-eye mean frame-to-frame luma change over 10 frames, head and bike still; provisional limits 2.0/255 mean and 2% of samples changing by more than 8/255). Both tools are deployed for Phase 6, which repeats the tests for every per-eye row.
   - 2026-10-21, run afterwards without the headset (test mode, `tools/tt2vr-test.sh`): leak test PASS (left 0.0% -> 81.8%, right +0.00%); **convergence PASS**: frame-to-frame change with history 3.48 / 4.12 (of 255), with the history reset every frame 11.28 / 11.66, ratio 0.31 / 0.35 (limit 0.60), eyes within 16% (limit 25%). An absolute limit does not work: the diff image shows wind in the foliage and the bike's idle shake (0.4 mm / 0.04 deg per frame), while road and sky are fully converged. **Race restart**: leaving the ride turns two views off and re-entry recreates both eyes' history (`OnSplitChanged`); the cut detector also fires on re-entry (9806 m, 86 deg). A cut inside two-view riding (no split toggle) is not reproduced yet.
   - 2026-10-21: **Phase 4 closed** by Balazs: stereo, dashboard, bike and shared exposure verified in the headset; leak test passed; recenter reset verified.
**Done when:** stereo depth is correct in static scenes; the leak, static convergence and cut tests pass for TAA; the frame trace shows one simulation/camera update per frame with both views rendered after it; exposure is identical in both eyes.

**Fallback (original plan) if the two-view path is not viable:** alternate-eye rendering with per-eye history via mechanism A (state swap of camera-index-0 fields) or B1 (texture copy-in/out), then Phase 5 per-view re-entry (`RenderLoop` vf+0x48 is re-entrant per Phase 0: the external-scene loop in `Renderer::OnRender` calls it several times per frame), then record/replay as last resort. Decide together before switching.

## Phase 5 — Synchronous stereo: shared per-frame work and correctness

- [ ] Map per-frame vs per-view work inside the two-view render (`docs/re/renderer.md`): probes (`RenderProbe`, once), CSM (per camera index — fit per eye or merged?), PopcornFX (evolve once, render per view), culling (per view from viewProj), froxels, lens flares.
- [ ] Shadows: confirm cascades cover both eye frusta without seams; share or keep per-eye based on cost and correctness.
- [ ] Per-object previous transforms: confirm `Mesh::UpdateRenderData` advances once per frame with two views (frame-id guard); check skinned meshes (`SkinMesh` per-camera-index validity).
- [ ] Re-entrancy/state audit for slot 1 (globals and RenderLoop fields that only assumed split-screen use).
- [ ] GPU timestamp queries per eye, shown in the overlay.

**Done when:** the leak and head-shake tests pass; per-frame passes appear once per frame in the trace; no visible seams or popping between eyes (shadows, culling, particles).

## Phase 6 — Temporal completeness

- [ ] Every inventory row implemented (per-eye via engine slot, or shared) and marked `confirmed` in `docs/re/temporal.md`, with evidence.
- [ ] History reset: the engine has no camera-cut reset. Reset both eyes on the CLAUDE.md trigger list (TAA reset flag `RenderLoop+0xd08` / `ClearTAA`, last-frame buffers, froxel ping-pong index −1, exposure first-frame flags).
- [ ] World Shift: confirm per-eye last-frame camera positions are rebased (`PreUpdateLastFrameCamera` handles both slots) and nothing smears across a 2048-unit shift.
- [ ] Dynamic resolution forced off (or a fixed scale) in VR.
- [ ] Overlay debug views: each eye's history, motion vectors and previous depth; a `[debug]` "freeze eye R history" option.
- [ ] VRAM readout for per-eye resources.

**Done when:** all four temporal tests pass for every per-eye row, exposure is identical in both eyes, and no temporal effect is disabled.

## Phase 7 — HUD, menus, desktop mirror

- [ ] Find where the HUD starts (frame trace: shader names or RT switch). The HUD renders once per frame into a UI texture, submitted as `XrCompositionLayerQuad` with configurable distance, size and tilt.
- [ ] Menus, loading screens and replays (no 3D scene this frame, or camera not first-person) go to a theater quad.
- [ ] Menu UI world-locked, never head-locked (Balazs, 2026-10-16, after the Phase 3 test: menu UI currently follows the head because it is baked into the eye image).
- [ ] Menu comfort (Balazs, 2026-10-16): no cinematic camera cuts in the main menu. Hold one fixed, head-tracked vantage point in the menu's 3D scene (render-side override only, rule 5) with the UI on a static quad in that world. Needs: a menu-vs-riding signal (RE), the HUD/UI separation above, and a check that the frozen vantage keeps its scene content loaded while the game's own menu camera moves elsewhere.
- [ ] Desktop mirror: eye L or side-by-side, per config.

**Done when:** the HUD is readable, not doubled, with no depth conflict, and I can navigate menus in the headset.

## Phase 8 — Bike comfort & camera

- [ ] Horizon lock: remove a configurable share (0–100%) of the bike's roll from `C` before applying the head pose.
- [ ] Camera shake / head bob scale (0–100%).
- [ ] Seated calibration: recenter aligns my head with the rider's eye point; the offset is saved.
- [ ] World scale and IPD scale options (history reset on change).
- [ ] Chase cam: decide whether to support it as an offset seat or disable it in VR.

**Done when:** a 10-minute ride on the TT course with horizon lock on is comfortable for me.

## Phase 9 — Performance

- [ ] Confirm per-frame passes are shared; if culling is significant, cull once with the combined eye frustum and reuse it for both eyes.
- [ ] Resolution scale; optional per-eye FSR1 (spatial, so no history concerns; reuse my openRBRVR FSR1 experience).
- [ ] Depth submission (`XR_KHR_composition_layer_depth`) for better runtime reprojection.
- [ ] Stretch: `XR_FB_space_warp` using the engine's motion vectors. Check runtime support on PC Link first.

**Done when:** the headset refresh rate holds at the chosen scale in a reference scene, with a frame-time graph in the overlay.

## Phase 10 — Robustness & release

- [ ] Signature table per known exe hash; graceful degradation on unknown versions.
- [ ] Crash handler writing a minidump to `tt2vr/logs/`.
- [ ] Compatibility notes: Steam overlay, RTSS, ReShade, other overlays.
- [ ] README: install, uninstall, config, known issues; license; credits.
- [ ] Decide whether the repo goes public.

---

## Risks

| Risk | Signal | Mitigation |
|---|---|---|
| Per-view render isn't re-entrant | second eye crashes or renders corrupted | snapshot/restore mutated globals; fallback record/replay (Phase 5) |
| Deferred contexts or a render thread | Phase 0 trace shows `ExecuteCommandList` / other threads | do stereo at the engine level, not the context level; hook command list building |
| Exe packed or encrypted | Phase 0 inspection | stop and decide together |
| Culling ignores our matrices | holes at the FOV edges | patch frustum construction or widen the engine's culling FOV |
| Double scene cost | low frame rate | shared per-frame passes, combined culling, resolution scale, FSR1 |
| Wrong OpenXR runtime active | runtime name in log | document runtime selection for Meta Link vs SteamVR |
| Game update | signature scan fails | version table, flat fallback |
| Game opts out of capture tools (`D3D11_CREATE_DEVICE_PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY`) | RenderDoc "application requested not to be hooked", null immediate context, crash at `TT2.exe+0x9592dd` | proxy clears flag 0x80 in our `D3D11CreateDeviceAndSwapChain` hook; keep that hook first in the chain |

## Decisions log

| Date | Decision | Reason |
|---|---|---|
| 2026-10-15 | Synchronous stereo is the target; alternate-eye is a milestone/debug mode only | both eyes must come from one simulation frame |
| 2026-10-15 | Temporal effects are never disabled; view-dependent state per eye, view-independent state shared | image quality, and exposure/sims must not diverge between eyes |
| 2026-10-15 | Repo outside the game folder; `deploy` target copies files; RE artifacts in `../tt2vr-re` (not in git) | no game material in git; survives Steam updates and file verification |
| 2026-10-15 | Hooking with MinHook; build with CMake + vcpkg + VS generator | simple, and scriptable by Claude Code |
| 2026-10-15 | GPU captures: Balazs drives the game into the wanted state; Claude triggers captures (Phase 1 command file + RenderDoc in-app API) and analyses them through a RenderDoc MCP server | Claude can't play or see the game, but can read every capture itself |
| 2026-10-16 | Loader: `dinput8.dll` proxy (forwards its 5 exports, loads `tt2vr\tt2vr.dll`) that hooks `D3D11CreateDeviceAndSwapChain` | static import of TT2.exe and not a KnownDLL, so it loads before any game code; no game copy to overwrite; leaves `d3d11.dll`/`dxgi.dll` free for ReShade-style tools |
| 2026-10-16 | Stereo approach (proposed, validate in Phase 4): engine split-screen two-view path, eye k = camera index k (mechanism C); alternate-eye + A/B1 kept as fallback | engine already keeps per-camera-index temporal history and renders both views from one simulation frame |
| 2026-10-16 | Signatures resolve by TT2.exe export name first, pattern scan as fallback (CLAUDE.md rule 3 reworded) | PlayAll engine functions are exported by mangled name, which survives code changes better than byte patterns; patterns stay as a checked fallback |
| 2026-10-16 | Camera override changes the main ViewContext after `InitFromCamera`, never the scene-graph pool; projection reshaped at the `MatrixPerspectiveFovRH` choke point inside the DoUpdate→Present window | the pool camera matrix is read by audio, physics and camera logic (rule 5); 20 render-side callers rebuild symmetric projections from the camera fields |
| 2026-10-16 | Pure matrix code lives in a `tt2vr_math` static lib (`src/stereo/math`), linked by the DLL and the tests | testable without the game; DirectXMath is header-only, no new dependency |
| 2026-10-16 | While the XR session runs, the desktop Present uses sync interval 0 (`[xr] desktop_vsync_off`, default on; chosen by Balazs) | `xrWaitFrame` paces the frame; desktop VSync at 50/60 Hz would fight the headset's 72/90 Hz. Only the desktop mirror's pacing changes (rule 5) |
| 2026-10-16 | `[xr] desktop_mirror = false` skips the desktop Present while the session runs (Balazs asked for a way to not render to the PC screen) | the scene is rendered once per frame either way; the desktop costs only the present. A smaller desktop window is not offered: the engine sizes its render targets from the window resolution, so it would shrink the eye image too |
| 2026-10-16 | XR frame loop: `xrWaitFrame`/`xrBeginFrame` at the end of the Present post-hook, `xrLocateViews` in `DoUpdate`, eye copy + `xrEndFrame` in the Present pre-hook before the overlay | the wait throttles the start of the next frame; the pose is taken after the game's camera update; the overlay stays desktop-only |
| 2026-10-16 | Interim menu comfort before Phase 4: menus/pause/loading on a world-fixed screen quad, not head-tracked (Balazs) | the menu's camera cuts and head-locked UI caused nausea; the full frozen 3D menu needs Phase 7's UI separation |
| 2026-10-20 | **Exception to CLAUDE.md rule 1 (Balazs's request):** motion blur off while the headset session runs (`[xr] motion_blur = false`, default); SSAO off too (`[xr] ssao`, not temporal) | the smear on bike and rider was ordinary camera motion blur lengthened by the VR frame rate, not a stereo artifact (MV data identical in both views, frame trace 13970); a comfort/clarity choice, TAA and the motion-vector pass stay on. Screen-space AO swims in the wide eye views even with correct constants |
| 2026-10-21 | **Exception to CLAUDE.md rule 5 (Balazs approved): test mode.** Launched with `-tt2vr-test` (only `tools/tt2vr-test.sh launch` passes it), the mod uses a simulated headset (no OpenXR; eye FOV / IPD / size from `tt2vr/headset_profile.txt`, saved by the last real session) and replaces XInput pad 0 with a scripted virtual pad (remote `pad`, `head`, `screenshot`, `cuttest`, `convergence`). Normal launches never install the pad | headset tests (leak, convergence, cut, head shake) without anyone wearing the headset; Claude drives the menus from screenshots. The wheel (DirectInput) is untouched |
| 2026-10-21 | Temporal mechanism: **C (engine dual-view slots)** for TAA, last-frame, froxel and exposure history; the mod only owns the shared TAA reset flag (written per view) and shares exposure from slot 0. A / B1 / B2 not needed so far; Phase 6 checks the remaining per-eye rows | leak test passed (no eye-0 history in eye 1), recenter reset clean in both eyes; the engine already keeps per-camera-index history |

## Open questions (answer in Phase 0)

- Does the game have TAA, and which other temporal effects exist? **Yes** (AA mode 2 FXAA_TAA / 3 TAA; 2-phase jitter, ping-pong history). Also last-frame colour/depth (SSR, water drops), froxel volumetrics, motion blur, per-slot auto-exposure, lens-flare queries, dynamic resolution. See `docs/re/temporal.md`. (static)
- Is there a separate render thread? Are deferred contexts used? **No render thread**: `GameContext::RenderOneFrame` 0x14048bf80 runs sim, camera, render recording, pass execution and Present on the main thread. **Deferred contexts: yes**, one per enkiTS worker for pass execution (`Engine.Renderer.RenderJobCount`), executed with `ExecuteCommandList` on the main thread. D3D hooks must cover deferred contexts. (static; confirm in a capture)
- Does the engine have its own history reset path for camera cuts? **No.** Only a shared TAA reset flag (`ClearTAA`, no callers) and RT recreation on resize/split change. We must reset on our trigger list.
- What is the exe's protection status? SteamStub wrapper only; `.text` is plaintext; no Denuvo or third-party anti-cheat; the engine's AntiCheatManager is inert offline (likely).

## Phase 0 findings that may change the approach (decide together)

- **Split-screen two-view path as the stereo vehicle.** `Renderer::DoUpdate` 0x1405addd0 can build two ViewContexts (cameras at `Renderer+0xe48/+0xe50`) and `DeferredRenderLoop::Render` renders both in one call, with per-camera-index TAA, last-frame, froxel and exposure history. Rendering eye k as camera index k would give Phase 5's synchronous stereo and Phase 6's per-eye history largely from the engine itself. Hazards: jitter is disabled in split mode, exposure adapts per slot (must be shared), split viewports are half-size (need full-size per-eye targets), per-split HUD.
- **Native Oculus/stereo code is dead.** `PlayAll::Game::Oculus` is never constructed and `ViewContext::InitFromCameraStereo` has no callers; it only supported one symmetric projection. Do not build on it.
- **Front pass.** The bike/rider foreground renders with its own projection (`Camera+0x8c` front FOV, `SetupZFrontViewContext` 0x1408df070); it needs the per-eye asymmetric projection too.
- **World Shift.** 2048-unit floating-origin rebases; cached world-space positions must be re-derived per frame.
- **Proposed loader (needs Balazs's OK):** option (b) with `dinput8.dll` as the neutral proxy (5 exports, static import, not a KnownDLL, no game copy in the folder), hooking `D3D11CreateDeviceAndSwapChain` (the game's only device entry point, statically imported, so `d3d11.dll` is already mapped when our DllMain runs) from the proxy's init. Keeps `d3d11.dll`/`dxgi.dll` free for ReShade-style tools. Not chosen: `version.dll`/`winmm.dll` (loaded by many system DLLs, more exports to forward), `hid.dll` (≈70 exports, used by the input stack).
