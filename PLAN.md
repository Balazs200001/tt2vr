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
- [ ] Phase 4 — Alternate-eye stereo + per-eye TAA history (milestone)
- [ ] Phase 5 — Synchronous stereo
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
3. Per-frame passes run **exactly once**: shadow maps, GPU simulations, exposure update, probes. Phase 5 decides whether they run inside eye L's render (eye R skips them) or are hoisted before the eye loop.
4. For eye `E` in `[L, R]`:
   1. Bind `ViewState[E]`: history resources, previous matrices, jitter index.
   2. View = inverse(`C'` · `Recenter` · `P_E` converted to engine space). Projection built from `F_E` with the engine's depth convention.
   3. Call the engine's per-view render.
   4. Copy pre-HUD scene color into eye `E`'s swapchain image.
   5. Store this frame's matrices in `ViewState[E]` as "previous".
5. HUD renders once into a UI texture, submitted as a quad layer.
6. Present hook: `xrEndFrame` (projection layer + quad layer), then present the desktop mirror.

If the engine has a separate render thread (Phase 0 finding), steps 2–6 run there, with the pose snapshot handed over per frame index.

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
| TAA history color (including engine ping-pong) | per-eye | ViewState textures, same desc as the engine's | unknown |
| TAA jitter index / offset | per-eye | ViewState; advances once per eye per frame | unknown |
| Previous view / projection / viewProj (jittered + unjittered) | per-eye | ViewState; feeds motion vectors and reprojection | unknown |
| Previous depth / Hi-Z (disocclusion, occlusion culling) | per-eye | ViewState | unknown |
| Temporal AO / SSR / volumetric accumulation | per-eye | ViewState | unknown |
| Motion blur camera matrices | per-eye | ViewState (object motion part is shared) | unknown |
| Per-object previous world transforms | shared | updated once per frame; guard against a second update | unknown |
| Auto-exposure / eye adaptation | shared | computed once per frame; both eyes use the same value | unknown |
| GPU particles / simulations | shared | once per frame | unknown |
| Shadow maps / cascades | shared | rendered once; cascades fitted to the combined eye frustum | unknown |
| Reflection / environment probes | shared | once per frame | unknown |

Why exposure is shared: two separate adaptation histories drift apart and give the eyes different brightness, which causes binocular rivalry. Exposure still adapts over time; there is one adaptation state for the head.

Why per-object previous transforms are shared: if the engine updates each object's "previous world matrix" during rendering, the second eye's render would see zero object motion. The update must happen once per frame.

### Mechanisms

Chosen per state in Phases 4 and 6 and recorded in `docs/re/temporal.md`.

- **A. Engine state swap (preferred where the state can be found).** Locate the engine's per-view temporal state: history texture pointers, previous matrices, jitter index, "history valid" flag. Keep one copy per eye; swap it in before eye E renders and save it afterwards. The engine's own ping-pong then runs inside each eye's state, and previous-matrix/jitter constants come out right automatically.
- **B1. Copy-in / copy-out (fallback, simplest).** Identify the temporal pass by shader hash. Immediately before its draw/dispatch: `CopyResource(ViewState[E].history → texture bound as history SRV)`. Immediately after: `CopyResource(pass output → ViewState[E].history)`. This works with engine ping-pong because it copies whatever is bound at that draw. Cost: two full-resolution copies per eye per temporal pass.
- **B2. Binding substitution (optimization of B1, only after B1 works).** Replace the history SRV and output RTV/UAV bindings with ViewState[E]'s views; no copies.
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
- [ ] Create `C:\dev\tt2vr\` (git, private remote) and `C:\dev\tt2vr-re\` (no git, back up locally). Set `TT2_GAME_DIR` to the game's install folder.
- [ ] Create `.claude/settings.local.json` (gitignored) with `permissions.additionalDirectories` for `<TT2_GAME_DIR>\tt2vr` and `C:\dev\tt2vr-re`, so Claude Code can read logs, dumps and captures.

**Toolchain**
- [ ] VS 2022 (Desktop C++), CMake ≥ 3.28, vcpkg, Git.
- [ ] RenderDoc, x64dbg, and a PE inspector (PE-bear or Detect It Easy).
- [ ] Ghidra 12.1.2 and JDK 21, then REA (`npm install --global rea-agents`). On Windows `rea setup` doesn't run, so:
  - set `GHIDRA_INSTALL_DIR`, `JAVA_HOME` and `REA_ANALYSIS_PROVIDER=ghidra`, then check with `rea doctor --json`;
  - register REA's MCP server with Claude Code manually (REA installation docs, MCP registration section);
  - raise `REA_GHIDRA_STARTUP_TIMEOUT_MS` if the exe times out;
  - fallback if Windows support is too limited: run REA + Ghidra under WSL2 (Linux x64 is REA's main platform) on the snapshot.

**Game facts → `docs/re/README.md`**
- [ ] Steam build ID, exe name, SHA-256. Copy the exe into `tt2vr-re/bin/<build-id>/`.
- [ ] Bitness and imports (d3d11/dxgi, input DLLs).
- [ ] Protection status: section names, entropy, Steam DRM wrapper, Denuvo, anti-cheat folders. **If the exe is packed or encrypted on disk, stop and discuss with me. Don't remove DRM.**
- [ ] In-game graphics options (AA modes including TAA, motion blur, resolution, FOV) and the config file location. Use the TAA setting for all captures.

**Frame anatomy (RenderDoc) → `docs/re/frame-anatomy.md`**
- [ ] Capture helmet-cam frames: daytime on the TT course, plus one in a menu.
- [ ] Pass table: event range, guessed name, RT formats and sizes, inputs, outputs, `per-frame` or `per-view`, temporal inputs.
- [ ] Threading: are deferred contexts / `ExecuteCommandList` used? Is there a separate render thread?
- [ ] Identify: TAA pass (jittered projection, history SRV slot, output), motion-vector pass, exposure pass, shadow passes, HUD start event.
- [ ] First pass at the temporal inventory table above, with real passes and resources.

**Conventions → `docs/conventions.md`**
- [ ] Depth: clear value, depth function (reversed-Z?), infinite far plane?
- [ ] Matrices: row or column major in constant buffers, handedness, units, up axis, camera-relative rendering?
- [ ] Back buffer format and color space.

**Loader decision → Decisions log**
- [ ] Requirement: our D3D11 device hooks must exist before the game creates its device, because shader bytecode is only visible at creation time.
- [ ] Option (a): proxy `d3d11.dll` or `dxgi.dll`. Simplest, but conflicts with ReShade-style overlays.
- [ ] Option (b): a neutral proxy from the import list (e.g. `version.dll`, `winmm.dll`, `dinput8.dll`) plus `LdrRegisterDllNotification` to hook `D3D11CreateDevice*` when `d3d11.dll` loads.
- [ ] Pick one and record why.

**Initial RE (Ghidra/REA) → `docs/re/symbols.md`**
- [ ] Strings and xrefs for: camera modes (helmet / first person), TAA, jitter, motion blur, exposure, render pass names, config keys.

**Done when:** `frame-anatomy.md` has a classified pass table; `conventions.md` is filled in with evidence; protection status is known; the loader is decided; the temporal inventory has a first pass.

## Phase 1 — Skeleton mod (flat, no VR)

- [ ] `CMakeLists.txt`, `CMakePresets.json` (VS 2022 generator, `x64-debug` / `x64-release`, test presets, `deploy` target), `vcpkg.json` with: minhook, spdlog, imgui (dx11-binding, win32-binding), tomlplusplus, directxtk (ScreenGrab), openxr-loader, xxhash, doctest.
- [ ] `.gitignore`: build output, `*.rdc`, dumps, `CLAUDE.local.md`, `.claude/settings.local.json`.
- [ ] Proxy DLL (per the Phase 0 decision): forwards all exports and loads `tt2vr\tt2vr.dll`. No heavy work in `DllMain` (loader lock); everything else happens on an init thread or the load notification.
- [ ] `core`: logger; TOML config including hotkeys; MinHook wrapper (install / enable / disable all); IDA-style pattern scanner with `??` wildcards; exe SHA-256 check. Unknown version → D3D layer only, engine hooks off, overlay notice.
- [ ] `d3d`: device and swapchain creation hooks, `Present` / `ResizeBuffers`, `Create*Shader` hooks feeding a shader registry (xxh64 of bytecode → stage, size, our name from `src/d3d/shader_names.inc`).
- [ ] Frame trace: on hotkey, log every relevant context call for one frame (shader binds with names, `OMSetRenderTargets`, SRV/UAV binds, viewports, draws/dispatches, copies, clears, constant buffer updates) to `captures/frametrace_<frame>.txt`.
- [ ] Texture dump: on hotkey, save tracked textures (PNG for UNORM, DDS for float formats).
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

## Phase 4 — Alternate-eye stereo + per-eye TAA history (milestone)

- [ ] `stereo::ViewState` and its resource lifecycle (created from engine texture descs, recreated on resize).
- [ ] Alternate-eye rendering: eye chosen by frame parity. Each projection view is submitted with the pose and FOV it was rendered with; the stale eye keeps its older pose so the runtime reprojects it correctly.
- [ ] Per-eye TAA: history color, previous matrices, jitter. Mechanism A if Phase 2 found the engine state, otherwise B1.
- [ ] `[debug]` tint injection for the cross-eye leak test.

**Done when:** stereo depth is correct in static scenes, and the leak, static convergence and cut tests pass for TAA.

## Phase 5 — Synchronous stereo (target architecture)

- [ ] Reverse engineer the per-view render entry and the per-frame work (shadows, sims, exposure, probes, HUD). Record the call graph in `docs/re/renderer.md`, mapping frame-trace ranges to functions.
- [ ] Hook the per-view entry and call the original once per eye with that eye's camera, projection and `ViewState`.
- [ ] Per-frame passes once: either skipped on eye R (reusing eye L's outputs) or hoisted before the eye loop. The frame trace must show each per-frame pass once per frame.
- [ ] Guard per-object previous transforms so they update once per frame.
- [ ] Re-entrancy audit: list the engine globals mutated by a per-view render; snapshot and restore any that break the second call.
- [ ] GPU timestamp queries per eye, shown in the overlay.
- [ ] Alternate-eye rendering kept only as a `[debug]` mode.

**Done when:** the log shows exactly one camera update between consecutive `xrEndFrame` calls with both eyes rendered after it; the leak and head-shake tests pass; per-frame passes appear once per frame in the trace.

**Fallback if the per-view entry can't be re-entered safely:** record eye L's scene-pass D3D11 calls and replay them for eye R with patched view constants and eye R's ViewState bindings. This is a large change; decide together before starting.

## Phase 6 — Temporal completeness

- [ ] Every inventory row implemented (per-eye or shared) and marked `confirmed` in `docs/re/temporal.md`, with evidence.
- [ ] History reset: hook the engine's own reset path (camera cut) if found; otherwise reset on the CLAUDE.md trigger list ourselves.
- [ ] Overlay debug views: each eye's history, motion vectors and previous depth; a `[debug]` "freeze eye R history" option.
- [ ] VRAM readout for ViewState resources.

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

## Decisions log

| Date | Decision | Reason |
|---|---|---|
| 2026-10-15 | Synchronous stereo is the target; alternate-eye is a milestone/debug mode only | both eyes must come from one simulation frame |
| 2026-10-15 | Temporal effects are never disabled; view-dependent state per eye, view-independent state shared | image quality, and exposure/sims must not diverge between eyes |
| 2026-10-15 | Repo outside the game folder; `deploy` target copies files; RE artifacts in `../tt2vr-re` (not in git) | no game material in git; survives Steam updates and file verification |
| 2026-10-15 | Hooking with MinHook; build with CMake + vcpkg + VS generator | simple, and scriptable by Claude Code |
| TBD (Phase 0) | Loader / proxy DLL | |
| TBD (Phase 4) | Temporal mechanism per state (A / B1 / B2) | |

## Open questions (answer in Phase 0)

- Does the game have TAA, and which other temporal effects exist?
- Is there a separate render thread? Are deferred contexts used?
- Does the engine have its own history reset path for camera cuts?
- What is the exe's protection status?
