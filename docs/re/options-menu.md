# Options menu: how it is built, and how a VR tab could be added at runtime (Phase 8i proposal)

All addresses refer to build 4989319 (`docs/re/README.md`). Static analysis only (2026-10-21); names are ours (TT2 classes are not exported). Confidence: `confirmed` / `likely` / `guess`; all `(static)`.

## 1. Screen structure

The Options screen is a native Ux widget screen (`UxManager`, `UxWidget`, `BaseScreen`) built from `COMMON:/UI/UIML/OPTIONS.UIML` (packed). Flash only skins single widgets; C++ never calls Flash functions by name here. `confirmed`

| Our name | Address | Behaviour | Confidence |
|---|---|---|---|
| `TTOptionsState::Create` | 0x140300e20 | loads the UIML, finds widgets by id, makes a pool of **20** row lines (struct 0x30: list +0x8, slider +0x10, edit +0x18, row type +0x28), parses the settings groups "Display" / "Graphics" / "PostProcess" into `+0x654/+0x658/+0x65c`, fills the tab-label table | confirmed |
| `TTOptionsState::SelectPage` | 0x140307630 | sets the page, shows the tab buttons, opens tab 0 | confirmed |
| tab buttons | 0x140308040 | for i in 0..8: loc id `state+0x4c0 + (page*9 + i)*4`; empty (`s_LocaIdEmpty`) = hidden, else shown with that int loc id. Tab count (`FUN_140304070`) = non-empty slots, which bounds LB / RB | confirmed |
| `TTOptionsState::SwitchTab` | 0x140307730 | rows from `FUN_140303dc0`; result 0x4b = "settings-driven": row types by `FUN_140300590` (only for tabs 1..3), rows filled by `FUN_1403085e0` | confirmed |
| `TTOptionsState::PageTabRange` | 0x140303dc0 | page 0: option ids 0..8; page 3 tab 0: 0x1e..0x27, tabs 1..: settings groups; page 4: audio | confirmed |
| `TTOptionsState::Validate` | 0x140308dd0 | 'ValidationInformation' / 'RestartInformation' boxes; on OK `SettingsManager::Apply(group, true)`, SaveUserSettings, `ProfileBase::AutoSave` | confirmed |
| state fields | | +0x450 page widgets (Gameplay, Controls, Cameras, Display, System), +0x478 tab buttons [9], +0x4c0 `locId[page][9]`, +0x590 page, +0x598 tab, +0x5a8 rows [20], +0x650 `int settingsGroup[9]` per tab | confirmed (offsets), likely (+0x650 is int[9], -1 when unused) |

The Display page (3) uses tab slots 0..3; slots 4..8 exist in the UIML and are hidden.

## 2. Two row sources

- **Profile options** (Gameplay, Cameras, HUD, Audio): a fixed table of 75 entries (singleton `DAT_141389a68`, 0xe8 bytes each, defaults in `FUN_140311f00`), stored in the profile. Not extendable. `confirmed`
- **SettingsManager groups** (Display tabs 1..3): `SettingsManager::s_pSingletonInstance` 0x1413ad228 holds `+4 count`, `+8 Settings*[]`; a `Settings` group holds `+4 count`, `+8 ISetting*[]`, parsed from `TUNING:/OPTIONS/<name>.cfg` by `ParseSettings` 0x14077b540. `confirmed`
- `ISetting`: +0x28 type (2 bool, 4 slider shown as `value*100 %`, 0/1/5..7 lists), +0x2c title loc id, +0x30 tip loc id, +0x38 current value, +0x74 / +0x78 list count and value loc ids. Vtable slots used by the manager: +0x08 SetSetting (every UI change), +0x10 Revert, +0x40 Apply. Vtables: SettingBool 0x140f76b18, SettingInt 0x140f76b60, SettingFloat 0x140f76ba8 (base ISetting 0x140f76968 has `_purecall` slots). Stock settings are bound to CVars, so mod rows must be built by hand. `confirmed`
- Change path: slider / list handler -> `FUN_140308ce0` -> `SettingsManager::SetSetting` 0x14077d800 -> ISetting vslot +0x08. `confirmed`
- Save: `WriteSave` / `ReadSave` / `GetSaveSizeByte` (0x14077ed30 / 0x14077c7b0 / 0x14077af60) iterate `SettingsManager+4` groups only; `Apply`, `SetSetting` and `GetSetting` index the array without a count check. `confirmed`

## 3. Localisation

Labels are int loc ids resolved by `LocaleManager::GetString16(int)` 0x1407547b0, which returns `L"NOT FOUND"` for unknown ids (no crash). A detour answering a private id range (e.g. >= 0x7F000000) with our own text is the clean way to name a tab and its rows. That the tab and row text reach this overload: `likely` (log it first).

## 4. Proposed injection (all in memory; nothing on disk)

1. Post-hook `TTOptionsState::Create`: our tab's loc id into `state+0x53c` (page 3, slot 4), our group index into `state+0x660`.
2. Our group: a `Settings` block with hand-built ISettings (SettingFloat-sized for sliders, SettingBool-sized for toggles) whose vtables are copies of the stock ones with +0x08 / +0x10 / +0x40 replaced (live apply, revert, apply). Never the stock destructor.
3. Register it so the save code never sees it: replace `SettingsManager+8` with a copy one slot longer, our group at index `count`, `+4` unchanged; put the original pointer back at shutdown. Do not hook `0x14040d3f0` (folded with Physics `ActorGroup::operator[]`).
4. Post-hook `SwitchTab`: for page 3 tab 4 call `FUN_140300590` then `FUN_1403085e0` (the game does this for tabs 1..3 only).
5. Detour `GetString16(int)` for the private id range.
6. Values are stored by the mod (its own file), never in the game profile.

Risks: the array swap (restore at exit), the unverified `+0x650` layout and slider scale (type 4: 0..1 vs 0..100 vs the CVar range), the 20-row pool, missing vtable slots. Fallback: any mismatch (prologue bytes, null widget, unexpected field) leaves the menu untouched. Size: about 450-550 lines.

Not usable: `MessageBoxManager::PopMessageBox` (0x14054c480 / 0x14054c540 / 0x14054c600) is a text and two-button dialog only.

## 4a. Implementation facts (static, 2026-10-21) and corrections to section 4

- `ParseSettings` runs only in `Create` and appends a new group every time (each Options open adds three), so the hidden group must be put back after every `Create`; the game may also grow the array there. The array is game-owned (`AllocAlignedInfo(n*8, 8, 9, array.inl, 0x76)` / `FreeAlignedInfo(old, false)`, exports); when the mod grows it, it uses the same allocator, so nothing needs restoring at exit. `confirmed (static)`
- The constructor sets all 45 tab labels to `s_LocaIdEmpty` (0x141358528, -1) and `+0x650` int[9] to -1; `+0x650` is indexed by tab only. Page 3 tab 4 already takes the settings branch of `SwitchTab` and reads `+0x660`: with -1 there and a label set, `FillRows` would read `data[-1]`. `confirmed (static)`
- The change handlers check the tab: `OnListChange` 0x140305970 calls `SetRow` for tabs 1..3 only, `OnGaugeValidate` 0x140305c40 for tab 1 only. `confirmed (static)`
- `ISetting` vtables have 9 slots; the screen and the manager call +0x08 SetSetting (stores +0x38, callbacks, then +0x40 if the byte at +0x40 is set), +0x10 Revert, +0x38 HasValueChanged, +0x40 SetValue (Apply calls it always). `GetFlags` = 1 | per setting (changed ? +0x68 : 1): with +0x68 = 1 no popup. `confirmed (static)`
- Slider rows: `OpenRow` 0x140305e10 builds the gauge from +0x38 (value), `GetMin`/`GetMax` (`**(+0x80) -> +0xb0 / +0xb8`, float pointers) and a step of 1; `OnGaugeValidate` passes the gauge value, `SetRow` divides it by 100. `confirmed (runtime, 8i)`: with a 0..1 range the gauge only offered 0 and 1, so the mod's sliders use a percent range and hold value x 100 while `OpenRow` builds the gauge.
- Titles and tab labels go through `UxWidget::SetTextLocaId` -> `HudTextLabel::SetTextLocaId` 0x140893970 -> `GetString16(int)`, which copies the text at once. `confirmed (runtime)`: the tab and row titles show the mod's text.

## 4b. Runtime (test mode, 2026-10-21, `game::OptionsTab`)

- All six TT2 patterns resolved at the static addresses; the tab is added at the first `Create` (the game builds the Options state once at startup) and on every later open.
- Options > Display shows "VR" under Post Processing; LB / RB and up / down reach it like the other tabs; its rows show the mod's values (sliders as "N%", lists with literal labels "+2 cm", "1.0 m"); ten rows since 2026-10-22 (render scale and FSR sharpening lists, PLAN 9e), drawn like the ten-row HUD tab.
- Slider: Camera shake opened at 100 in 0..100, 25 steps left gave 75, the mod received 0.75. List: Seat forward "+2 cm" gave index 12 -> 0.02 m. Both applied at once and reached `user.toml` 1 s later; after a game restart the tab showed the saved values.
- Leaving Options runs the game's apply and save: `UserSettings.cfg` stayed byte-identical to the backup taken before the test; `Save.dat` kept its size and changed in one float (26421 -> 26840, a counter that grows between sessions: likely play time, written by every autosave).

## 5. Runtime checks before writing anything

1. `state+0x650[0..8]` values on the first Options open (expect -1 except tabs 1..3).
2. A log in the `GetString16(int)` detour showing the tab-button and row-title ids.
3. Slider range and value read back after moving a slider.
4. The tab button widget pointer at `state+0x478 + 4*8` is non-null.
