#include "game/camera_noise.hpp"

#include "core/log.hpp"
#include "game/engine_layout.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <windows.h>

namespace game {
namespace {

using namespace layout;

struct Entry {
    const void* settings = nullptr;
    float original[2] = {}; // bob, roll
    float written[2] = {};
};
std::vector<Entry> g_entries; // one per CockpitSettings seen (views x bikes); main thread
constexpr std::size_t kMaxEntries = 32;
Entry g_last; // the live view's entry after the last ApplyCameraNoise (settings null: none)

// SEH: tuning data owned by the game. False if the object cannot be read or written.
bool ReadAmplitudes(const void* settings, float out[2])
{
    __try {
        out[0] = Read<float>(settings, kCockpitSpeedNoiseAmp);
        out[1] = Read<float>(settings, kCockpitSpeedRollNoiseAmp);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteAmplitudes(const void* settings, const float v[2])
{
    __try {
        void* s = const_cast<void*>(settings);
        Write<float>(s, kCockpitSpeedNoiseAmp, v[0]);
        Write<float>(s, kCockpitSpeedRollNoiseAmp, v[1]);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

void ApplyCameraNoise(const void* settings, float scale)
{
    g_last = {};
    float now[2];
    if (!settings || !ReadAmplitudes(settings, now)) {
        return;
    }
    auto it = std::find_if(g_entries.begin(), g_entries.end(), [&](const Entry& e) { return e.settings == settings; });
    if (it == g_entries.end()) {
        if (g_entries.size() >= kMaxEntries) {
            g_entries.erase(g_entries.begin()); // forget the oldest pointer; it is never written again
        }
        g_entries.push_back({settings, {now[0], now[1]}, {now[0], now[1]}});
        it = g_entries.end() - 1;
        TT2VR_LOG_INFO("camera noise: view settings {} (bob amplitude {}, roll amplitude {})", settings, now[0], now[1]);
    }
    else if (now[0] != it->written[0] || now[1] != it->written[1]) {
        TT2VR_LOG_INFO("camera noise: view settings {} changed by the game ({} {} -> {} {}); new originals", settings,
            it->written[0], it->written[1], now[0], now[1]);
        it->original[0] = now[0];
        it->original[1] = now[1];
    }
    const float k = std::clamp(scale, 0.0f, 1.0f);
    const float want[2] = {it->original[0] * k, it->original[1] * k};
    if ((want[0] != now[0] || want[1] != now[1]) && WriteAmplitudes(settings, want)) {
        it->written[0] = want[0];
        it->written[1] = want[1];
    }
    else {
        it->written[0] = now[0];
        it->written[1] = now[1];
    }
    g_last = *it;
}

std::string DescribeCameraNoise()
{
    if (!g_last.settings) {
        return "speed noise: no cockpit view";
    }
    return fmt::format("speed noise amplitudes bob {:.4g} roll {:.4g} (game {:.4g} {:.4g})", g_last.written[0],
        g_last.written[1], g_last.original[0], g_last.original[1]);
}

} // namespace game
