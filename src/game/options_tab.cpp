#include "game/options_tab.hpp"

#include "core/config.hpp"
#include "core/hooks.hpp"
#include "core/log.hpp"
#include "game/camera.hpp"
#include "game/engine_layout.hpp"
#include "game/signatures.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include <windows.h>

namespace game {
namespace {

using namespace layout;

using CreateFn = int (*)(void* state);
using SwitchTabFn = void (*)(void* state, void* pageEnum, int tab);
using StateFn = void (*)(void* state);
using ListChangeFn = void (*)(void* state, void* widget, unsigned index, void* p);
using GaugeFn = void (*)(void* state, void* widget, float value, void* p);
using OpenRowFn = void (*)(void* state, int row, void* widget);
using GetString16Fn = const wchar_t* (*)(const void* localeManager, int id);
using AllocFn = void* (*)(std::size_t size, int align, unsigned tag, const char* file, int line);
using FreeFn = void (*)(void* p, bool flag);

CreateFn g_create = nullptr;
SwitchTabFn g_switchTab = nullptr;
StateFn g_rowTypes = nullptr, g_fillRows = nullptr;
ListChangeFn g_listChange = nullptr;
GaugeFn g_gauge = nullptr;
OpenRowFn g_openRow = nullptr;
GetString16Fn g_getString16 = nullptr;
AllocFn g_alloc = nullptr;
FreeFn g_free = nullptr;
void* const* g_manager = nullptr; // &SettingsManager::s_pSingletonInstance

// TTOptionsState (docs/re/options-menu.md 1)
constexpr std::ptrdiff_t kStatePage = 0x590;
constexpr std::ptrdiff_t kStateTab = 0x598;
constexpr std::ptrdiff_t kStateTabLabels = 0x4c0; // int[5 pages][9 slots]
constexpr std::ptrdiff_t kStateTabGroups = 0x650; // int[9], group index per tab (any page)
constexpr int kDisplayPage = 3;
constexpr int kVrTab = 4;
constexpr std::ptrdiff_t kVrTabLabel = kStateTabLabels + (kDisplayPage * 9 + kVrTab) * 4; // 0x53c
constexpr std::ptrdiff_t kVrTabGroup = kStateTabGroups + kVrTab * 4; // 0x660
constexpr std::ptrdiff_t kTab1Group = kStateTabGroups + 1 * 4; // 0x654
constexpr int kLocaEmpty = -1; // PlayAll::Core::s_LocaIdEmpty
// SettingsManager / Settings: {+0 capacity, +4 count, +8 data}; Settings: name at +0x10 (SSO), preset at +0x30.
constexpr std::ptrdiff_t kArrayCap = 0x0, kArrayCount = 0x4, kArrayData = 0x8;
// ISetting
constexpr std::ptrdiff_t kSetName = 0x08, kSetNameLen = 0x18, kSetNameCap = 0x20;
constexpr std::ptrdiff_t kSetType = 0x28, kSetTitle = 0x2c, kSetTip = 0x30, kSetFlags16 = 0x34;
constexpr std::ptrdiff_t kSetValue = 0x38, kSetApplied = 0x3c, kSetLiveApply = 0x40, kSetCallbackCount = 0x5c;
constexpr std::ptrdiff_t kSetPopupFlags = 0x68; // 1 = no validation / restart popup
constexpr std::ptrdiff_t kSetListCount = 0x74, kSetListData = 0x78; // list types (5: 0x30-byte wide strings)
constexpr std::ptrdiff_t kSetCvarCap = 0x78, kSetCvarCount = 0x7c, kSetCvars = 0x80; // SettingFloat: ICVar*[]
constexpr std::ptrdiff_t kCvarMin = 0xb0, kCvarMax = 0xb8; // ICVar: float* minimum / maximum
constexpr int kTypeSlider = 4, kTypeTextList = 5;
constexpr int kVtableSlots = 9;
constexpr int kSlotDestructor = 0, kSlotSetValue = 8;

constexpr int kLocBase = 0x7f010000; // private loc ids (the game's table is far smaller)
constexpr int kLocTab = kLocBase;

// Engine wide string (0x30): 16 wchar_t SSO buffer, length +0x20, capacity +0x28.
struct EngineWString {
    wchar_t buf[16];
    std::size_t len;
    std::size_t cap;
};
static_assert(sizeof(EngineWString) == 0x30);

struct alignas(8) Block88 {
    unsigned char b[0x88];
};
struct alignas(8) Block38 {
    unsigned char b[0x38];
};
struct alignas(8) BlockC0 {
    unsigned char b[0xc0];
};

enum class Value {
    HorizonLock,
    Shake,
    WorldScale,
    IpdScale,
    SeatForward,
    SeatUp,
    HudDistance,
    HudHeight,
    RenderScale,
    FsrSharpening
};

// Sliders (type 4): the row shows the setting value x 100 as "N%"; the gauge works in its CVar range with a step of 1
// and the game hands SetSetting the gauge value / 100 (runtime 2026-10-21). So the range is in percent and the gauge
// opens with the value x 100 (HookedOpenRow).
struct RowDef {
    const char* name; // ISetting name (8..15 chars, SSO)
    const wchar_t* title;
    Value value;
    int type;
    float min, max; // slider: percent range of the gauge; list: first value (setting units), max unused
    float step; // list only
    int count; // list only
    const wchar_t* unit; // list label format, e.g. L"%+.0f cm"
    float labelScale; // value -> label number
};

constexpr RowDef kRows[] = {
    {"vrHorizonLock", L"Horizon lock", Value::HorizonLock, kTypeSlider, 0.0f, 100.0f, 0, 0, nullptr, 0},
    {"vrShake", L"Camera shake", Value::Shake, kTypeSlider, 0.0f, 100.0f, 0, 0, nullptr, 0},
    {"vrWorldScale", L"World scale", Value::WorldScale, kTypeSlider, 50.0f, 200.0f, 0, 0, nullptr, 0},
    {"vrIpdScale", L"IPD scale", Value::IpdScale, kTypeSlider, 50.0f, 200.0f, 0, 0, nullptr, 0},
    {"vrSeatFwd", L"Seat forward", Value::SeatForward, kTypeTextList, -0.10f, 0, 0.01f, 21, L"%+.0f cm", 100.0f},
    {"vrSeatUp", L"Seat height", Value::SeatUp, kTypeTextList, -0.10f, 0, 0.01f, 21, L"%+.0f cm", 100.0f},
    {"vrHudDist", L"HUD distance", Value::HudDistance, kTypeTextList, 0.40f, 0, 0.10f, 17, L"%.1f m", 1.0f},
    {"vrHudHeight", L"HUD height", Value::HudHeight, kTypeTextList, -0.30f, 0, 0.05f, 11, L"%+.0f cm", 100.0f},
    // PLAN 9e: [xr] render_scale, and [xr] fsr_sharpness shown as a strength (100% = 0 stops, FSR's strongest).
    {"vrRenderScale", L"Render scale (FSR)", Value::RenderScale, kTypeTextList, 0.50f, 0, 0.05f, 11, L"%.0f%%", 100.0f},
    {"vrFsrSharpen", L"FSR sharpening", Value::FsrSharpening, kTypeTextList, 0.0f, 0, 0.10f, 11, L"%.0f%%", 100.0f},
};
constexpr int kRowCount = static_cast<int>(std::size(kRows));

// Engine-side objects (never freed: the game only ever holds pointers to them).
Block88 g_settings[kRowCount];
void* g_settingPtrs[kRowCount];
BlockC0 g_cvars[kRowCount];
void* g_cvarPtrs[kRowCount];
float g_cvarRange[kRowCount][2];
std::vector<EngineWString> g_labels[kRowCount];
Block38 g_group;
void* g_vtableSlider[kVtableSlots];
void* g_vtableList[kVtableSlots];
bool g_ready = false;
int g_installs = 0;

// Our strings for GetString16: the tab, then one title per row.
const wchar_t* LocText(int id)
{
    if (id == kLocTab) {
        return L"VR";
    }
    const int row = id - kLocTab - 1;
    return row >= 0 && row < kRowCount ? kRows[row].title : nullptr;
}

void SetNarrowSso(void* base, std::ptrdiff_t at, std::ptrdiff_t lenAt, std::ptrdiff_t capAt, const char* text)
{
    const std::size_t len = std::min<std::size_t>(std::strlen(text), 15);
    std::memcpy(static_cast<char*>(base) + at, text, len);
    static_cast<char*>(base)[at + len] = '\0';
    Write<std::size_t>(base, lenAt, len);
    Write<std::size_t>(base, capAt, 15);
}

// ISetting vslot 0: scalar deleting destructor. The game never owns these objects.
void* __fastcall NoDestructor(void* self, unsigned)
{
    return self;
}

float CurrentValue(Value v)
{
    const CameraSettings& s = Settings();
    const auto& hud = core::GetConfig().hud;
    switch (v) {
    case Value::HorizonLock:
        return s.horizonLock / 100.0f;
    case Value::Shake:
        return s.shake / 100.0f;
    case Value::WorldScale:
        return s.worldScale;
    case Value::IpdScale:
        return s.ipdScale;
    case Value::SeatForward:
        return s.seat[0];
    case Value::SeatUp:
        return s.seat[1];
    case Value::HudDistance:
        return hud.distance;
    case Value::HudHeight:
        return hud.height;
    case Value::RenderScale:
        return core::GetConfig().xr.render_scale;
    case Value::FsrSharpening:
        return 1.0f - core::GetConfig().xr.fsr_sharpness / 2.0f;
    }
    return 0.0f;
}

void ApplyValue(Value v, float x)
{
    CameraSettings& s = Settings();
    switch (v) {
    case Value::HorizonLock:
        s.horizonLock = std::clamp(x, 0.0f, 1.0f) * 100.0f;
        break;
    case Value::Shake:
        s.shake = std::clamp(x, 0.0f, 1.0f) * 100.0f;
        break;
    case Value::WorldScale:
        s.worldScale = std::clamp(x, 0.5f, 2.0f);
        break;
    case Value::IpdScale:
        s.ipdScale = std::clamp(x, 0.5f, 2.0f);
        break;
    case Value::SeatForward:
        s.seat[0] = std::clamp(x, -0.5f, 0.5f);
        break;
    case Value::SeatUp:
        s.seat[1] = std::clamp(x, -0.5f, 0.5f);
        break;
    case Value::HudDistance:
    case Value::HudHeight: {
        core::Config c = core::GetConfig(); // read every frame by xr's HUD panel
        (v == Value::HudDistance ? c.hud.distance : c.hud.height) =
            v == Value::HudDistance ? std::clamp(x, 0.3f, 5.0f) : std::clamp(x, -1.0f, 1.0f);
        core::SetConfig(c);
        break;
    }
    case Value::RenderScale:
    case Value::FsrSharpening: {
        core::Config c = core::GetConfig(); // xr applies a render scale change once it has held (one resize)
        if (v == Value::RenderScale) {
            c.xr.render_scale = std::clamp(x, 0.5f, 1.0f);
        }
        else {
            c.xr.fsr_sharpness = std::clamp(2.0f * (1.0f - x), 0.0f, 2.0f);
        }
        core::SetConfig(c);
        break;
    }
    }
}

int RowOf(const void* setting)
{
    for (int i = 0; i < kRowCount; ++i) {
        if (setting == &g_settings[i]) {
            return i;
        }
    }
    return -1;
}

int ListIndex(const RowDef& r, float x)
{
    return std::clamp(static_cast<int>(std::lround((x - r.min) / r.step)), 0, r.count - 1);
}

// ISetting vslot 8 (SetValue, `bits` = SettingValue): called by SetSetting (live apply flag set), Apply and Revert.
// Thread: game main thread (Options screen).
void __fastcall SetValue(void* setting, std::uint32_t bits)
{
    const int i = RowOf(setting);
    if (i < 0) {
        return;
    }
    const RowDef& r = kRows[i];
    const float x = r.type == kTypeSlider ? std::bit_cast<float>(bits)
                                          : r.min + r.step * static_cast<float>(std::clamp<int>(static_cast<int>(bits), 0, r.count - 1));
    if (std::fabs(x - CurrentValue(r.value)) < 1e-5f) {
        return; // Apply on leaving the screen repeats unchanged values
    }
    ApplyValue(r.value, x);
    TT2VR_LOG_INFO("options tab: {} = {:.3f} (raw {:#010x})", r.name, x, bits);
}

// Writes each row's current value into its ISetting (+0x38 and the applied copy +0x3c).
void SyncRows()
{
    for (int i = 0; i < kRowCount; ++i) {
        const RowDef& r = kRows[i];
        const float x = CurrentValue(r.value);
        if (r.type == kTypeSlider) {
            Write<float>(&g_settings[i], kSetValue, x);
            Write<float>(&g_settings[i], kSetApplied, x);
        }
        else {
            Write<int>(&g_settings[i], kSetValue, ListIndex(r, x));
            Write<int>(&g_settings[i], kSetApplied, ListIndex(r, x));
        }
    }
}

bool BuildGroup()
{
    const auto* sliderVt = static_cast<void* const*>(SignatureAddress(Sig::SettingFloatVftable));
    const auto* listVt = static_cast<void* const*>(SignatureAddress(Sig::SettingBoolVftable));
    if (!sliderVt || !listVt) {
        return false;
    }
    for (int k = 0; k < kVtableSlots; ++k) {
        g_vtableSlider[k] = sliderVt[k];
        g_vtableList[k] = listVt[k];
    }
    for (void** vt : {g_vtableSlider, g_vtableList}) {
        vt[kSlotDestructor] = reinterpret_cast<void*>(&NoDestructor);
        vt[kSlotSetValue] = reinterpret_cast<void*>(&SetValue);
    }
    for (int i = 0; i < kRowCount; ++i) {
        const RowDef& r = kRows[i];
        void* s = &g_settings[i];
        std::memset(s, 0, sizeof(Block88));
        Write<void*>(s, 0, r.type == kTypeSlider ? static_cast<void*>(g_vtableSlider) : static_cast<void*>(g_vtableList));
        SetNarrowSso(s, kSetName, kSetNameLen, kSetNameCap, r.name);
        Write<int>(s, kSetType, r.type);
        Write<int>(s, kSetTitle, kLocTab + 1 + i);
        Write<int>(s, kSetTip, kLocTab + 1 + i);
        Write<std::uint16_t>(s, kSetFlags16, 0x0100);
        Write<std::uint8_t>(s, kSetLiveApply, 1); // SetSetting calls SetValue right away
        Write<int>(s, kSetCallbackCount, 0);
        Write<int>(s, kSetPopupFlags, 1);
        if (r.type == kTypeSlider) {
            g_cvarRange[i][0] = r.min;
            g_cvarRange[i][1] = r.max;
            std::memset(&g_cvars[i], 0, sizeof(BlockC0));
            Write<int>(&g_cvars[i], 0x7c, 1);
            Write<float*>(&g_cvars[i], kCvarMin, &g_cvarRange[i][0]);
            Write<float*>(&g_cvars[i], kCvarMax, &g_cvarRange[i][1]);
            g_cvarPtrs[i] = &g_cvars[i];
            Write<int>(s, kSetCvarCap, 1);
            Write<int>(s, kSetCvarCount, 1);
            Write<void**>(s, kSetCvars, &g_cvarPtrs[i]);
        }
        else {
            g_labels[i].assign(r.count, EngineWString{});
            for (int k = 0; k < r.count; ++k) {
                EngineWString& w = g_labels[i][k];
                const float x = r.min + r.step * static_cast<float>(k);
                float shown = std::round(x * r.labelScale * 10.0f) / 10.0f; // no "-0" from float steps
                std::wstring format = r.unit;
                if (std::fabs(shown) < 0.05f) {
                    shown = 0.0f;
                    if (const auto plus = format.find(L"%+"); plus != std::wstring::npos) {
                        format.erase(plus + 1, 1); // "0 cm", not "+0 cm"
                    }
                }
                const int n = std::swprintf(w.buf, 16, format.c_str(), shown);
                w.len = n > 0 ? static_cast<std::size_t>(n) : 0;
                w.cap = 15;
            }
            Write<int>(s, kSetListCount, r.count);
            Write<EngineWString*>(s, kSetListData, g_labels[i].data());
        }
        g_settingPtrs[i] = s;
    }
    std::memset(&g_group, 0, sizeof(g_group));
    Write<int>(&g_group, kArrayCap, kRowCount);
    Write<int>(&g_group, kArrayCount, kRowCount);
    Write<void**>(&g_group, kArrayData, g_settingPtrs);
    SetNarrowSso(&g_group, 0x10, 0x20, 0x28, "tt2vr");
    SyncRows();
    return true;
}

// Puts our group at manager data[count] (count unchanged) and makes it the Display page's tab 4. SEH: engine data.
bool Install(void* state)
{
    __try {
        void* mgr = g_manager ? *g_manager : nullptr;
        if (!mgr) {
            return false;
        }
        const int label = Read<int>(state, kVrTabLabel);
        const int group = Read<int>(state, kVrTabGroup);
        void** data = Read<void**>(mgr, kArrayData);
        if (label != kLocaEmpty && label != kLocTab) {
            return false; // the game uses the slot itself
        }
        if (label == kLocTab && group >= 0 && data && data[group] == &g_group) {
            return true; // already installed for this state
        }
        if (group != -1 && label != kLocTab) {
            return false;
        }
        const int count = Read<int>(mgr, kArrayCount);
        const int cap = Read<int>(mgr, kArrayCap);
        if (count < 0 || cap < count) {
            return false;
        }
        if (cap <= count) {
            const int newCap = count + 8;
            auto** grown = static_cast<void**>(g_alloc(static_cast<std::size_t>(newCap) * 8, 8, 9, "tt2vr options tab", 1));
            if (!grown) {
                return false;
            }
            if (count > 0) {
                std::memcpy(grown, data, static_cast<std::size_t>(count) * 8);
            }
            if (data) {
                g_free(data, false);
            }
            Write<void**>(mgr, kArrayData, grown);
            Write<int>(mgr, kArrayCap, newCap);
            data = grown;
        }
        data[count] = &g_group;
        Write<int>(state, kVrTabGroup, count);
        Write<int>(state, kVrTabLabel, kLocTab);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool OnVrTab(const void* state)
{
    __try {
        return Read<int>(state, kStatePage) == kDisplayPage && Read<int>(state, kStateTab) == kVrTab &&
               Read<int>(state, kVrTabLabel) == kLocTab && Read<int>(state, kVrTabGroup) >= 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Thread: game main thread (menu state machine push of the Options screen).
int HookedCreate(void* state)
{
    const int result = g_create(state);
    if (result == 0 && state && core::GetConfig().overlay.options_tab) {
        const bool ok = Install(state);
        ++g_installs;
        if (!ok || g_installs <= 3) {
            TT2VR_LOG_INFO("options tab: {} (Options opened, {} time(s))", ok ? "VR tab added to Display" : "not added",
                g_installs);
        }
    }
    return result;
}

// Thread: game main thread (Options screen: page switch, LB / RB).
void HookedSwitchTab(void* state, void* pageEnum, int tab)
{
    bool ours = false;
    __try {
        ours = pageEnum && Read<int>(pageEnum, 8) == kDisplayPage && tab == kVrTab && Read<int>(state, kVrTabLabel) == kLocTab;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        ours = false;
    }
    if (ours) {
        ours = Install(state); // re-check that data[+0x660] is still ours
        SyncRows();
    }
    g_switchTab(state, pageEnum, tab);
    if (ours && OnVrTab(state)) {
        g_rowTypes(state); // the game does this for tabs 1..3 only
        g_fillRows(state);
    }
}

// Runs a change handler as tab 1 with our group in tab 1's slot (the handlers accept tabs 1..3 / tab 1 only).
template <typename Fn>
void AsTabOne(void* state, Fn&& call)
{
    const int tab = Read<int>(state, kStateTab);
    const int tab1Group = Read<int>(state, kTab1Group);
    Write<int>(state, kStateTab, 1);
    Write<int>(state, kTab1Group, Read<int>(state, kVrTabGroup));
    call();
    Write<int>(state, kStateTab, tab);
    Write<int>(state, kTab1Group, tab1Group);
}

// Thread: game main thread (Options screen widget callbacks).
void HookedListChange(void* state, void* widget, unsigned index, void* p)
{
    if (!state || !OnVrTab(state)) {
        g_listChange(state, widget, index, p);
        return;
    }
    AsTabOne(state, [&] { g_listChange(state, widget, index, p); });
}

void HookedGauge(void* state, void* widget, float value, void* p)
{
    if (!state || !OnVrTab(state)) {
        g_gauge(state, widget, value, p);
        return;
    }
    TT2VR_LOG_INFO("options tab: gauge value {:.3f}", value);
    AsTabOne(state, [&] { g_gauge(state, widget, value, p); });
}

// Thread: game main thread (Options screen: a row activated). The gauge takes +0x38 as its value in the percent range
// of the CVar, so our sliders hold value x 100 while it is built.
void HookedOpenRow(void* state, int row, void* widget)
{
    if (!state || !OnVrTab(state)) {
        g_openRow(state, row, widget);
        return;
    }
    float saved[kRowCount] = {};
    for (int i = 0; i < kRowCount; ++i) {
        if (kRows[i].type == kTypeSlider) {
            saved[i] = Read<float>(&g_settings[i], kSetValue);
            Write<float>(&g_settings[i], kSetValue, saved[i] * 100.0f);
        }
    }
    g_openRow(state, row, widget);
    for (int i = 0; i < kRowCount; ++i) {
        if (kRows[i].type == kTypeSlider) {
            Write<float>(&g_settings[i], kSetValue, saved[i]);
        }
    }
}

// Thread: any caller of the locale table (UI on the main thread).
const wchar_t* HookedGetString16(const void* localeManager, int id)
{
    if (id >= kLocBase && id <= kLocBase + kRowCount) {
        if (const wchar_t* text = LocText(id)) {
            return text;
        }
    }
    return g_getString16(localeManager, id);
}

} // namespace

void InstallOptionsTab()
{
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;
    if (!core::GetConfig().overlay.options_tab) {
        TT2VR_LOG_INFO("options tab: off ([overlay] options_tab)");
        return;
    }
    void* create = SignatureAddress(Sig::OptionsCreate);
    void* switchTab = SignatureAddress(Sig::OptionsSwitchTab);
    void* listChange = SignatureAddress(Sig::OptionsListChange);
    void* gauge = SignatureAddress(Sig::OptionsGaugeValidate);
    void* openRow = SignatureAddress(Sig::OptionsOpenRow);
    void* getString = SignatureAddress(Sig::LocaleGetString16);
    g_rowTypes = reinterpret_cast<StateFn>(SignatureAddress(Sig::OptionsRowTypes));
    g_fillRows = reinterpret_cast<StateFn>(SignatureAddress(Sig::OptionsFillRows));
    g_manager = static_cast<void* const*>(SignatureAddress(Sig::SettingsManagerInstance));
    g_alloc = reinterpret_cast<AllocFn>(SignatureAddress(Sig::MemoryAllocAlignedInfo));
    g_free = reinterpret_cast<FreeFn>(SignatureAddress(Sig::MemoryFreeAlignedInfo));
    if (!create || !switchTab || !listChange || !gauge || !openRow || !getString || !g_rowTypes || !g_fillRows || !g_manager ||
        !g_alloc || !g_free || !BuildGroup()) {
        TT2VR_LOG_WARN("options tab: signatures missing; no VR tab in the game's Options");
        return;
    }
    using core::hooks::Result;
    // GetString16 first: once Create adds the tab, its label must resolve.
    const bool ok =
        core::hooks::Create("LocaleManager::GetString16(int)", getString, &HookedGetString16, &g_getString16) == Result::Ok &&
        core::hooks::Create("TTOptionsState::OnListChange", listChange, &HookedListChange, &g_listChange) == Result::Ok &&
        core::hooks::Create("TTOptionsState::OnGaugeValidate", gauge, &HookedGauge, &g_gauge) == Result::Ok &&
        core::hooks::Create("TTOptionsState::OpenRow", openRow, &HookedOpenRow, &g_openRow) == Result::Ok &&
        core::hooks::Create("TTOptionsState::SwitchTab", switchTab, &HookedSwitchTab, &g_switchTab) == Result::Ok &&
        core::hooks::Create("TTOptionsState::Create", create, &HookedCreate, &g_create) == Result::Ok;
    g_ready = ok;
    if (ok) {
        TT2VR_LOG_INFO("options tab: hooks installed ({} rows on Options > Display > VR)", kRowCount);
    }
    else {
        TT2VR_LOG_ERROR("options tab: hook install failed; the Options menu may lack the VR tab");
    }
}

} // namespace game
