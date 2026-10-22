# tt2vr: VR for TT Isle of Man: Ride on the Edge 2

A PC VR mod for *TT Isle of Man: Ride on the Edge 2* (Steam, Direct3D 11). The game's own renderer draws each eye separately: real stereo, not a stretched flat image. You get head tracking in the helmet and chase cameras, the HUD on a floating panel, menus on a virtual screen, comfort options, and AMD FSR1 upscaling. It works through OpenXR (tested with SteamVR).

The mod changes only what you see. Physics, input, saves and the game's own settings stay as they are.

## Requirements

- The Steam version of the game, build 4989319 (its last update). Other builds are not supported: the mod refuses to hook an unknown game exe, and the game then runs flat.
- Windows 10 or 11, a PC VR headset and an OpenXR runtime. Tested: a Meta Quest headset over Link, with SteamVR as the active OpenXR runtime.
- A reasonably strong GPU. Development used an RTX 3060, where render scale 0.65 to 0.85 is the useful range (see Performance).

## Install

1. Close the game.
2. Copy `dinput8.dll` next to `TT2.exe` in the game folder (Steam > the game > Manage > Browse local files).
3. Copy the `tt2vr` folder (`tt2vr.dll`, `tt2vr.toml`) into the same game folder, so you have `<game>\tt2vr\tt2vr.dll`.
4. In SteamVR: Settings > OpenXR > set SteamVR as the OpenXR runtime.
5. Start SteamVR, put the headset on, then start the game from Steam.
6. In the game's options, use Borderless or Windowed display mode.

The mod writes its log to `<game>\tt2vr\logs\tt2vr.log`.

## Uninstall

Delete `dinput8.dll` from the game folder and delete the `tt2vr` folder. The mod never changes game files, `UserSettings.cfg` or your save.

## Using it

- **Riding:** the helmet camera and the chase cameras are head-tracked. The game's own view switch (camera button) still works.
- **Menus, pause and loading screens:** shown on a flat screen in front of you that stays fixed in the room.
- **HUD:** speed, map, times and messages are on their own panel in front of you while riding.
- **Recenter:** F7. Seat position: Num8 / Num2 forward / back, Num9 / Num3 up / down, Num5 reset (NumLock on).
- **Desktop overlay** (mod status, comfort sliders, performance graph): F10; F8 shows the mouse cursor for it.

### Settings

Most settings can be changed in the game: **Options > Display > VR**. They take effect at once and are saved to `<game>\tt2vr\user.toml`.

| Setting | What it does |
|---|---|
| Horizon lock | How much of the bike's lean is removed from your view (100% = the horizon stays level). |
| Camera shake | How much of the game's camera shake and speed noise you keep. |
| World scale, IPD scale | Size of the world, and eye separation. |
| Seat forward / height | Moves your eye point relative to the rider. |
| HUD distance / height | Where the HUD panel floats. |
| Render scale (FSR) | The game renders at this fraction of the headset's resolution, and FSR1 upscales it. Lower is faster. |
| FSR sharpening | Sharpening strength of the FSR upscale. |
| Occlusion culling | The game's occlusion culling; leave it on. |

Everything else is in `<game>\tt2vr\tt2vr.toml`, with a comment on every setting. Delete `user.toml` to go back to the values in `tt2vr.toml`.

## Performance

- Hold the headset's refresh rate first. If SteamVR keeps dropping to half rate, lower **Render scale (FSR)** in the VR tab.
- Keep SteamVR's own render resolution near 100% (Settings > Video). FSR does the upscaling, and a higher SteamVR resolution costs time in both compositors and in FSR.
- The game loads the track in sections. A short hitch when a new section loads is the game's own streaming, and also happens flat.

## Compatibility

- **Steam overlay:** works.
- **RenderDoc:** works (the mod clears the game's anti-capture flag). F9 triggers a capture when RenderDoc is attached.
- **ReShade:** not tested. The mod uses `dinput8.dll`, so `dxgi.dll` / `d3d11.dll` stay free for ReShade. But ReShade's screen-space effects would run on the game's side-by-side two-eye image, across the seam between the eyes. Expect artefacts with depth- or blur-based effects.
- **RTSS / MSI Afterburner overlay:** not tested. It draws into the game's frame before the mod copies the eyes, so it would show up in the headset (left eye on the left half, right eye on the right).
- **NVIDIA Ansel** (bundled with the game): don't use it in VR.

## Known issues

- Mid-range GPUs struggle to hold 72 Hz at full resolution; use the render scale.
- The game's streaming hitches (see Performance).

## Building from source

Requirements: Windows, Visual Studio 2022 (MSVC), CMake 3.28 or newer, vcpkg with `VCPKG_ROOT` set.

```
cmake --preset x64-release
cmake --build --preset x64-release
ctest --preset x64-release
```

`cmake --build --preset x64-release --target deploy` copies the build into the game folder named by the `TT2_GAME_DIR` environment variable (close the game first).

## License and credits

The mod's license is not decided yet. Third-party code:

| Component | License |
|---|---|
| [MinHook](https://github.com/TsudaKageyu/minhook) | BSD 2-Clause |
| [spdlog](https://github.com/gabime/spdlog) | MIT |
| [toml++](https://github.com/marzer/tomlplusplus) | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT |
| [DirectXTK](https://github.com/microsoft/DirectXTK) | MIT |
| [xxHash](https://github.com/Cyan4973/xxHash) | BSD 2-Clause |
| [OpenXR SDK loader](https://github.com/KhronosGroup/OpenXR-SDK) | Apache 2.0 |
| [AMD FidelityFX FSR 1.0](https://github.com/GPUOpen-Effects/FidelityFX-FSR) (`third_party/fidelityfx-fsr1`) | MIT |
| [doctest](https://github.com/doctest/doctest) (unit tests only) | MIT |

*TT Isle of Man: Ride on the Edge 2* is a game by Kylotonn / Nacon. This project is not affiliated with them and contains none of the game's code or data.
