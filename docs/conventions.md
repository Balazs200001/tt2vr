# Conventions: matrices, depth, handedness, units, color

Ground truth for every matrix, depth and color decision in `stereo/` and `xr/`. All addresses refer to build 4989319 (`docs/re/README.md`).

Rules for reading this file:

- Confidence is `confirmed`, `likely` or `guess`, as in `CLAUDE.md`. `(static)` means it comes from decompile/disassembly only and has not been seen at runtime. Nothing in this file has been checked in a RenderDoc capture yet, so every row has a "RenderDoc check" that would promote it.
- Do not write matrix code from a `likely` or `guess` row without running its check first (CLAUDE.md: verify against a captured matrix; never assume).
- "Engine" means the KT Engine (PlayAll). "XR" means OpenXR (right-handed, +X right, +Y up, -Z forward, meters).

## Quick reference

| Topic | Rule |
|---|---|
| World frame | Right-handed, **Z up**, **+Y forward**, +X right, 1 unit = 1 m (likely) |
| View space | Right-handed, camera looks down **-Z**, +Y up, +X right (same basis as OpenXR view space) |
| Math | Column vectors (`clip = M * v`), arrays stored row-major (`M[r][c]` at `r*4+c`), translation at 3,7,11 (3x4) or 3,7,11 of the 4x4 |
| Projection | `MatrixPerspectiveFovRH`: RH, **reversed-Z**, **finite far**, D3D clip z in [0,1], `w = -z_view`, fov argument is **horizontal** |
| Depth | Clear **0.0**, default test **GREATER_EQUAL**, ndc z = 1 at near, 0 at far |
| Shader camera constants | **Camera-relative**: ViewProj = Proj * (rotation-only View); camera position passed separately |
| Back buffer | `R8G8B8A8_UNORM_SRGB`, 1 buffer, DISCARD, windowed |
| XR to engine | One fixed basis change `B` (see "OpenXR to engine"), applied in `stereo/xr_to_engine.*` only |

---

## 1. World space

| Item | Value | Evidence | Confidence | RenderDoc / runtime check |
|---|---|---|---|---|
| Axes | right = (1,0,0), up = (0,0,1), forward = (0,1,0) | `ansel::setConfiguration` argument block built in `Renderer::Create` 0x1405ace80 (call at 0x1405adba0); basis vectors in the config struct | likely. The values are read from code, but the Ansel Configuration field order was not checked against the SDK header (headers could not be fetched). Treat as unverified until a runtime matrix dump agrees. | Break at `GraphNode::SetWorldMatrix` 0x14086ecd0 for the helmet camera node; dump 12 floats at `DAT_1413cda88 + node[+0x50]*0x30`. With the bike heading along +Y on a level road, expect rotation columns (right, up, back) = (1,0,0), (0,0,1), (0,-1,0). |
| Handedness | Right-handed | `right x up = (1,0,0) x (0,0,1) = (0,-1,0) = -forward`. Also: RH projection (`w = -z`), and the Ansel adapter quaternion maps Z-up/Y-forward into the camera's -Z-forward frame | likely (same single source as Axes; two independent hints agree) | Same matrix dump: determinant of the 3x3 rotation must be +1 and `col0 x col1 = col2`. |
| Up axis | +Z | same as Axes | likely | same |
| Units | 1 world unit = 1 metre | `metersInWorldUnit = 1.0` in the Ansel config; no other unit evidence | likely (static only; the notes say "not directly confirmed") | Measure a known length in a capture (bike wheelbase ~1.4 m, rider eye offset) in vertex positions of the bike mesh. Camera note suggests: log helmet camera offset from bike origin and compare to a real figure. |
| Floating origin | World re-centred in **2048-unit steps** on X or Y ("World Shift") | `GameEngine::UpdateShift` 0x1404d5630, log line `World Shift (%f %f)` (65 shifts in two races; deltas always one of (0,+-2048), (+-2048,0); first at race start was (2048, -8192)); grid size from `GameEngine+0x84..0x8c` | confirmed | Frame trace across a shift frame: the camera world translation (matrix idx 3,7,11) jumps by exactly 2048 on one axis while the scene stays visually continuous. |
| What a shift moves | `GraphNode::ShiftOrigin` 0x14086eee0 translates **both** world-matrix arrays (the pool at `DAT_1413cda88` and `_s_oInstance`) by `-shift`; physics, terrain, spawners, particles, world also shift. Each `ViewContext` is rebuilt via `InitFromCamera` with **zero** jitter | `GameEngine::UpdateShift` callees (wf4_temporal, wf4_camera) | confirmed (static) | Check frame N-1 vs N cbCameraStd camera position at a shift. |
| What a shift does NOT move | lastVP matrices (camera-relative, so unaffected), TAA/SSR/froxel/exposure history textures, per-object previous world matrices (`Mesh+0xf4`, likely one frame stale, guess). Only the stored last-frame **camera positions** are corrected by `RenderLoop::PreUpdateLastFrameCamera` 0x1408dc6b0 | wf4_temporal | confirmed for PreUpdate, guess for stale `Mesh+0xf4` | On a shift frame, inspect the motion-vector RT for streaks on static geometry. |

Rules that follow:

- A head-pose offset applied to the camera must be re-derived each frame from the current camera node translation. Never accumulate it in world coordinates across frames; after a shift the node translation changes by 2048 and any cached world-space position is wrong.
- Per-eye history that stores world-space positions must subtract the shift delta in the same place the engine does (`PreUpdateLastFrameCamera`). Camera-relative matrices need nothing.

---

## 2. View space

| Item | Value | Evidence | Confidence | Check |
|---|---|---|---|---|
| Camera local basis | +X right, +Y up, looks down **-Z** (OpenGL/OpenXR style) | RH projection with `w = -z_view` (`MatrixPerspectiveFovRH` m[14] = -1); `UpdatePerCameraCB` 0x14091b910 negates the third column of the camera world matrix to get the view-forward vector; Ansel adapter quaternion q11 = rotation (0,0,1)->(0,1,0) in `AnselCam::UpdateFromAnsel` 0x1403f1a10 | likely (derived from the math; not seen in a captured matrix) | In cbCameraStd (+0x60) the forward vector should equal minus the camera world matrix's third column. |
| View matrix | inverse of the 4x4-extended camera world matrix | `TransformMatrix::RecomputeView` 0x140914ff0 (view at +0, inverse at +0x40, rotation-only at +0x80); general inverse `FUN_14046b6c0` (our name `Math::Matrix44_Inverse`) | confirmed (static) | `View * World = I` on the dumped matrices. |
| Camera world position | matrix indices 3,7,11 of the Matrix34; copied to `ViewContext+0x0..+0x8` | `ViewContext::InitFromCamera` 0x14091aeb0 | confirmed (static) | Equals the camera position float3 in cbCameraStd +0x40. |

Local-frame equivalence: the engine camera frame and the OpenXR view frame are the same basis (X right, Y up, -Z forward). Only the **world** frames differ (Z-up vs Y-up). This is what makes the XR conversion a single rotation `B` (section 8).

**Conflict between notes.** The Ansel note says `ViewContext+0x18` gets the inverse of the world matrix "transposed into row-vector form". The projection/storage note shows `TransformMatrix::SetProjection` 0x1409150d0 computing `ViewProj = P * V` through the row-major `A*B` routine `FUN_14046baa0` with `V` used as-is, i.e. column-vector form. The column-vector reading is better supported (a transposed view would not compose as `P * V` with that routine; the matrix at `Camera world` has translation in column 3 and the projection has its w-row at [12..15] = (0,0,-1,0)). Treat the "transposed" wording as unverified.

---

## 3. Projection

### 3.1 Formula

`PlayAll::Math::MatrixPerspectiveFovRH` at **0x140813390**, our name `Math::MatrixPerspectiveFovRH`, signature `(out, hfov_rad, near, far, aspectScale)`.

| Float index (row-major `r*4+c`) | Element | Value | Role |
|---|---|---|---|
| m[0] | [0][0] | `1 / tan(fov/2)` | x scale |
| m[5] | [1][1] | `m[0] * aspect` | y scale |
| m[10] | [2][2] | `n / (f - n)` | z scale |
| m[11] | [2][3] | `f*n / (f - n)` | z offset |
| m[14] | [3][2] | `-1` | `w = -z_view` |
| m[15] | [3][3] | `0` | |
| m[2], m[6] | [0][2], [1][2] | 0 in the base matrix | off-axis / jitter terms (3.5) |
| all others | | 0 | |

Derived relations (our algebra from the elements above; consistent with the clear/compare evidence in section 4):

- With `d = -z_view` (positive distance): `ndc_z = n*(f - d) / (d*(f - n))`. So `ndc_z = 1` at `d = n`, `ndc_z = 0` at `d = f`, and it decreases monotonically with distance.
- Inverse: `d = f*n / (ndc_z*(f - n) + n)`.
- The engine's own linearisation uses `TransformMatrix::GetInvProjData` (elements [10],[11],[14],[15] of the inverse projection at `TransformMatrix+0x180`): `viewZ = (i10*z + i11) / (i14*z + i15)`.

| Row | Evidence | Confidence | RenderDoc check |
|---|---|---|---|
| Formula above | Decompile of 0x140813390: `fVar2 = far/(near-far)`; m[10] = -fVar2-1; m[11] = -(fVar2*near); m[14] = -1; m[15] = 0; m[0] = 1/tan(fov*0.5); m[5] = m[0]*aspectScale. Caller `InitFromCamera` passes `(Camera+0x78, +0x7c, +0x80, (vpW/vpH)*Camera+0x84)` | confirmed (static decompile, runtime-check pending) | VS cbuffer `cbCameraStd` (3.6): m[11] should equal `f*n/(f-n)`, m[14] = -1. With n = 0.5, f = 10000 (struct defaults; real values unknown) m[10] ~ 0.00005 and m[11] ~ 0.50003. |
| `Camera+0x7c` = near, `+0x80` = far | `Camera::GetClipNear` 0x14012ddc0 reads `[rcx+0x7c]`, `GetClipFar` 0x14012ec40 reads `[rcx+0x80]`, `SetClips` 0x140821cb0. Copied to `ViewContext+0x430/0x434` | confirmed | Debugger: break at `InitFromCamera` 0x14091aeb0, dump `Camera+0x78..0x94`. |
| Finite far | `f` is a real parameter in m[10]/m[11]; no infinite-far variant | confirmed | ndc_z of geometry at the far plane = 0 exactly. |
| D3D clip range [0,1] | m[10]/m[11] map near->1, far->0 | confirmed (static) | Depth buffer values in [0,1]. |
| Orthographic variant `Math::MatrixOrthoRH` 0x140813260 | `Camera+0x58 != 0` selects ortho; param order looks odd (near = 0.0 as argument 4); also reversed-Z | likely; no 3D scene pass is known to use it | Not needed for VR; only check if a pass shows a different projection. |

### 3.2 Horizontal vs vertical FOV

**The fov argument is horizontal.** Evidence chain:

1. `m[5] = m[0] * aspect` with `aspect = viewportW/viewportH * Camera+0x84`. A vertical-fov matrix would have `m[0] = m[5] / aspect`. Here the x scale is the primary term. This is direct decompile evidence at 0x140813390 (confirmed static).
2. `KTUCamera::Update` 0x1409d9060 applies a Hor+ widening for aspect ratios beyond the 16:9 reference (constant 1.7777778, or `ComputeTripleScreenAspectRatio`): `atanf(W / ((H*ref)/tan)) * 2`. It adjusts the **horizontal** angle so the vertical extent stays constant.
3. The Ansel bridge `FUN_1403f1a10` sends `Camera+0x78 * 57.29578` as the fov and the Ansel config `fovType = 0` (kHorizontalFov if the enum is {horizontal, vertical}; enum unverified).
4. CVar defaults `Engine.Camera.HorizontalFov` = 1.25 rad and `VerticalFov` = 0.75 rad. At 16:9, `2*atan(tan(0.625) * 9/16)` = ~0.75, so the pair is self-consistent as hfov/vfov. Those CVars are registered in `FUN_1400ba090` / `FUN_1400ba1d0` and have no other readers found, so they do not drive the main path.

**Conflict.** `wf4_camera.md` labels `Camera+0x78` "vertical FOV in radians" (struct map and the `Graphics::Camera` finding). That label was an early guess. The decompile evidence in 1 and 2 outranks it: use **horizontal**. Confidence: likely (static; the code path is clear, but no runtime value of `Camera+0x78` was compared to a measured frustum). Camera struct default 0.7854 rad (45 degrees) is only the constructor default, so it cannot settle this.

| Check | How |
|---|---|
| Confirm horizontal | In a capture, compute `atan(1/m[0])*2` and `atan(1/m[5])*2` from cbCameraStd. At 16:9 the horizontal value is the larger; the Hor+ formula in `KTUCamera::Update` should reproduce `Camera+0x78` from the CVar. Or: debugger read of `Camera+0x78` at 0x14091aeb0, then compare against `2*atan(1/m[0])`. |

Consequence for VR: do not pass an XR fov through `Camera+0x78` and rely on the engine's Hor+/aspect logic; per-eye asymmetric fov cannot be expressed there. Build the matrix ourselves (section 8).

### 3.3 Aspect multiplier

| Item | Value | Evidence | Confidence | Check |
|---|---|---|---|---|
| `Camera+0x84` | aspect multiplier; final aspect = `(vpW / vpH) * Camera+0x84`; default presumably 1.0 (struct default per the camera note) | `InitFromCamera` call; `Camera::SetAspectRatio` 0x140821c90 | likely | Dump at 0x14091aeb0. `m[5]/m[0]` should equal `vpW/vpH` when the multiplier is 1. |
| Open question | Effect of `+0x84` and the triple-screen logic at VR aspect ratios on the HUD/helmet camera is unknown | wf4_projection_depth open questions | guess | Only relevant if the engine's own projection path is kept. |

### 3.4 Front pass projection

| Item | Value | Evidence | Confidence | Check |
|---|---|---|---|---|
| Separate projection for bike/rider foreground | `MatrixPerspectiveFovRH(Camera+0x8c front fov, +0x90 front near, +0x94 front far, aspect, ...)`, same view matrix, same jitter, enabled by `Camera+0x98` (and `RenderLoop+0x111c`) | `RenderLoop::SetupZFrontViewContext` 0x1408df070; separate `ViewContext` at `RenderLoop+0x1590`; `DeferredLighting::GetFrontPassInvProj` 0x140839010. Struct defaults: front fov 0.7854, near 0.1, far 10 | confirmed (static) for structure; likely for details | Capture passes `Opaque Front`, `AlphaTest Front`, `Motion Vectors front pass`; check the projection used there differs from the main pass in m[0], m[10], m[11]. |
| Viewport depth range of the front pass | MinDepth = 1.0, MaxDepth = `1.0 - RenderLoop[0x1118]`; field defaults to 0 from the ctor (`RenderLoop` ctor 0x1408d92ba), so likely MaxDepth = 1.0 too | `SetupZFrontViewContext` | likely, **needs RenderDoc confirmation** | Viewport state on the front-pass draws; also whether depth is cleared between main and front pass. |
| Per-eye consequence | The front pass has its own fov and near/far. For VR it must use the **same eye asymmetry** (same m[0]/m[5]/m[2]/m[6] shape) with its own n/f in m[10]/m[11]. Using the main-pass matrix for the front pass is wrong whenever front n/f differ | derived | rule | Compare the two matrices in a capture. |

### 3.5 Off-axis terms: jitter and Ansel offset

Both go through the same slot: a `Vector2` (3rd argument of `ViewContext::InitFromCamera` 0x14091aeb0).

| Item | Value | Evidence | Confidence | Check |
|---|---|---|---|---|
| Offset elements | `proj[2]` (row 0, col 2) `+= offset.x`; `proj[6]` (row 1, col 2) `+= offset.y`. Stored at `ViewContext+0x448/0x44c` (field `ViewContext.projOffset`) | `InitFromCamera` decompile (`local_80 += offset.x; local_70 += offset.y`) | confirmed (static) | cbCameraStd +0x110 holds the offset; the projection's [0][2],[1][2] must equal it. |
| Effect on NDC | Column 2 multiplies `z_view`, and `w = -z_view`, so `ndc.x = m0 * (x/-z) - m[2]`. The offset therefore shifts NDC x/y by **minus** the offset. | our algebra from the matrix form; the Ansel note states the same ("shifts NDC by minus the offset"); the projection note left the sign "to be confirmed" | likely (derived) | Render a point on the optical axis with a known offset and read its ndc x. |
| Jitter pattern | **Not Halton.** Two-phase alternating: even frames `(1/viewportW, 0)`, odd frames `(0, 1/viewportH)`. Since NDC spans 2, `1/W` is half a pixel | `Renderer::DoUpdate` 0x1405addd0; parity counter `DAT_1413a88c4` (our name `jitterFrameCounter`, likely) | confirmed (static), runtime-check pending | Consecutive TAA frames: [0][2] = 1/W then 0, [1][2] = 0 then 1/H. |
| Jitter enable | AA mode `DAT_141407dc8` (0 none, 1 FXAA, 2 FXAA+TAA, 3 TAA) in {2,3}, physics not paused, and no split-screen mode active (`!(splitMode != 0 && Renderer+0xc60 != 0)`). Counter holds while dt == 0 | `DoUpdate`, `IsUsingTAA` 0x1404fd660 | confirmed (static) | Toggle AA in the game options; jitter terms vanish for modes 0/1. |
| Jitter in split screen | Disabled in split mode (hazard if split mode is used as the stereo route) | wf4_renderloop / wf4_temporal | confirmed (static) | n/a |
| Other callers | `RenderContext::Prepare` 0x1408cfae0 and `OnRender` call `InitFromCamera` with a zero offset; `UpdateShift` re-inits with zero | wf4_projection_depth, wf4_temporal | confirmed (static) | n/a |
| Ansel path | While an Ansel session is active (`Renderer+0x19`), the offset comes from `GameEngine::GetAnselProjectionOffset` 0x1404b36a0 (returns `AnselCam+0x54`) instead of the jitter | `DoUpdate` | confirmed (static) | n/a (Ansel session not run) |
| Un-jittered projection | `ViewContext::GetProjectionNoOffset` 0x14091adb0 returns proj minus the offset (used by `RenderTAA` and last-frame camera). `ViewContext::ResetProjectionOffset` 0x14091b570 subtracts it; called at the end of each view after `PostUpdateLastFrameCamera` | wf4_projection_depth, wf4_temporal | confirmed (static) | n/a |
| Shader-side jitter | `g_jitter` (only written in `RenderTAA` 0x1408ddf70) = `(cur.x*0.5, cur.y*-0.5, prev.x*0.5, prev.y*-0.5)`: the NDC offsets converted to UV units (y flipped); previous from `RenderLoop+0xf70+idx*8` | wf4_temporal | confirmed (static) | TAA resolve cbuffer. |

Rules for VR:

- The Ansel-offset slot is the engine's own off-centre-projection hook, but it is a single Vector2 **added** to the symmetric matrix. For XR asymmetric fov, writing `m[0]`, `m[5]`, `m[2]`, `m[6]` directly (section 8) is cleaner; combine with jitter as `m[2] = m2_fov + jitter.x`, `m[6] = m6_fov + jitter.y`, so the engine's sign convention for jitter is preserved. Per-eye jitter phase must be kept per eye (CLAUDE.md rule 1).
- Both eyes drawn within one frame must not share the "history jitter" slot (`RenderLoop+0xf70+idx*8`); it is indexed by the engine's camera index (see `docs/re/temporal.md`).

### 3.6 Projection storage inside `ViewContext`

See section 5 for the slot layout. `ViewContext::SetProjection` 0x14091b7e0 stores the projection and recomputes ViewProj/InvViewProj/InvProj terms through `TransformMatrix::SetProjection` 0x1409150d0; it also writes the second `TransformMatrix` block when `ViewContext+0x466 != 0`.

---

## 4. Depth

| Item | Value | Evidence | Confidence | RenderDoc check |
|---|---|---|---|---|
| Clear value | **0.0** (far in reversed-Z) | Single `ClearDepthStencilView` (ctx vtable +0x1a8) in wrapper `FUN_140948b50` at 0x140948d94; `XORPS XMM3,XMM3` at 0x140948d7f; flags depth=1, stencil=2. The same wrapper calls `ansel::markBufferBind` | confirmed (static) | `ClearDepthStencilView` event: depth value 0. Note: only one call site was found, so the front-pass depth clear question is separate (3.4). |
| Default comparison | **GREATER_EQUAL** | Depth-func table at `this+0x35c` filled by `FUN_140949920` (engine enum 0..7 mapped to D3D11 NEVER, LESS, EQUAL, LEQUAL, GREATER, NOT_EQUAL, GEQUAL, ALWAYS); `DepthStencilState` ctor 0x1408e0fd0 default argument 6 (call in `FUN_14094d1c0` with `..., true, true, 6`) selects entry 6 = GREATER_EQUAL | confirmed (static) | `OMSetDepthStencilState` on opaque draws: DepthFunc = GREATER_EQUAL. Other passes may use other funcs; the engine state is a 64-bit bitfield (bit 37 write, bit 38 test, bits 39-41 func). |
| Depth state creation | `FUN_14094d1c0` (our name `DX11 DepthStencilState cache`) builds `D3D11_DEPTH_STENCIL_DESC` from the 64-bit state; `CreateDepthStencilState` via device vtable +0xa8, `OMSetDepthStencilState` via context +0x120 | wf4_projection_depth | confirmed (static) | n/a |
| ndc_z | 1 at near, 0 at far | section 3.1 | confirmed (static) | Near geometry (bike) writes depth near 1.0. |
| Depth formats supported | `VideoDeviceDX11::GetDepthStencilTextureFormat` 0x140957400: `'FL32'` -> R32_TYPELESS / D32_FLOAT; `'ZB16'` -> R16_TYPELESS / D16_UNORM; `'32S8'` -> R32G8X24_TYPELESS / D32_FLOAT_S8X24; `'24S8'` -> R24G8_TYPELESS / D24_UNORM_S8 | wf4_projection_depth | confirmed (mapping) | n/a |
| Format of the **main** depth target | **Not determined.** Hint: 'Last Frame Depth' is created as `'FL32'` (R32F) by `InitializeLastFrameBuffers` 0x1408db790, and the TAA/SSR code treats the scene depth as a float; a stencil is used (TAA "No motion Vector Fix" is stencil-gated, `Clear Stencil` pass), which points at `32S8` or `24S8` | wf4_temporal, wf4_hud_ui | guess | Resource inspector on the depth target bound at the `ZPrepass` event. |
| Front-pass depth | Same depth buffer; viewport range squeezed to [1.0, 1.0 - RenderLoop[0x1118]] | 3.4 | likely | Viewport at `Opaque Front`. |

Rules:

- Anything we create that is compared or cleared against the engine's depth (debug views, our own passes, depth submitted to OpenXR) must use clear 0.0 and GREATER_EQUAL, and treat larger values as nearer.
- If depth is submitted with `XR_KHR_composition_layer_depth`, the swapchain depth follows the same reversed convention: nearZ/farZ in the layer must match the engine n and f and the reversed mapping. Unverified, depends on the runtime; check Link/Steam Link support before relying on it.
- Linearise with the formula in 3.1, not the textbook `(2nf)/(f+n-z(f-n))`.

---

## 5. Matrix storage and multiply order

### 5.1 Conventions

| Item | Value | Evidence | Confidence | Check |
|---|---|---|---|---|
| Math convention | **Column-vector**: `clip = M * v`, `ViewProj = Proj * View` | `TransformMatrix::SetProjection` 0x1409150d0 calls `FUN_14046baa0` with `(this+0x100, proj, view)` = `P*V`; projection w-row at [12..15] = (0,0,-1,0); camera translation read from index 3,7,11 | confirmed | In cbCameraStd the first matrix equals `Proj * RotOnlyView` (3.1 shows element positions); multiply a known point in code and compare to the VS output position. |
| Storage order | **Row-major**: element `[r][c]` at index `r*4 + c` | `FUN_14046baa0` (our name `Math::Matrix44_Mul`, args `(out, A, B)`): `out[0] = A[0]B[0] + A[1]B[4] + A[2]B[8] + A[3]B[12]` | confirmed (static) | n/a |
| 4x4 inverse | `FUN_14046b6c0` (`Math::Matrix44_Inverse`): cofactor inverse, optional determinant output | wf4_projection_depth | confirmed | n/a |
| Matrix34 | 12 floats, 3 rows x 4, **row-major**, rotation in the 3x3 part, translation at indices **3, 7, 11**, implicit last row (0,0,0,1). Stride **0x30**. Composition `FUN_14016cad0` (`Math::Matrix34_Mul`): `out = A * B` (affine) | wf4_camera, wf4_conventions_ansel | confirmed (static) | Dump of a camera world matrix (section 1 check). |
| World-matrix pool | `GraphNodeChunkPool` at 0x1413cda80: [+0] local Matrix34 array, [+8] world array = `DAT_1413cda88`, [+0x10] node-data array with stride 0x18 (flags ushort at +4, dirty flag array at `DAT_1413cda90`). Camera world = `DAT_1413cda88 + node[+0x50]*0x30` | wf4_camera, wf4_conventions_ansel | confirmed | Same dump. |
| Camera write point | `GraphNode::SetWorldMatrix` 0x14086ecd0 writes the pool and marks dirty; `ViewContext::InitFromCamera` reads the pool directly | wf4_conventions_ansel | confirmed (static) | n/a |
| Quaternions | Standard Hamilton column-vector convention; `QuaternionRotationMatrix` 0x1408166a0 | wf4_conventions_ansel | likely | Not needed unless we feed Ansel; we write matrices, not quaternions. |

### 5.2 `TransformMatrix` slot layout

A `TransformMatrix` is a block of 4x4 matrices, `Get(i)` at stride **0x40** from the block base.

| Slot | Offset | Content | Written by |
|---|---|---|---|
| 0 | +0x00 | View | `TransformMatrix::RecomputeView` 0x140914ff0 |
| 1 | +0x40 | Camera world (inverse of view) | same |
| 2 | +0x80 | Rotation-only view (translation zeroed) | same |
| 3 | +0xC0 | Projection (including jitter/offset terms) | `TransformMatrix::SetProjection` 0x1409150d0 |
| 4 | +0x100 | ViewProj = Proj * View | same |
| 5 | +0x140 | InvViewProj | same |
| - | +0x180 | InvProj elements [10],[11],[14],[15] (`GetInvProjData`) | same |

`ViewContext` holds **two** `TransformMatrix` blocks: eye0 at `ViewContext+0x18`, eye1 at `+0x1a8` (stride 400 = 0x190), flag `ViewContext+0x466` (stereo). Status: **dead code**. `InitFromCameraStereo` 0x14091b100 and `SetViewProjectionStereo` 0x14091b850 have no direct callers (data references only, at 0x1411a9cf8 and 0x1414c60bc); `ViewContext::SetActive` 0x14091b780 reads only slots 0..3 of block 0; `IsStereoscopic` 0x1404fd5c0 has no callers. They take two Matrix34 eye worlds and **one shared projection**, so native stereo never had per-eye asymmetric projections. Confidence: confirmed (static) for "no direct callers"; a byte scan for disp32 0x466 found no stereo-flag consumer, but indirect calls are not excluded. Source: `docs/re/` stereo notes (`wf4_stereo_native.md`).

Rule: to push a per-eye asymmetric projection, feed a Matrix44 through `ViewContext::SetProjection` 0x14091b7e0 (or `TransformMatrix::SetProjection`). All dependants (ViewProj, InvViewProj, InvProj data, per-camera CB, frustum) are derived from it inside, but **runtime confirmation is needed** that every dependant refreshes consistently (debugger test listed at the end). `RebuildFrustum` uses slot 0 only, so engine culling follows the matrix we set only if it goes through this path (CLAUDE.md: culling may use its own FOV; check).

---

## 6. Constant buffers

### 6.1 Per-camera CB (`cbCameraStd`)

Filled by `ViewContext::UpdatePerCameraCB` 0x14091b910, called from `ViewContext::SetActive` 0x14091b780 into the shared CB (`ViewContext+0x458`, global `DAT_14140bb48`).

| Offset | Content | Confidence |
|---|---|---|
| +0x00 | **Proj * RotOnlyView**: camera-relative ViewProj (translation stripped from the view; verified by `Matrix34::SetTranslation(.., 0)` before inverting) | confirmed (static) for the content; layout of the rest is likely |
| +0x40 | camera world position float3 (+w) | likely |
| +0x50 | inverse projection data vec4 (from `GetInvProjData`) | likely |
| +0x60 | view forward float3 (= minus the camera z axis) | likely |
| +0x70 | `ViewContext+0xc..0x14` (culling position) and `float(ushort ViewContext+0x464)` (camera index mask) | likely |
| +0x80 | yaw rotation matrix (`SetRotationZ(atan2(...))`) | likely |
| +0xC0 | near, far (`ViewContext+0x430/0x434`) | likely |
| +0xD0 | matrix, likely camera-relative inverse ViewProj; meaning unresolved (built from a transposed projection reload) | guess |
| +0x110 | projection offset (`ViewContext+0x448/0x44c`) | likely |

Open: `UpdatePerCameraCB` copies the matrix floats into the mapped CB **without transposing** (row-major array verbatim). Whether HLSL reads it as `mul(M, v)` with `row_major` packing or as `mul(v, M)` is unresolved (the TAA code transposes by hand). Do not assume until the DXBC reflection says so.

RenderDoc check: select the first draw of `ZPrepass`, open VS cbuffer slot for `cbCameraStd`; verify: first 16 floats = Proj * RotOnlyView (with m[14] = -1, m[11] = f*n/(f-n) if read as row-major); +0x40 = camera position; +0x110 = jitter. Read the `row_major`/`column_major` packing in the shader reflection of the VS.

### 6.2 Camera-relative rule

- **Confirmed.** Shader camera matrices exclude the camera translation; the camera world position is supplied separately (cbCameraStd +0x40; in `RenderTAA`: `g_cameraPosition` / `g_cameraPositionLastFrame`, set via `IShaderProgram::GetConstantByName + SetValue`, history at `RenderLoop+0x10a8 + idx*0xc`).
- Consequence for XR: eye translation relative to the camera node cannot be put into the shader ViewProj by editing the translation of a world matrix alone unless `UpdatePerCameraCB` runs on that matrix. The robust way is to change the camera world matrix (pool) per eye **before** `InitFromCamera`/`SetActive`, and let the engine rebuild everything. World-space reconstruction in shaders is `camera_position + camera_relative_position`; both must be per-eye consistent.

### 6.3 Last-frame constant buffers

| Item | Value | Evidence | Confidence | Check |
|---|---|---|---|---|
| lastVP | `RenderLoop+0xed8 + idx*0x40`: **un-jittered** projection (NoOffset) * view with translation zeroed; `idx` = engine camera index (0 or 1) | `RenderLoop::PostUpdateLastFrameCamera` 0x1408dc2f0 | confirmed (static) | Compare `cbVSCameraLastFrameStd` in frame N with the prior frame's cbCameraStd main matrix, un-jittered. |
| lastCamWorldPos | `RenderLoop+0xf58 + idx*0xc` | same | confirmed (static) | |
| lastJitter | `RenderLoop+0xf70 + idx*8` | same | confirmed (static) | |
| Front-pass set | separate lastVP/pos/jitter for `RenderLoop+0x1590`: +0xf80 / +0x1000 / +0x1018 | same | confirmed (static) | |
| Latch time | **After** TAA and motion blur, per view; skipped when `ViewContext::s_bMotionPause` | same | confirmed (static) | |
| Motion-vector CB | `cbVSCameraLastFrameStd` (lastVP + lastCamPos) filled by `UpdateLastFrameCameraCB`, shared CB, then 'Motion Vectors' and 'Motion Vectors front pass' draw | `DeferredRenderer::RenderGBuffer` 0x140842040 | confirmed (static) | Event 'Motion Vectors'. |
| TAA copies | `RenderTAA` 0x1408ddf70 also keeps `RenderLoop+0x1028 (+idx*0x40)`, `+0x10a8`, `+0x10c0`, `+0x10d0` (previous ViewProj, camera pos, jitter, resolution scale), used only during motion pause | wf4_projection_depth, wf4_temporal | likely | n/a |
| StringIds of matrix parameters | `g_viewProjectionLastFrame` 0x141408018, `g_viewProjectionInv` 0x141408178 (both used in `RenderMotionBlur` 0x1408dd5e0); `g_viewProjLastFrame` 0x141408288, `g_invViewProj` 0x141408278, `g_jitter` 0x141408268, `g_cameraPosition` 0x141408280 (set in `RenderTAA`) | wf4_projection_depth | confirmed | |
| Order of operations | In `RenderTAA` the matrices are explicitly transposed by hand before `SetValue` (local copy `local_6f8`, translation zeroed twice) | `RenderTAA` decompile | confirmed (static) | Shader reflection for `g_viewProjLastFrame`. |

Rule: last-frame matrices must be the **same eye's** previous matrices, un-jittered and camera-relative with the previous camera position supplied separately. The engine already keys them by camera index (2 slots), see `docs/re/temporal.md`.

---

## 7. Back buffer and color

| Item | Value | Evidence | Confidence | RenderDoc / runtime check |
|---|---|---|---|---|
| Swapchain creation | `D3D11CreateDeviceAndSwapChain` in `VideoDeviceDX11Windows::EnableVideoAdapter` 0x140958eb0 (calls at 0x140959180 and 0x1409591e9). DriverType 0 (UNKNOWN), Flags 0x80 (`PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY`), feature level request {11_1} then retry {11_0} on `E_INVALIDARG` / `DXGI_ERROR_UNSUPPORTED`; SDKVersion 7 | wf1/inline findings | confirmed | Frame trace of device creation. |
| Back buffer format | `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB` (0x1d), set via `MOV qword [RBP-0x29], 0x1d` at 0x1409590db | same | confirmed | `GetDesc` on the swapchain in our hook. |
| Buffer count / effect | `BufferCount = 1`, `SwapEffect = DXGI_SWAP_EFFECT_DISCARD` (0), `BufferUsage = 0x20` (render target output), `Flags = 0`, `Scaling = 0`, Windowed = (`screenMode != 1`) | same | confirmed | same |
| Sample count | 1 (R14); MSAA not used on the swapchain | same | likely | same |
| Size | Width `this+0x418`, Height `this+0x41c`; refresh `this+0x424/0x428`. "Resizing SwapChain" is logged on resize | game log | confirmed | n/a |
| Back buffer contents | Holds **sRGB-encoded** values (per CLAUDE.md). Whether the engine's final RTV is the `_SRGB` view (hardware encodes on write) or a UNORM view with manual encode in the final shader is **not determined** | CLAUDE.md gotcha; no RTV-format evidence in the notes | likely for "gamma-encoded in the buffer"; guess for who encodes | `CreateRenderTargetView` desc for the back buffer; the final composite pass's output format. Decides whether our copy must be a raw bit copy (it must be either way; CLAUDE.md) or a shader blit. |
| Extra windows | `VideoDeviceDX11Windows::AddWindow` 0x140945fbe uses `CreateDXGIFactory`; adapter enumeration via `CreateDXGIFactory1` in `EnumerateVideoAdapters` 0x140959692 | inline findings | confirmed | Only the main window's swapchain is relevant for the mod. |
| Device threading | Created without `D3D11_CREATE_DEVICE_SINGLETHREADED`; resources are created on the loader thread (`indexbuffer.cpp` log lines) | inline findings | confirmed | n/a |

### 7.1 Order of the post chain (per view)

Source: `DeferredRenderLoop::Render` 0x1408469c0 (confirmed static for the order; formats likely).

1. G-buffer, lighting, froxel, SSAO/SSR, sky, fog, deferred transparency, 3D HUD "before post" (flag 8; HDR scene RT).
2. `CopyBuffersForNextFrame` 0x1408d9ee0 (conditional): scene depth -> 'Last Frame Depth' (`'FL32'`, R32F); HDR scene colour -> 'Last Frame Buffer' (`0x464c4834`, likely RGBA16F, 8 mips). Taken **before** tonemap. Consumer: SSR and water-drop refraction.
3. **ToneMapper** (`FUN_140914680`, called at 0x140848768): 'Downscale', then `HDR_Luminance*`, `HDR_FinalCompose` (0x141009818), CPU luminance readback; ACES LUT `DATA:/Engine/Textures/ACES/Lut_ACES_sRGB_100nits.PTX` (0x141009530).
4. `RenderFXAA` (modes 1, 2).
5. `PreUpdateLastFrameCamera` then **`RenderTAA`** (`NoMotionVectorFixTAA` right after). History RTs `'TAA render target 0/1'`, format code `'RGBA'` (likely R8G8B8A8, i.e. TAA runs on tonemapped LDR: likely, needs RenderDoc).
6. `RenderMotionBlur` (if enabled for that view), then `PostUpdateLastFrameCamera`.
7. Lens flare, other post, 3D HUD "after post" (flag 4, stencil cleared first), `UpscaleDynamicResolution` 0x1408df750, 2D HUD (flags 0x22), `RenderDebug`.

Color facts:

- **SDR only.** The tonemapper uses an ACES LUT for 100-nit sRGB; no HDR10/scRGB output strings exist (confirmed static, string search). Exposure is auto-adapted from a 1x1 luminance RT (per camera index history, plus one shared CPU readback at `tonemap+0x148`).
- Linear HDR exists only up to the tonemapper. After step 3 everything (TAA, motion blur, HUD) is LDR display-referred.
- A copy of the final image to an `_SRGB` OpenXR swapchain image must keep the bits (CLAUDE.md): same typeless family (R8G8B8A8), raw copy.

RenderDoc checks: resource formats of the HDR scene RT, 'Last Frame Buffer', `TAA render target 0/1`, and the final RT; confirm `'RGBA'` = R8G8B8A8_UNORM and `0x464c4834` = R16G16B16A16_FLOAT in the resource inspector; confirm event order against the list above (the notes also say it is unknown whether `ID3DUserDefinedAnnotation` markers are present in this release build).

---

## 8. OpenXR to engine conversion

Everything here is **unverified** (derived by us from the conventions above; no capture or runtime test). Implement in `stereo/xr_to_engine.*` only, with unit tests (`tests/`), and validate against the runtime checks below before trusting it.

### 8.1 Basis change (rule)

Engine world axes: X right, Y forward, Z up. XR (stage/local) axes: X right, Y up, Z back (-Z forward). Define the point/vector mapping

| Engine component | = | XR components |
|---|---|---|
| x_e | = | x_xr |
| y_e | = | -z_xr |
| z_e | = | y_xr |

As a matrix (column vectors, rows = engine x, y, z; columns = xr x, y, z):

```
B = [ 1  0  0 ]
    [ 0  0 -1 ]
    [ 0  1  0 ]
```

Check: `B * (0,0,-1) = (0,1,0)` (XR forward -> engine forward); `B * (0,1,0) = (0,0,1)` (XR up -> engine up); `det(B) = +1`, a proper rotation, so no mirroring and no winding flip.

Rules:

1. **Positions** (metres): `p_e = anchor_e + s * B * (p_xr - p_xr_origin)`. `s` = world scale, 1.0 for 1 unit = 1 m (likely; IPD/world-scale change resets per-eye history, CLAUDE.md).
2. **Orientations**: because the engine camera-local frame and the XR view-local frame are the same basis (section 2), the basis change is applied on the **world side only**: `R_e = B * R_xr`, where `R_xr` is the XR eye-to-world rotation and the result's columns are the eye's (right, up, back) expressed in engine world axes. Do not also conjugate with `B^-1` and do not add a local-side adapter, since the local frames already agree. Relative head motion (head pose in the XR reference space) must go through `B` the same way before it is composed with the game's camera orientation. Do **not** apply the Ansel quaternion adapter q11: that exists only because Ansel's camera frame is Z-up/Y-forward, ours is not.
3. **Matrix34 write**: store 12 floats row-major, rotation 3x3 in columns 0..2, translation at indices 3, 7, 11 (section 5).
4. **Anchor**: `anchor_e` is the engine camera node's world position and heading taken from the game (helmet camera, `KTUCamera::Update` 0x1409d9060 result), re-read every frame after any World Shift (section 1). The head offset must not leak into the game camera's own state (`GraphNode` world matrix is the single write point; do not change gameplay camera logic, CLAUDE.md rule 5).
5. **Per-eye**: eye pose = `xrLocateViews` view pose (already includes IPD half-offset in the head/reference space) converted with the same `B`. Use one function for both eyes.

### 8.2 Projection from `XrFovf` (rule)

Let `tL, tR, tU, tD` = `tan(angleLeft)`, `tan(angleRight)`, `tan(angleUp)`, `tan(angleDown)` (angleLeft and angleDown are negative). Build the engine matrix directly, in the engine's layout:

| Index | Value |
|---|---|
| m[0] | `2 / (tR - tL)` |
| m[5] | `2 / (tU - tD)` |
| m[2] | `(tR + tL) / (tR - tL)` plus any x jitter |
| m[6] | `(tU + tD) / (tU - tD)` plus any y jitter |
| m[10] | `n / (f - n)` |
| m[11] | `f*n / (f - n)` |
| m[14] | `-1` |
| m[15] | `0` |
| everything else | 0 |

Derivation basis: the engine's NDC x = `m[0]*(x/-z) - m[2]` (from `w = -z` and the offset term in column 2, section 3.5), so `m[2]` must be `+(tR+tL)/(tR-tL)` to land the left/right tangents on -1/+1. Same for y. This keeps the engine's reversed-Z and finite-far rows unchanged and the jitter sign convention unchanged. Status: derived, **unverified**; sign errors here show up as a horizontal or vertical eye-image shift.

Further rules:

- Both eyes use the **same** n and f as the engine camera (`Camera+0x7c/+0x80`), unless deliberately changed; the front pass uses its own n/f with the same asymmetry (3.4).
- `m[0]` and `m[5]` are written independently, so the `Camera+0x84` aspect multiplier and Hor+ logic are bypassed. This only works if our matrix survives after `InitFromCamera`: the matrix must be installed through `ViewContext::SetProjection` 0x14091b7e0 (section 5) at the point after the engine builds its own, every frame.
- Keep the matrices submitted to OpenXR (`XrCompositionLayerProjectionView.fov` and pose) identical to the ones rendered (CLAUDE.md).

### 8.3 Unverified items

- That the game camera world matrix really has columns (right, up, back) with Z-up world (section 1 check).
- Sign of the m[2]/m[6] shift.
- That an installed projection reaches culling (`RebuildFrustum`, slot 0 only) and the front pass.
- Whether the OpenXR runtime accepts reversed-Z depth submission (`XR_KHR_composition_layer_depth`).

---

## Contradictions found between source notes

| Topic | Note A | Note B | Reading used here |
|---|---|---|---|
| FOV type of `Camera+0x78` | `wf4_camera.md`: "vertical FOV in radians" | `wf4_projection_depth.md` / `wf4_conventions_ansel.md`: `MatrixPerspectiveFovRH` makes `m[5] = m[0]*aspect` (x scale primary); `KTUCamera::Update` Hor+ logic; Ansel `fovType = 0` | **Horizontal**. Decompile outranks the early guess. Likely, with runtime check pending. |
| View matrix form | `wf4_conventions_ansel.md`: view is the inverse of the world matrix "transposed into row-vector form" | `wf4_projection_depth.md`: `ViewProj = P * V` via row-major `A*B`; column-vector everywhere | Column-vector. The "transposed" wording is treated as unverified. |
| Offset sign | `wf4_projection_depth.md`: sign "to be confirmed" | `wf4_conventions_ansel.md`: "shifts NDC by minus the offset" | Minus (algebra in 3.5 agrees); likely. |
| `Camera+0x78` default vs CVar | struct default 0.7854 rad | CVar `HorizontalFov` default 1.25 rad | Not a conflict: the struct default is the constructor value; CVars have no other native readers, so neither is the runtime value. Runtime value unknown. |
| Where Ansel's `fovType` is | Config offset `+0x48` dword 0 (Ansel note, "candidate") | inline note lists `fovType=0` | Same field. Unverified field order. |

---

## Runtime checks still needed

Ordered by what unblocks the most code.

1. **Camera world matrix dump** (debugger, break at `GraphNode::SetWorldMatrix` 0x14086ecd0 for the helmet camera node, or after `KTUCamera::Update` 0x1409d9060): confirm Matrix34 row-major, translation at 3/7/11, columns (right, up, back) with Z-up world, det +1, and 1 unit = 1 m (measure bike/helmet offset). Unblocks section 1, 2, 8.1.
2. **cbCameraStd in RenderDoc** (VS of first `ZPrepass` draw): first matrix = Proj * RotOnlyView, m[10]/m[11]/m[14] values, camera position at +0x40, offset at +0x110; read the HLSL packing (`row_major`/`column_major`, `mul` argument order) in the shader reflection. Unblocks 3.1, 5.1, 6.1.
3. **Horizontal vs vertical fov**: compute `2*atan(1/m[0])` vs `2*atan(1/m[5])` against the CVar/`Camera+0x78` at 0x14091aeb0; confirm Hor+ behaviour at 16:9 and a wider window.
4. **Jitter**: two or more consecutive TAA frames: [0][2] = 1/W, then 0; [1][2] = 0, then 1/H; sign of the shift on a known point; `g_jitter` values in the TAA resolve.
5. **Depth state**: `ClearDepthStencilView` value 0.0; `OMSetDepthStencilState` func GREATER_EQUAL on opaque draws; main depth target format; near geometry depth ~1.0.
6. **Front pass**: viewport MinDepth/MaxDepth on `Opaque Front`; whether depth is cleared between main and front pass; projection differences (fov, n, f) vs the main pass; value of `RenderLoop[0x1118]`.
7. **Back buffer / final RTV**: RTV format of the back buffer (`_SRGB` view vs UNORM with manual encode); formats of HDR scene RT, 'Last Frame Buffer' (0x464c4834), `TAA render target 0/1` (`'RGBA'`); order of passes vs section 7.1.
8. **Installing a custom projection**: debugger test calling `ViewContext::SetProjection` 0x14091b7e0 / `TransformMatrix::SetProjection` 0x1409150d0 with a test matrix; verify ViewProj, InvViewProj, InvProj data, per-camera CB, frustum culling and the front pass all refresh consistently.
9. **World Shift**: on a shift frame, check camera translation jump (2048), motion-vector streaks (stale `Mesh+0xf4`), and that lastVP needs no change.
10. **Asymmetric fov test** (after our matrix exists): a deliberate known `XrFovf` shows correct left/right and up/down, confirming the m[2]/m[6] signs in 8.2.
11. **Culling frustum**: does `RebuildFrustum` follow the installed projection or the camera's own FOV (CLAUDE.md gotcha)?
12. **Camera constants of the actual race camera**: dump `Camera+0x78..0x94` (hfov, near, far, aspect multiplier, front values) during a race, since the struct defaults are only constructor defaults.
