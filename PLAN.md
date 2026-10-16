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

- [ ] Phase 0 — Recon & setup
- [ ] Phase 1 — Skeleton mod (flat)
- [ ] Phase 2 — Camera control (flat)
- [ ] Phase 3 — OpenXR, left eye head-tracked
- [ ] Phase 4 — Dual-view stereo via the split-screen path (milestone)
- [ ] Phase 5 — Synchronous stereo: shared per-frame work
- [ ] Phase 6 — Temporal completeness
- [ ] Phase 7 — HUD, menus, desktop mirror
- [ ] Phase 8 — Bike comfort & camera
- [ ] Phase 9 — Performance
- [ ] Phase 10 — Robustness & release

**Active phase: Phase 0**

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
- [ ] Capture helmet-cam frames: daytime on the TT course, plus one in a menu. **Blocked until Phase 1's first slice:** RenderDoc cannot capture the unmodified game. `EnableVideoAdapter` passes `Flags = 0x80` (`D3D11_CREATE_DEVICE_PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY`) to `D3D11CreateDeviceAndSwapChain`; RenderDoc treats that flag as "application requested not to be hooked" (its log, `d3d11_hooks.cpp(163)`), skips wrapping the device while DXGI is already wrapped, and the call returns S_OK with a null immediate context. The game then crashes at `TT2.exe+0x9592dd` (`mov r9,[rcx]`, QueryInterface on context slot 0). Confirmed 2026-10-16 from 3 CrashRpt minidumps (access violation reading 0, rcx = 0, loop index 0, returned feature level 11_1) and the RenderDoc log; NVIDIA NvCamera and the Steam overlay were ruled out. Fix: our proxy hooks `D3D11CreateDeviceAndSwapChain` and clears bit 0x80 before the call goes on (debug-layer registry behaviour only; no gameplay effect).
- [ ] Pass table (static draft in `frame-anatomy.md`; needs a capture): event range, guessed name, RT formats and sizes, inputs, outputs, `per-frame` or `per-view`, temporal inputs.
- [x] Threading (static: no render thread; pass execution on deferred contexts per enki worker): are deferred contexts / `ExecuteCommandList` used? Is there a separate render thread?
- [ ] Identify (static candidates found; confirm in a capture): TAA pass (jittered projection, history SRV slot, output), motion-vector pass, exposure pass, shadow passes, HUD start event.
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

- [ ] `CMakeLists.txt`, `CMakePresets.json` (VS 2022 generator, `x64-debug` / `x64-release`, test presets, `deploy` target), `vcpkg.json` with: minhook, spdlog, imgui (dx11-binding, win32-binding), tomlplusplus, directxtk (ScreenGrab), openxr-loader, xxhash, doctest.
- [ ] `.gitignore`: build output, `*.rdc`, dumps, `CLAUDE.local.md`, `.claude/settings.local.json`.
- [ ] Proxy DLL (per the Phase 0 decision): forwards all exports and loads `tt2vr\tt2vr.dll`. No heavy work in `DllMain` (loader lock); everything else happens on an init thread or the load notification.
- [ ] `core`: logger; TOML config including hotkeys; MinHook wrapper (install / enable / disable all); IDA-style pattern scanner with `??` wildcards; exe SHA-256 check. Unknown version → D3D layer only, engine hooks off, overlay notice.
- [ ] `d3d`: device and swapchain creation hooks, `Present` / `ResizeBuffers`, `Create*Shader` hooks feeding a shader registry (xxh64 of bytecode → stage, size, our name from `src/d3d/shader_names.inc`).
- [ ] Frame trace: on hotkey, log every relevant context call for one frame (shader binds with names, `OMSetRenderTargets`, SRV/UAV binds, viewports, draws/dispatches, copies, clears, constant buffer updates) to `captures/frametrace_<frame>.txt`.
- [ ] Texture dump: on hotkey, save tracked textures (PNG for UNORM, DDS for float formats).
- [ ] Remote requests: Claude writes a command file into `tt2vr/commands/`; the mod checks it once per frame and runs a frame trace, a texture dump, or — when the game was launched under RenderDoc (`renderdoc.dll` loaded) — a RenderDoc capture through the in-application API (`renderdoc_app.h`, `TriggerCapture`), written to the capture folder set in `tt2vr.toml`. Balazs only holds the game in the wanted state.
- [ ] ImGui overlay on the desktop window: FPS, hook status, version status, toggles.
- [ ] Unit tests: pattern scanner, config parsing.

**Done when:** 30 minutes of offline riding with the mod loaded, no crash; overlay works; shader registry and frame trace are produced; the unknown-version path is tested by faking the hash.

## Phase 2 — Camera control (flat)

- [ ] Find the view matrix:
  - search constant buffer updates for a 4×4 whose upper 3×3 is orthonormal and whose translation changes as the bike moves;
  - put a hardware write breakpoint (x64dbg) on its CPU-side source to find the writer function, then walk up the call chain in Ghidra/REA;
  - cross-check with the strings route from Phase 0.
- [ ] Document in `docs/re/camera.md`: the camera object fields we use; where world→view and projection are built (fovY, aspect, near, far, jitter); where the culling frustum comes from.
- [ ] `game::CameraOverride`: hook after the camera update; overlay sliders for yaw, pitch, roll and position offset.
- [ ] Projection override: inject an asymmetric frustum (e.g. shifted 20% right) and verify both the image and culling.
- [ ] Unit tests: projection-from-FOV builder against hand-computed values; XR→engine basis conversion.

**Done when:** I can turn the helmet camera 360° from the overlay with no culling holes and correct depth, and an asymmetric projection renders correctly at the frustum edges.

## Phase 3 — OpenXR, left eye head-tracked

- [ ] `xr`: instance with `XR_KHR_D3D11_enable`; log the active runtime name and version (`xrGetInstanceProperties`), since Meta and SteamVR runtimes behave differently.
- [ ] `xrGetD3D11GraphicsRequirementsKHR`: if the adapter LUID differs from the game device's adapter, log it and stay flat.
- [ ] Session on the game's device; `LOCAL` reference space; per-eye swapchains (format per the sRGB rule); a session state machine (idle, focus loss, stopping) that never blocks the game.
- [ ] Frame loop: `xrWaitFrame` / `xrBeginFrame` / `xrEndFrame` integrated with `Present`. Place `xrWaitFrame` so it throttles the start of the frame rather than stalling mid-render; measure.
- [ ] Force the game to render at the recommended per-eye resolution × config scale (cleanest route: game config file, window size, or `ResizeBuffers`).
- [ ] Left-eye-only mode: camera from `P_L`, projection from `F_L`, right eye black. This validates pose, projection and conversion exactly; a mono image shown to both eyes would match neither.
- [ ] Recenter via hotkey and overlay.

**Done when:** in the headset, a roadside sign I fixate on stays fixed while I rotate and lean my head (no swimming), and colors match the desktop.

## Phase 4 — Dual-view stereo via the engine's split-screen path (milestone)

Approach (proposed 2026-10-16, see "Phase 0 findings"): the engine renders up to two views per frame in one `DeferredRenderLoop::Render` call (`Renderer::DoUpdate` 0x1405addd0 builds ViewContexts from cameras at `Renderer+0xe48/+0xe50`) and keeps TAA, last-frame, froxel and exposure history per camera index. Eye L = camera index 0, eye R = camera index 1. Both eyes come from the same simulation frame, so this milestone is already synchronous. The original alternate-eye plan is kept as the fallback below.

- [ ] Static deep-dive before hooking (Ghidra + a split-screen RenderDoc capture): how split mode 1 is switched on at the **renderer** level only (view mask `RenderDesc+0xce8`, `Renderer+0xc90`, `RenderLoop::OnSplitChanged`) without `GameEngine::SetSplitMode`'s gameplay side effects (second player, `EffectorMapManager`, per-split CVar tables). Rule 5: visual only — if the renderer cannot be put in two-view mode without gameplay changes, stop and decide together.
- [ ] Second camera: drive `Renderer+0xe50` with our eye-R `Graphics::Camera` (a copy of the helmet camera node, moved by `P_R`); eye L uses the real camera moved by `P_L`.
- [ ] Full-size eye images. Preferred: render at width `2 × eyeWidth` with vertical split, so each half is one full-resolution eye; grab each view after `CopySplitRT` (RenderLoop vf+0x50). Alternative if the back buffer can't be that wide: patch `ComputeSplitSize` so each view gets a full-size target.
- [ ] Per-eye asymmetric projection from `F_E` for each ViewContext, using the reversed-Z RH form in `docs/conventions.md`. Cover every place that rebuilds a symmetric projection from camera FOV (`PrepareForCulling`/`RestoreAfterCulling`, `SetupZFrontViewContext` front pass, `FillViewVector`, `GetFrontPassInvProj`, CSM setup, sky, 3D HUD) — or hook `MatrixPerspectiveFovRH` 0x140813390 with the current view's eye.
- [ ] Per-eye TAA jitter: the engine disables jitter when two views exist; add the half-pixel offset per eye into proj [0][2]/[1][2] the way `DoUpdate` does, advancing once per frame.
- [ ] Shared exposure: slot 1's adaptation must use slot 0's luminance (tonemapper per-slot history `+0x138+idx*8`); verify with identical exposure in both eyes.
- [ ] Remove split-only visuals: `Split border` pass, per-split 2D HUD duplication (HUD handled in Phase 7).
- [ ] `[debug]` tint injection for the cross-eye leak test.

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
| TBD (Phase 4) | Temporal mechanism per state (C / A / B1 / B2) | |

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
