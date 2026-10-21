// game::OptionsTab (PLAN 8i): a "VR" tab in the game's own Options menu (Display page, tab slot 4) whose rows are the
// mod's runtime settings. Evidence: docs/re/options-menu.md. Everything is in memory; nothing on disk is changed.
//
// The Display page's tabs 1..3 are drawn generically from SettingsManager groups (state+0x650[tab] = group index).
// The mod builds one more group of hand-made ISetting rows (copies of the SettingFloat / SettingBool vtables with
// the destructor and SetValue replaced) and, after every TTOptionsState::Create (each Options open appends the
// game's three groups again):
//   - puts the group at SettingsManager data[count] without raising count, so the game's save, load, name lookups
//     and destructor never see it (they iterate count), while the index calls of the Options screen reach it;
//   - writes its index to state+0x660 (tab 4) and the tab's label id to state+0x53c (page 3, slot 4).
// Hooks (game main thread, the menu update):
//   TTOptionsState::Create          post: install as above.
//   TTOptionsState::SwitchTab       page 3 tab 4: rows synced from the settings before, row kinds and values after
//                                   (the game sets them for tabs 1..3 only).
//   OpenRow                         page 3 tab 4: sliders hold value x 100 while the gauge is built (it opens in
//                                   the percent range of the row's CVar with a step of 1).
//   OnListChange / OnGaugeValidate  page 3 tab 4: run as tab 1 with our group in the tab-1 slot (the game's handlers
//                                   only accept tabs 1..3 / tab 1), then the fields are restored.
//   LocaleManager::GetString16(int) the tab and row titles for our private ids.
// A row's SetValue applies the value at once (game::CameraSettings or [hud]); game::CameraOverride saves changes to
// tt2vr\user.toml. Fail safe: any missing signature or unexpected field leaves the menu as the game made it.
#pragma once

namespace game {

// After ResolveSignatures and InstallCameraHooks (main thread, between frames). [overlay] options_tab.
void InstallOptionsTab();

} // namespace game
