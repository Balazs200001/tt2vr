# RE baseline — TT Isle of Man: Ride on the Edge 2

Every address, offset and signature in `docs/` refers to the build below unless a note says otherwise.

## Build identity

| Item | Value | Evidence |
|---|---|---|
| Steam app / depot | 1082180 / 1082181 | `steamapps/appmanifest_1082180.acf` |
| Steam build ID | **4989319** | `appmanifest_1082180.acf` → `"buildid" "4989319"` |
| Depot manifest | 5123329659964927792 | same file, `InstalledDepots` |
| Exe | `TT2.exe`, 21,771,984 bytes | game root |
| SHA-256 | `103d4b2e4d86786a7eb1810faa280a41cec6cadce1e8865165c3de8ba87a601a` | `sha256sum` on the live file and on the snapshot (identical) |
| Snapshot | `C:\dev\tt2vr-re\bin\4989319\TT2.exe` | copied 2026-10-15; REA/Ghidra analyse this copy only |
| PE timestamp | 1588758476 (2020-05-06 09:47:56 UTC) | COFF header |
| PDB (not shipped) | `F:\Clean\TouristTrophy2\Bin\x64\TouristTrophy264_s.pdb`, GUID {80E09F16-7306-4E59-BC79-382E9BC5BD5D}, age 93 | CodeView debug entry |
| Engine | Kylotonn KT Engine, internal name **PlayAll**; game code `touristtrophy2` | source paths in the game log, e.g. `f:\clean\playall\code\runtime\dx11\playall\drivers\graphics\videodevicedx11init.cpp` |

## Binary facts

- **x64** PE32+ (machine 0x8664), image base 0x140000000, `DYNAMIC_BASE` + `HIGH_ENTROPY_VA` + `NX_COMPAT`, CFG off, GUI subsystem.
- Sections: `.text` 0x1000 (14.8 MB), `.rdata`, `.data`, `.pdata`, `.rodata`, `_RDATA`, `.rsrc`, `.reloc`, `.bind` (SteamStub).
- **22,730 named exports** (export DLL name `TouristTrophy264_s.exe`), 20,252 of them in `.text`. Ghidra applies and demangles them, so most engine classes have real names (`PlayAll::Graphics::Camera`, `DeferredRenderLoop`, `CascadedShadowMapRenderer`, …).
  - Caveat: the linker folded identical functions (`/OPT:ICF`). One address can carry several unrelated export names, e.g. 0x1403ff1d0 is both `VideoDevice::SetRenderThreadID` and `SystemInfo::SetVRamMo`. Treat a name at an address as "one of the folded bodies", never as proof of the caller's intent.
- Dear ImGui is compiled into the exe (≈600 `imgui` strings; `imgui.ini` with a default `[Debug]` window in the game root).
- Ghidra: 41,444 functions after auto-analysis (headless, 442 s).

### Imports that matter to the mod

| DLL | Functions | Why it matters |
|---|---|---|
| `d3d11.dll` | `D3D11CreateDeviceAndSwapChain` (only) | device + swapchain in one call; static import |
| `dxgi.dll` | `CreateDXGIFactory`, `CreateDXGIFactory1` | adapter enumeration; extra windows (`VideoDeviceDX11Windows::AddWindow`) |
| `AnselSDK64.dll` | `ansel::setConfiguration`, `updateCamera`, `isAnselAvailable`, `markBufferBind` | engine-side camera override incl. off-centre projection; buffer-bind hints |
| `DINPUT8.dll` | `DirectInput8Create` | wheels/controllers |
| `VERSION.dll`, `WINMM.dll`, `HID.DLL` | 3 / 3 / 1 functions | neutral proxy candidates (see Decisions log) |
| `WINTRUST.dll`, `CRYPT32.dll`, `imagehlp.dll` | `WinVerifyTrust`, … | signature checks; must not reject our proxy (to verify) |
| `VCOMP140.DLL` | OpenMP runtime | parallel CPU work |
| `CrashRpt1403_x64.dll` | 6 ordinals | crash reporter; coexists with our crash handler (Phase 10) |

Game-shipped DLLs in the root (never overwrite): `AnselSDK64.dll`, `CrashRpt1403_x64.dll`, `dbghelp.dll`, `dbxLive64.dll`, `fmod64.dll`, `fmodstudio64.dll`, PhysX (`PhysX64_s.dll`, `PhysXCommon64_s.dll`, `PhysXCooking64_s.dll`, `PxFoundation64_s.dll`), `cudart32_30_9.dll`, `steam_api64.dll`; `UWP\xinput1_3.dll`.

## Protection status

Verdict: **SteamStub DRM wrapper only; the game code is not packed or encrypted on disk. No Denuvo, no third-party anti-cheat; the engine's own AntiCheatManager appears inert (see table).**

| Check | Result | Evidence |
|---|---|---|
| Steam DRM wrapper | present | `.bind` section (0x322d0 bytes, entropy 7.96) holds the entry point 0x141577310 |
| `.text` encrypted? | no | entropy 6.51 (normal x64 code; encrypted ≈ 8.0); 3 TLS callbacks point into `.text` (0x140e51c7c, 0x140e51b40, 0x1407fcb60) and run before the stub, which would crash if `.text` were encrypted; Ghidra disassembles and decompiles it cleanly |
| Import / export tables | intact | 44 DLLs / 749 imported functions; 22,730 readable exports |
| Denuvo / VMProtect / Themida | no indicators | no extra high-entropy sections besides `.bind`; exe size 21.7 MB |
| Third-party anti-cheat | none | no EasyAntiCheat/BattlEye folders or imports |
| Capture-tool opt-out | present | RenderDoc cannot capture the unmodified game. `EnableVideoAdapter` passes `Flags = 0x80` (`D3D11_CREATE_DEVICE_PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY`) to `D3D11CreateDeviceAndSwapChain`; RenderDoc treats that flag as "application requested not to be hooked" (its log, `d3d11_hooks.cpp(163)`), skips wrapping the device while DXGI is already wrapped, and the call returns S_OK with a null immediate context. The game then crashes at `TT2.exe+0x9592dd` (`mov r9,[rcx]`, QueryInterface on context slot 0). Confirmed 2026-10-16 from 3 CrashRpt minidumps (access violation reading 0, rcx = 0, loop index 0, returned feature level 11_1) and the RenderDoc log; NVIDIA NvCamera and the Steam overlay were ruled out. Fix: our proxy hooks `D3D11CreateDeviceAndSwapChain` and clears bit 0x80 before the call goes on (debug-layer registry behaviour only; no gameplay effect). |
| Engine anti-tamper | present but inert offline (likely) | `PlayAll::Core::AntiCheatManager`: debugger checks, CRC32 of loaded modules' `.text` (d3d11/dxgi/d3d9/d3d10/d3d12 excluded), process-name scan, thread-start-address check. Created only if `CoreModule::Desc+0x8e == 1` (runtime check); its worker threads start only from `SearchThread` 0x1404d33e0, which has no code callers; results are never read. Do not hook `kernel32!QueryPerformanceCounter` or ntdll debug APIs anyway. |
| Authenticode | not signed | empty security directory |

Consequences: static analysis of the snapshot is valid for runtime; signature scans match the in-memory image. Run engine scans after the stub has executed (e.g. lazily from the device-creation hook), even though `.text` is plaintext. Never modify, strip or bypass the stub.

## Runtime facts from the game log

Snapshot: `C:\dev\tt2vr-re\logs\TT Isle of Man 2.build4989319.2026-10-15T1304.log` (the game overwrites its log every launch).

- Main thread (0x3364 in that session) runs engine init, creates the render window and the D3D11 device (`videodevicedx11init.cpp`), loads settings, and logs ~90% of all lines.
- Other logging threads: level loader (`levelserializer.cpp`, `indexbuffer.cpp` — resources are created off the main thread), world streamer (`worldstreamer.cpp`), online services (`leaderboardsteam.cpp`), async loader.
- Adapter: NVIDIA GeForce RTX 3060, windowed (`Fullscreen: false`).

## Settings and graphics options

- Engine defaults: `COMMON:/SETTINGS/DUMMYS.DAT` (`<game>\Common\Settings\DUMMYS.DAT`, XOR-obfuscated Lua, decrypted by `VirtualFile::EncryptDecrypt` 0x140786f20). Never modify it.
- User settings: Options menu → `SettingsManager::Apply` → `GameEngine::SaveUserSettings` 0x1404d2a20 writes `<Documents>\My Games\TT Isle of Man 2\UserSettings.cfg` (Lua, serialisable CVars only). On this machine: `C:\Users\Balazs\OneDrive\Documents\My Games\TT Isle of Man 2\` (empty until an option is changed). Read at startup by `GameEngine::LoadSettings` 0x1404bbeb0 unless a budget preset is active. Camera/HUD/gameplay options go to the profile (Steam Cloud `Save.dat`), not to CVars.
- CVars of interest: `Engine.Renderer.AntiAliasing` (0 off, 1 FXAA, 2 FXAA_TAA, 3 TAA), `Engine.Renderer.MotionBlur.*`, `Engine.Device.{VSync,ScreenMode,ResolutionWidth,ResolutionHeight,RefreshRate}`, `Engine.Renderer.DynamicResolution.*`, `Engine.Renderer.RenderJobCount`, `Engine.Renderer.UseAnsel`.
- In-game menu (confirmed by Balazs, 2026-10-16): AA Off / FXAA / TAA, motion blur Off / Low / Normal, dynamic resolution On / Off, no FOV option. Menu "TAA" is written as `Engine.Renderer.AntiAliasing = "FXAA_TAA";` (mode 2).
- `UserSettings.cfg` syntax (confirmed, written by the game): one `Engine.Group.Name = value;` line per CVar, strings quoted (`Engine.Device.ScreenMode = "Borderless";`, `Engine.Renderer.DynamicResolution.DynamicResolutionMode = "Off";`). Resolution: `Engine.Device.ResolutionWidth/ResolutionHeight`. This is the route for forcing per-eye render size in Phase 3/4 without touching game files.
- Captures use AA = TAA (`FXAA_TAA`), dynamic resolution Off.

## Tooling for this build

- Ghidra headless project (analysed): `C:\dev\tt2vr-re\ghidra-headless\TT2_4989319.gpr`.
- Query server: `tools/ghidra/start-query-server.ps1` + `node tools/ghidra/gq.js "<command>"`; bulk exports (functions, string xrefs, import xrefs, vftables, call graph) in `C:\dev\tt2vr-re\ghidra-query\exports\`.
- Export/string dumps: `C:\dev\tt2vr-re\dumps\` (`exports_qualified_4989319.tsv`, `strings_4989319.tsv`, `pe_report_4989319.*`).
