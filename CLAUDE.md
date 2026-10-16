# CLAUDE.md — tt2vr

## What this project is

`tt2vr` is a VR mod for **TT Isle of Man: Ride on the Edge 2** (PC, Steam app 1082180, KT Engine, Direct3D 11). It is a DLL injected into the game that:

1. renders the 3D scene **once per eye, every frame**, with per-eye view and asymmetric projection matrices from OpenXR (synchronous stereo is the end state; alternate-eye rendering is only a milestone/debug mode),
2. drives the game camera from the headset pose (6DoF, seated),
3. keeps **every temporal effect working, with per-eye history** (rule 1 below).

Headset: Meta Quest 3 over Link or Steam Link, through OpenXR. Developer: Balazs, graphics programming student. Roadmap, status and decisions live in `PLAN.md`.

## Start of every session

1. Read `PLAN.md` → **Status** and the active phase's tasks.
2. Read the `docs/re/*.md` notes relevant to the task. Read `docs/conventions.md` before touching any matrix, depth or color code.
3. Say which PLAN task you're working on before writing code.

## Hard rules

1. **Temporal rule.** Never disable or bypass a temporal effect (TAA, jitter, motion vectors, motion blur, temporal AO/SSR, previous-frame depth, temporal upscaling) to fix a stereo artifact.
   - View-dependent temporal state is duplicated **per eye** in `stereo::ViewState`.
   - View-independent per-frame state (auto-exposure, GPU sims, per-object previous transforms, shadow maps, probes) is updated **once per frame and shared** by both eyes. Shared is not disabled.
   - If an effect looks impossible to make per-eye, stop and show me the evidence.
   - Debug A/B bypass toggles are allowed only if default-off, labelled `[debug]`, and never used as the fix.
   - Full design, invariants and tests: `PLAN.md` → "Temporal design".
2. **No game material in git:** no binaries, assets, shader bytecode or disassembly dumps, Ghidra projects, decompiler output, RenderDoc captures. Those live in `../tt2vr-re/`, outside the repo. Short byte signatures and our own notes are fine.
3. **No hard-coded addresses.** Engine functions and globals are found by pattern scan through `src/game/signatures.hpp`. Each entry records: our name, pattern, how it was found, and the exe SHA-256 it was validated on.
4. **Fail safe.** A missing signature or failed hook logs an error and falls back to pass-through; the flat game must keep running. No C++ exceptions across hook boundaries.
5. **Visual only.** Never change gameplay, physics, timing, input or network behavior. Test in offline modes only.
6. **Hands off the game install** except through the `deploy` build target. Never patch the exe on disk.
7. **Ask first** before adding a dependency, changing the build system, or bending any rule above.

## Repo layout

```
tt2vr/                          ← this repo
├── CLAUDE.md  PLAN.md  README.md
├── CMakeLists.txt  CMakePresets.json  vcpkg.json  .gitignore
├── .claude/settings.json       shared Claude Code permissions (committed)
├── config/tt2vr.toml           default config, deployed next to the game
├── docs/
│   ├── conventions.md          matrices, depth, handedness, units, color: ground truth
│   └── re/
│       ├── README.md           exe build ID + SHA-256 that every finding refers to
│       ├── symbols.md          our names ↔ addresses (source of truth for names)
│       ├── frame-anatomy.md    pass table, per-frame vs per-view classification
│       ├── shaders.md          shader hash → our name, pass, notes
│       ├── camera.md
│       ├── renderer.md
│       └── temporal.md         every temporal resource/constant and how it's made per-eye
├── src/
│   ├── proxy/                  tiny forwarding DLL; loads tt2vr\tt2vr.dll
│   ├── core/                   log, config, hooks (MinHook), pattern scan, hashing, version check
│   ├── d3d/                    D3D11/DXGI hooks, shader registry, state tracker, frame trace, dumps
│   ├── xr/                     OpenXR instance/session, swapchains, frame loop, poses, layers
│   ├── game/                   signatures.hpp, engine struct mirrors, camera + render-entry hooks
│   ├── stereo/                 eye loop, ViewState (per-eye history), projections, xr_to_engine
│   └── ui/                     ImGui overlay, HUD capture → quad layer, desktop mirror
├── shaders/                    our HLSL (blits, debug views, tint test)
├── tools/                      helper scripts
└── tests/                      doctest unit tests for code that doesn't need the game

../tt2vr-re/                    ← NOT in git (local only)
├── bin/<build-id>/             snapshot copies of the game exe
├── ghidra/                     Balazs's Ghidra GUI project
├── rdc/                        RenderDoc captures
└── dumps/                      shader bytecode / disassembly
```

The game install only ever receives: `<proxy>.dll` in its root, and a `tt2vr/` folder containing `tt2vr.dll`, `tt2vr.toml`, `logs/`, `captures/`.

## Build, test, deploy

- Windows x64 only. MSVC (VS 2022), CMake ≥ 3.28, vcpkg in manifest mode (`VCPKG_ROOT` set), C++20. Presets use the Visual Studio generator so commands work from any shell.
- Configure: `cmake --preset x64-debug`
- Build: `cmake --build --preset x64-debug`. Run after every change. Our targets build with `/W4 /WX`; third-party code doesn't.
- Unit tests (no game needed): `ctest --preset x64-debug`
- Deploy: `cmake --build --preset x64-debug --target deploy` copies the proxy DLL, `tt2vr.dll` and `config/tt2vr.toml` into the folder named by the `TT2_GAME_DIR` environment variable. It fails while the game is running (DLL locked); tell me to close it. Never hard-code the game path.
- Presets and the deploy target are created in PLAN Phase 1.

## Runtime testing: you can't run the game

You cannot launch the game or see the headset. When a change needs runtime testing:

1. Build and deploy.
2. Give me a numbered test script (6 steps max): game mode, track, camera, what to do, what I should see.
3. Name the log lines that prove success or failure.
4. Don't mark a runtime "Done when" item complete until I've reported the result.

Artifacts you can read afterwards (relative to `TT2_GAME_DIR`; hotkeys are in `config/tt2vr.toml`):

- `tt2vr/logs/tt2vr.log`: main log.
- `tt2vr/captures/frametrace_<frame>.txt`: one frame's D3D11 call trace with shader names, render targets, SRVs/UAVs, viewports, draws, copies, constant buffer updates.
- `tt2vr/captures/<frame>_<eye>_<pass>.png|.dds`: per-eye texture dumps. You can open the PNGs (float formats also get a `.preview.png`). `<frame>_dump.txt` indexes them.
- `tt2vr/captures/shader_registry.txt`: every shader's xxh64, stage, size.

You can trigger these yourself while I hold the game in a state: `powershell -File tools/tt2vr-request.ps1 frametrace dump` (also `"capture <name>"` under RenderDoc, `shaders`). It prints the result paths.

When I report a visual bug, ask for a frame trace or texture dump (or a RenderDoc capture) before guessing.

## Reverse-engineering workflow

- Tools: Ghidra 12.1.2 + JDK 21 (my GUI project in `../tt2vr-re/ghidra/`), REA over MCP (headless and read-only; its Windows support is experimental), RenderDoc, x64dbg.
- REA analyzes the **snapshot** exe in `../tt2vr-re/bin/`, never the live Steam file.
- REA can't see names I add in the Ghidra GUI, so **`docs/re/symbols.md` is the source of truth for names.** Read it before analysis and add every new name to it.
- Every finding goes into `docs/re/<subsystem>.md` with: our name, address (for the exe hash in `docs/re/README.md`), evidence (xrefs, strings, RenderDoc event IDs, frame-trace lines, runtime logs), and confidence: `confirmed`, `likely` or `guess`.
- Never hook or patch based on a `guess` without a runtime check first.
- Don't paste large decompiled functions into docs or comments. Summarize the behavior; keep snippets short.

## Code conventions

- Namespaces match folders: `core::`, `d3d::`, `xr::`, `game::`, `stereo::`, `ui::`.
- COM through `Microsoft::WRL::ComPtr`; no manual `AddRef`/`Release`.
- Math: DirectXMath. Row/column major, handedness, reversed-Z and units are documented in `docs/conventions.md`. Verify against a captured matrix; never assume.
- Logging: `TT2VR_LOG_*` macros (spdlog). Per-frame logging only behind a verbosity flag.
- Every hook gets a comment naming the thread that calls it (game, render or OpenXR). Cross-thread data goes through an explicit per-frame handoff keyed by frame index.
- Small commits, one change each, prefixed by area, e.g. `stereo: per-eye TAA history copy-out`.

## Graphics gotchas

- Shader bytecode is only visible at `Create*Shader` time, so device hooks must be in place before the game creates its device.
- sRGB: the game's back buffer holds gamma-encoded values. Copy the bits raw (`CopyResource` / `CopySubresourceRegion`) into an `_SRGB` swapchain image of the **same typeless family** (R8G8B8A8 and B8G8R8A8 are different families). If no matching format exists, blit with a shader that doesn't encode again.
- The game renders at the per-eye resolution (forced via config), so the engine's own render targets match the eye swapchains.
- Per-eye projection is asymmetric (from `XrFovf`). Engine culling may use its own symmetric FOV; check before assuming culling follows our matrix.
- OpenXR is right-handed, +Y up, −Z forward, meters. Convert to engine space in exactly one place: `stereo/xr_to_engine.*`.
- Poses come from `xrLocateViews` at the frame's `predictedDisplayTime`, applied as late as possible before the scene render. Each projection view is submitted with the pose and FOV it was actually rendered with.
- Reset per-eye history on: camera cuts, race restart, recenter, resolution or format change, IPD/world-scale change, stereo-mode toggle.

## Working with me

- For anything large (new subsystem, more than ~200 lines, new hooks), propose a short plan first and wait for my OK.
- Explain with explicit rules, invariants and if/then conditions, anchored to concrete reference points (addresses, event IDs, matrix values, frame indices). Avoid "picture this" spatial descriptions.
- Skip rendering basics. For RE steps, show the evidence chain so I can verify and repeat it myself.
- If something is unclear or the evidence is ambiguous, ask me instead of guessing.
- When a task is done or a decision is made, update `PLAN.md` (checkboxes, Decisions log).

## References

- openRBRVR, the closest similar project: open-source VR mod for Richard Burns Rally (OpenXR and OpenVR; the D3D9 game renders through a dxvk fork's Vulkan backend; loads as an RBR plugin; TOML config). I worked on its FSR1 upscaling. Use it as a reference for how a game-specific VR mod is structured. It is MPL-2.0, so any file we copy from it stays MPL-2.0: https://github.com/Detegr/openRBRVR
- UEVR, stereo injection and per-view state handling: https://github.com/praydog/UEVR
- OpenXR 1.1 specification: https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html
- REA (Windows notes in `docs/windows-ghidra-p0.md`): https://github.com/morluto/rea
- RenderDoc: https://renderdoc.org
