# tt2vr: VR for TT Isle of Man: Ride on the Edge 2

A PC VR mod for *TT Isle of Man: Ride on the Edge 2* (Steam, Direct3D 11). The game's own renderer draws each eye separately: real stereo, not a stretched flat image. You get head tracking in the helmet and chase cameras, the HUD on a floating panel, menus on a virtual screen, comfort options, and AMD FSR1 upscaling. It works through OpenXR (tested with SteamVR).

The mod changes only what you see. Physics, input, saves and the game's own settings stay as they are.

## Install

1. Download `tt2vr-<version>.zip` from the [Releases](https://github.com/Balazs200001/tt2vr/releases) page.
2. Extract it into the game folder (Steam > the game > Manage > Browse local files): `dinput8.dll` lands next to `TT2.exe`, and the rest in `<game>\tt2vr\`.

To update, extract the new zip over the old files. Your in-game settings (`user.toml`) are kept.

The mod writes its log to `<game>\tt2vr\logs\tt2vr.log`.

## Uninstall

Delete `dinput8.dll` from the game folder and delete the `tt2vr` folder. The mod never changes game files, `UserSettings.cfg` or your save.

## Using it

- **Menus, pause and loading screens:** shown on a flat screen in front of you that stays fixed in the room.
- **HUD:** speed, map, times and messages are on their own panel in front of you while riding.
- **Recenter:** F7. Seat position: Num8 / Num2 forward / back, Num9 / Num3 up / down, Num5 reset (NumLock on).
- **Desktop overlay** (mod status, comfort sliders, performance graph): F10; F8 shows the mouse cursor for it.

### Settings

Most settings can be changed in the game: **Options > Display > VR**. They take effect at once and are saved to `<game>\tt2vr\user.toml`.

| Setting | What it does |
|---|---|
| Seat forward / height | Moves your eye point relative to the rider. |
| HUD distance / height | Where the HUD panel floats. |
| Render scale (FSR) | The game renders at this fraction of the headset's resolution, and FSR1 upscales it. Lower is faster. |
| FSR sharpening | Sharpening strength of the FSR upscale. |

Everything else is in `<game>\tt2vr\tt2vr.toml`, with a comment on every setting. Delete `user.toml` to go back to the values in `tt2vr.toml`.

## Performance

- Hold the headset's refresh rate first. If SteamVR keeps dropping to half rate, lower **Render scale (FSR)** in the VR tab.
- Keep SteamVR's own render resolution near 100% (Settings > Video). FSR does the upscaling, and a higher SteamVR resolution costs time in both compositors and in FSR

## Building from source

Requirements: Windows, Visual Studio 2022 (MSVC), CMake 3.28 or newer, vcpkg with `VCPKG_ROOT` set.

```
cmake --preset x64-release
cmake --build --preset x64-release
ctest --preset x64-release
```

`cmake --build --preset x64-release --target deploy` copies the build into the game folder named by the `TT2_GAME_DIR` environment variable (close the game first). `tools/make-release.ps1` builds, tests and packs the release zip (`build\release\tt2vr-<version>.zip`, version from `src/core/version.hpp`).

## License and credits

tt2vr is released under the [MIT License](LICENSE). Third-party code:

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
| [RenderDoc in-application API header](https://github.com/baldurk/renderdoc) (`third_party/renderdoc`) | MIT |
| [doctest](https://github.com/doctest/doctest) (unit tests only) | MIT |

*TT Isle of Man: Ride on the Edge 2* is a game by Kylotonn / Nacon. This project is not affiliated with them and contains none of the game's code or data.
