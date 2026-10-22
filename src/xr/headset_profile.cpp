#include "xr/headset_profile.hpp"

#include "core/log.hpp"
#include "core/paths.hpp"

#include <spdlog/fmt/fmt.h>

#include <fstream>
#include <sstream>

namespace xr {
namespace {

std::filesystem::path ProfilePath()
{
    return core::ModDir() / "headset_profile.txt";
}

} // namespace

HeadsetProfile LoadHeadsetProfile()
{
    HeadsetProfile p;
    std::ifstream in(ProfilePath());
    if (!in) {
        TT2VR_LOG_INFO("xr: no headset profile yet ({}); simulated headset uses defaults", ProfilePath().string());
        return p;
    }
    HeadsetProfile read;
    int fields = 0;
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) {
            continue;
        }
        std::string key = line.substr(0, eq);
        key.erase(key.find_last_not_of(" \t") + 1);
        std::istringstream value(line.substr(eq + 1));
        if (key == "system") {
            std::getline(value >> std::ws, read.system);
            ++fields;
        }
        else if (key == "recommended" && value >> read.recommendedW >> read.recommendedH) {
            ++fields;
        }
        else if (key == "ipd" && value >> read.ipd) {
            ++fields;
        }
        else if (key == "left_mask" || key == "right_mask") {
            std::vector<float>& m = read.mask[key == "left_mask" ? 0 : 1];
            float v = 0.0f;
            while (value >> v) {
                m.push_back(v);
            }
            if (m.size() % 6 != 0) {
                m.clear(); // whole triangles only
            }
        }
        else if ((key == "left_fov" || key == "right_fov")) {
            float* f = read.fov[key == "left_fov" ? 0 : 1];
            if (value >> f[0] >> f[1] >> f[2] >> f[3]) {
                ++fields;
            }
        }
    }
    if (fields < 5 || read.recommendedW < 16 || read.recommendedH < 16) {
        TT2VR_LOG_WARN("xr: headset profile {} incomplete ({} of 5 fields); using defaults", ProfilePath().string(),
            fields);
        return p;
    }
    return read;
}

void SaveHeadsetProfile(const HeadsetProfile& p)
{
    std::ofstream out(ProfilePath(), std::ios::trunc);
    if (!out) {
        TT2VR_LOG_WARN("xr: cannot write headset profile {}", ProfilePath().string());
        return;
    }
    out << "# tt2vr: the last real headset, written at session start. The simulated headset (test mode) uses it.\n";
    out << "system = " << p.system << "\n";
    out << fmt::format("recommended = {} {}\n", p.recommendedW, p.recommendedH);
    out << fmt::format("ipd = {:.5f}\n", p.ipd);
    for (int i = 0; i < 2; ++i) {
        out << fmt::format("{}_fov = {:.5f} {:.5f} {:.5f} {:.5f}\n", i == 0 ? "left" : "right", p.fov[i][0],
            p.fov[i][1], p.fov[i][2], p.fov[i][3]);
    }
    for (int i = 0; i < 2; ++i) {
        if (p.mask[i].empty()) {
            continue;
        }
        out << (i == 0 ? "left_mask =" : "right_mask =");
        for (const float v : p.mask[i]) {
            out << fmt::format(" {:.5f}", v);
        }
        out << "\n";
    }
    TT2VR_LOG_INFO("xr: headset profile saved ({}, {}x{}, ipd {:.4f} m)", p.system, p.recommendedW, p.recommendedH,
        p.ipd);
}

} // namespace xr
