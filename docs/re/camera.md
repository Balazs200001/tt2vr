# Camera: data flow, view/projection construction, culling, and our override

All addresses refer to build 4989319 (`docs/re/README.md`). Names follow `symbols.md`. Matrix conventions (column vectors, row-major storage, reversed-Z) are in `docs/conventions.md`; this file does not repeat them. Confidence: `confirmed` / `likely` / `guess`; `(static)` = decompile only.

## 1. Per-frame data flow (main thread)

| Step | Where | What | Confidence |
|---|---|---|---|
| 1 | `CameraManager::OnPostManage` 0x14042efe0 | iterates cameras, calls `KTUCamera::Update` (vtable slot 1) | confirmed (static) |
| 2 | `KTUCamera::Update` 0x1409d9060 | active mode `Manage` (`call [mode+0x30]` at 0x1409d9125; helmet = `TTSpringCameraMode::Manage` 0x14019d700) produces a Matrix34; written to the camera node; `SetFov` 0x1409d93e4, `SetFrontFov` 0x1409d93f1, `SetClips` 0x1409d942e, `SetFrontClips` 0x1409d9447; Hor+ widening beyond 16:9 | confirmed (static) |
| 3 | `GraphNode::Update` call at 0x1409d94cf | camera world matrix final for the frame, in the pool `DAT_1413cda88 + node[+0x50]*0x30` | confirmed (static) |
| 4 | `GameContext::RenderOneFrame` 0x14048bf80, `call [rax+0x58]` at 0x14048c536 | `RendererWin32::OnUpdate` 0x140709070 -> `Renderer::DoUpdate` 0x1405addd0 | confirmed (static) |
| 5 | `DoUpdate` | split mode 0: `idx = ctz(Renderer+0xe58)`, `ViewContext::InitFromCamera(Renderer + idx*0x620 + 0x30, Renderer+0xe48[idx], viewport, jitter)`; then `LensFlareManager::Update` per active view | confirmed (static) |
| 6 | `Renderer::OnRender` 0x1405af470 -> `DeferredRenderLoop::Render` 0x1408469c0 | per view: `RenderContext::Prepare(const ViewContext&)` 0x1408cfad0 (= `ViewContext::operator=`), culling, passes | confirmed (static) |
| 7 | Present | end of frame | confirmed |

Rule: steps 1-3 are game logic (simulation side). Steps 4-6 are the renderer. Everything we change lives in 4-6.

## 2. `Graphics::Camera` fields we read

From `symbols.md` 5a; offsets mirrored in `src/game/engine_layout.hpp`.

| Field | Offset | Meaning | Confidence |
|---|---|---|---|
| projType | +0x58 (int) | 0 = perspective, else ortho (`MatrixOrthoRH`) | confirmed (static) |
| fov | +0x78 (float, rad) | **horizontal** fov (`m[1][1] = m[0][0] * aspect`) | confirmed (runtime f8323 via matrix; field itself to be read by our log) |
| near / far | +0x7c / +0x80 | clip planes; helmet camera 1.0 / 25000 at runtime (f8323) | confirmed |
| aspectMul | +0x84 | final aspect = `vpW / vpH * aspectMul` | likely |
| frontFov / frontNear / frontFar | +0x8c / +0x90 / +0x94 | front pass (bike/rider foreground) | confirmed (static) |
| frontPass | +0x98 (byte) | front pass enabled (with `RenderLoop+0x111c`) | confirmed (static) |
| node index | +0x50 (int, GraphNode) | index into the world-matrix pool | confirmed (static) |

## 3. Where world -> view and projection are built

`ViewContext::InitFromCamera(Camera*, Viewport&, Vector2& offset)` 0x14091aeb0 (confirmed, static decompile):

1. `ViewContext+0x450 = camera`; near/far to `+0x430/+0x434`; viewport to `+0x338..+0x34f`.
2. Reads the camera world Matrix34 from the pool. Translation (indices 3, 7, 11) to `camPos +0x0`; also to `cullPos +0xc` unless `g_bLockCullingQuery`.
3. Extends the Matrix34 to 4x4 with row (0,0,0,1) and inverts it (`Math::Matrix44_Inverse` 0x14046b6c0): that is the view.
4. Perspective: `Math::MatrixPerspectiveFovRH(proj, Camera+0x78, +0x7c, +0x80, vpW/vpH * Camera+0x84)`; then `proj[0][2] += offset.x`, `proj[1][2] += offset.y` (TAA jitter or Ansel offset), stored at `+0x448/+0x44c`.
5. `TransformMatrix::SetViewProjection(ViewContext+0x18, view, proj)` 0x140915260 = `RecomputeView` (view, world, rotation-only view) + `SetProjection` (proj, ViewProj, InvViewProj, InvProj terms).
6. `ViewContext::RebuildFrustum` 0x14091b4e0.

Other code that rebuilds a **symmetric** projection from the camera fields, all through `MatrixPerspectiveFovRH` (callers listed by Ghidra, 20 code refs; confirmed static):

| Caller | fov argument | Effect |
|---|---|---|
| `RenderLoop::PrepareForCulling` 0x1408dc850 | `max(fov, frontFov)`, `min` near, `max` far; jitter scaled by `fov / max` | widened projection installed via `ViewContext::SetProjection` for culling (only when the front pass is on) |
| `RenderLoop::RestoreAfterCulling` 0x1408ded00 | `fov` | re-installs the symmetric projection after culling |
| `RenderLoop::SetupZFrontViewContext` 0x1408df070 | `frontFov` | front-pass ViewContext at `RenderLoop+0x1590`: copy of the main ViewContext (same view) + own projection |
| `DeferredLighting::FillViewVector` 0x1408384c0 | `frontFov` | front-pass inverse projection for lighting reconstruction; main pass uses the ViewContext's slot 3 |
| `CascadedShadowMapRenderer::SetupRenderContext` 0x140828320, `SkyRenderer::Render` 0x140909c30, `HudRenderer::Render` 0x14088b530 | camera fov (likely) | cascade fitting, sky, 3D HUD |
| `DeferredLighting::FillLightRenderInstanceCollection` 0x140837cb0 | spot-light cone | light volumes; reads the **light's** node from the pool, not the camera (checked) |
| `KTUCameraFixed::Manage` 0x1409dcf20, `KTUCameraRailMode::Reset` 0x1409de730 | camera fov | game-side camera logic (step 1-3 timing, before `DoUpdate`) |

`Math::MatrixPerspectiveFovLH` 0x1408132f0 is a near-identical body (first difference at +0x33: `movaps xmm1,xmm7` vs `xmm6`); not used by the 3D scene as far as checked.

## 4. Culling frustum

- `ViewContext::RebuildFrustum` builds the frustum from slot 0 ViewProj plus clip bounds `+0x438..+0x444` (confirmed static). `VisibilityCuller::AddCullingData` 0x14091c850 copies the SIMD frustum `+0x3b0..+0x42f` and the cull position.
- With the front pass on, culling runs between `PrepareForCulling` (widened projection) and `RestoreAfterCulling`.
- Consequence: culling follows whatever view and projection are in the ViewContext at that point, as long as both the view fix-up and every `MatrixPerspectiveFovRH` result agree. Runtime check pending (Phase 2 test, section 6).

## 5. Who else reads the camera world matrix from the pool

`xrefs 0x1413cda88` gives ~380 readers. Relevant ones: `SoundListener3D::Manage` (audio), `PhysicsManager::UpdatePhysicFromSceneGraph`, `Mesh::_ComputeLOD`, `HudRenderer::Render`, `HHParticleEffectInstance::Update`, `KTUCamera*` modes. Rule that follows: **we never write the camera's pool matrix.** Writing it would move the audio listener and could feed back into physics or camera logic (CLAUDE.md rule 5). The renderer takes its view from the ViewContext, which we change after `InitFromCamera` returns.

Residual risk (guess, check at runtime): renderer code that reads the camera node from the pool instead of the ViewContext keeps using the unmodified camera (candidates: `Mesh::_ComputeLOD` LOD distance, `HudRenderer::Render`). Visible symptom: LOD or 3D HUD not following a large override (yaw 180 deg).

## 6. Our override (`game::CameraOverride`, `src/game/camera.*`)

| Hook | Signature | Thread | Behaviour |
|---|---|---|---|
| `Renderer::DoUpdate` 0x1405addd0 | export + pattern | main | pre: if split mode (`Renderer+0xe40`) is 0, main camera = `Renderer+0xe48[ctz(+0xe58)]`; publish its fov / front fov bits and the projection settings; open the render window |
| `ViewContext::InitFromCamera` 0x14091aeb0 | export + pattern | main | post, only inside `DoUpdate` and only for the main camera: record camera fields; if the view override is on: `world' = world * T(offset) * Ry(yaw) * Rx(pitch) * Rz(roll)` (camera-local), rewrite `camPos` (and `cullPos` if it equalled `camPos`), `SetViewProjection(view', existing proj)`, `RebuildFrustum` |
| `Math::MatrixPerspectiveFovRH` 0x140813390 | export + pattern | any | post, render window only, `fov` bit-equal to the main camera's fov or front fov: reshape m[0][0], m[1][1], m[0][2], m[1][2] (Phase 2: window shift; Phase 3: XrFovf tangents), keep the caller's near/far rows. Callers add jitter afterwards, so jitter is preserved |

The render window opens in `DoUpdate` and closes in our Present hook, so the game-side callers in section 3 (which run before `DoUpdate`) are never reshaped. Hooks are installed on the first Present after the exe hash is known (main thread, between frames). None of the five hooked/called functions has an ICF export alias (checked in `exports_4989319.tsv`).

Why the projection choke point instead of `ViewContext::SetProjection` after `InitFromCamera`: `RestoreAfterCulling`, `SetupZFrontViewContext`, `FillViewVector`, CSM, sky and HUD all rebuild a symmetric projection from the camera fields and would erase or contradict an installed matrix. All of them go through `MatrixPerspectiveFovRH`.

World Shift: the override is recomputed every frame from the current pool matrix; nothing is accumulated in world space.

Head tracking (Phase 3, `src/game/camera.cpp`, runtime test pending): in the `DoUpdate` pre-hook, `xr::LocateViews` gives the left eye's pose and `XrFovf` at the frame's predicted display time. The view becomes `world * sliders * EyeLocal(recenter, P_L)` (conventions 8.1 rule 1: camera-local, no basis change), the projection takes the eye's tangents at the same choke point (callers keep their near/far rows and add jitter afterwards), and the helmet uses the same local transform one physics step later.

## 7. Helmet mesh (HelmetView actor)

| Item | Value | Evidence | Confidence |
|---|---|---|---|
| Actor | `HelmetView_%d`, created from `ARCHETYPES:/GAMEPLAY/HELMETVIEW.PPF` by `FUN_1403b20a0`, stored at `cam+0x160` of TT2's player camera object; its mesh node `Helmet_01` cached at `cam+0x158`; visibility toggled by `FUN_1403ae6a0` / `FUN_1403af2b0` (`cam+0x360`) | decompile of `FUN_1403b20a0` (string xrefs 0x140f60c30, 0x140f60c58, "Helmet_01") | confirmed (static) |
| Placement | `FUN_1403afc80` (vtable slot at 0x140f60b00; TT2 camera class `AfterPhysicsStep`, calls `Game::Camera::AfterPhysicsStep` first): if the view enum at `cam+0x138` passes `FUN_1401847b0` and the camera-mode manager `DAT_14141e5c8` reports mode type 2, `Actor::SetWorldMatrix(cam+0x160, mode+0x4c4)` | decompile; only TT2-range code using immediate 0x4c4 besides the mode ctor, `HelmetSprings` and `FUN_14019d4b0` | confirmed (static) |
| Helmet matrix | `mode+0x4c4` (Matrix34) built by `TTCamera::HelmetSprings` `FUN_1401a04c0` from the mode's camera matrix `mode+8` (axes I/J/K, origin) plus pitch/yaw spring angles (`mode+0x4f4`, `+0x50c`) and the camera data's helmet position offset (`+0xe4`); fully recomputed each call, never read back | decompile | confirmed (static) |
| Other reader of `mode+0x4c4` | `FUN_14019d4b0`: origin minus `mode+0x524` (a distance), called from `FUN_14035f270`, `FUN_1403613e0` | disasm | likely; purpose unknown |
| Timing | physics step (CameraManager AfterPhysicsStep), i.e. before `KTUCamera::Update` and before `DoUpdate` | vtable slot / CameraManager slots 11/12 | likely |
| `Actor::SetWorldMatrix` 0x1403d77f0 | `mov rcx,[rcx+0x70]; jmp GraphNode::SetWorldMatrix` (actor node at `Actor+0x70`) | disasm | confirmed |

Our fix (Phase 2): hook `FUN_1403afc80` (pattern) and `Actor::SetWorldMatrix` (export); inside the former, for the HelmetView actor only, pass `M * local * M^-1 * H` (M = `mode+0x8`, the pivot the helmet was built from) to the game's own setter, so child meshes update the normal way. Runs in the physics step: while physics is paused the helmet does not follow slider changes, and with head tracking the helmet may trail the view by up to a frame (revisit with late latching if visible).

Consequence: the helmet is placed in the scene graph by game code, separately from the render view, so our ViewContext override (section 6) does not move it. Runtime (2026-10-16): with the view override on, the world view turns but the helmet stays fixed to the original camera. After the fix (below): helmet follows the view, verified by Balazs 2026-10-16.

## 8. Runtime checks (Phase 2)

| # | Check | How | Status |
|---|---|---|---|
| 1 | Camera field values in a race (hfov, near/far, aspect multiplier, front pass) | log line `camera: main view camera ...`; `tools/tt2vr-request.ps1 camera` | done 2026-10-16: helmet cam hfov 91.00 deg (1.5882 rad), near 0.01, far 25000, aspect mul 1, **front pass off** (front fov 90, near 1, far 1); proj m00 0.982697, m11 1.74702 at 1920x1080, vfov 59.57 deg; menu cameras: hfov 45 / 90 / 50, near 0.5, far 10000 |
| 2 | Camera world matrix: columns (right, up, back), Z-up, det +1 | same log | done 2026-10-16: right (-0.6214, 0.7835, -0.0026), up (-0.0551, -0.0405, 0.9977), back (0.7815, 0.6201, 0.0683); right x up = back (det +1), up z-dominant |
| 3 | View override reaches image, culling and front pass | overlay yaw 0 -> 180 deg | done 2026-10-16 (Balazs: all view overrides work, no artefacts reported); helmet mesh does not follow (section 7) |
| 4 | Projection reshape reaches image and culling | shift x 0.2 | done 2026-10-16 (Balazs: works) |
| 5 | Depth still correct | texture dump of depth with yaw 90 deg | not separately dumped; no depth artefacts reported |

Note: the helmet camera of f8323 had near 1.0; this session's helmet camera has near 0.01. Near differs per camera view (`GameViewCameras.cfg` data), so never hard-code it.

## 9. Automatic look into corners ("Dynamic View" / look to apex) and stick look (static, 2026-10-20)

`confirmed (static)` unless tagged. Helmet views (enum 2 Helmet, 3 HelmetNoMesh, 4 Dash) in `TTSpringCameraMode`.

- Physics step `PhysicsStepTarget` 0x1401a23e0 builds the look-at frame from the bike (horizon-level, yaw follows the bike), pre-multiplies `Q_lean * Q_tuck` (`LeanHead` 0x14019b880, `TuckInOut` 0x1401a50e0), smooths into quat `mode+0x3d4`, and stores the result at both `mode+0x8` and **`mode+0x3e4` (the pre-look frame)**.
- Every frame `ComputeView` 0x1401a11c0 copies `+0x3e4` to `+0x8` and calls `LookAroundInput` 0x1401a0a70, which (a) post-multiplies `Ry(theta)`, theta = `TTDriverAnim+0x120` (component `*(mode+0x430)`, updated by `FUN_1401738b0` toward a racing-line point ahead; max 0.25 / 0.20 / 0.15 / 0 rad for the profile option Anticipated / Normal / Delayed / Off, drift rate 0.3 rad/s), and (b) pre-multiplies the stick look `mode+0x2c0` (pad or look left/right/back; max angles CockpitSettings `+0xb4/+0xb8`). `HelmetSprings` then builds the helmet matrix from the post-look `+0x8`; `KTUCamera::Update` adds shakes and writes the pool.
- Game option: profile enum `eCameraDynamicView` (Anticipated/Normal/Delayed/Off; `FUN_14030f520`), probably the menu's "look to apex" setting (`likely`, Scaleform labels); also drives the rider's head-look IK (visual).
- Mod rule (`[camera] remove_auto_look`, default on, head-tracked only): `D = R(mode+0x8)^T * R(mode+0x3e4)` (camera-local, translation zeroed) is put before the head pose: view = W * D * sliders * eye; helmet local = D * head. Removes the auto yaw and the stick look, keeps lean/tuck and shakes; the pool, audio and the game option are untouched. Mode reached via the player-0 `KTUCamera` (`+0x48` mode with the `TTSpringCameraMode::Manage` vtable check, `+0x68` override mode null, `+0x10` = rendered camera). The removed yaw is shown by the `camera` command (`game look rotation removed: N deg`).
- Not removed: `LeanHead`'s own yaw/roll into the corner (CockpitSettings `LeanHeadRotationOffset` +0xc8, applied with the lean angle, baked into `+0x3e4`); separable only with hooks on `LeanHead` / `TuckInOut` (their output quats, call sites 0x1401a2c24 / 0x1401a2c7e).
