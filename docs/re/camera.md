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

## 7. Runtime checks (Phase 2)

| # | Check | How | Status |
|---|---|---|---|
| 1 | Camera field values in a race (hfov, near/far, aspect multiplier, front pass) | log line `camera: main view camera ...` on first sight; `tools/tt2vr-request.ps1 camera` | pending |
| 2 | Camera world matrix: columns (right, up, back), Z-up, det +1 | log line `camera world: ...` / `camera` request; compare with cbCameraStd of the same frame | pending |
| 3 | View override reaches image, culling and front pass | overlay yaw 0 -> 180 deg; no holes, bike/rider consistent | pending |
| 4 | Projection reshape reaches image and culling | shift x 0.2: image moves left, no culling holes at the right edge, front pass consistent, `N calls reshaped` > 0 | pending |
| 5 | Depth still correct | texture dump of depth with yaw 90 deg | pending |
