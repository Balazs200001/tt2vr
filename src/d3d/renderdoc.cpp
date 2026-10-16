// renderdoc.cpp - RenderDoc in-application API (third_party/renderdoc/renderdoc_app.h, MIT).
// Only active when RenderDoc injected renderdoc.dll; we never load it ourselves.
#include "d3d/renderdoc.hpp"

#include "core/log.hpp"
#include "core/paths.hpp"

#include <renderdoc/renderdoc_app.h>
#include <spdlog/fmt/fmt.h>
#include <windows.h>

#include <vector>

namespace d3d::renderdoc {
namespace {

RENDERDOC_API_1_1_2* g_api = nullptr;
bool g_probed = false;

struct Pending {
    std::string name;
    std::function<void(const std::string&)> done;
    std::uint32_t capturesBefore = 0;
    std::uint64_t triggerFrame = 0;
    bool triggered = false;
};
std::vector<Pending> g_pending; // present thread only

RENDERDOC_API_1_1_2* Api()
{
    if (!g_probed) {
        g_probed = true;
        if (HMODULE mod = GetModuleHandleW(L"renderdoc.dll")) {
            auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"));
            if (getApi && getApi(eRENDERDOC_API_Version_1_1_2, reinterpret_cast<void**>(&g_api)) == 1) {
                int major = 0, minor = 0, patch = 0;
                g_api->GetAPIVersion(&major, &minor, &patch);
                TT2VR_LOG_INFO("RenderDoc in-app API {}.{}.{} available", major, minor, patch);
            }
            else {
                g_api = nullptr;
                TT2VR_LOG_WARN("renderdoc.dll is loaded but RENDERDOC_GetAPI failed");
            }
        }
    }
    return g_api;
}

void Finish(Pending& p, const std::string& result)
{
    TT2VR_LOG_INFO("{}", result);
    if (p.done) {
        p.done(result);
    }
}

} // namespace

bool Available()
{
    return Api() != nullptr;
}

void Request(const std::string& name, std::function<void(const std::string&)> onDone)
{
    Pending p{name.empty() ? std::string("tt2vr") : name, std::move(onDone)};
    if (!Api()) {
        Finish(p, "capture FAILED: RenderDoc is not attached (launch the game through RenderDoc)");
        return;
    }
    g_pending.push_back(std::move(p));
}

void Tick(std::uint64_t frame)
{
    if (g_pending.empty() || !g_api) {
        return;
    }
    Pending& p = g_pending.front();
    if (!p.triggered) {
        const auto templ = (core::RenderDocDir() / p.name).generic_u8string();
        g_api->SetCaptureFilePathTemplate(reinterpret_cast<const char*>(templ.c_str()));
        p.capturesBefore = g_api->GetNumCaptures();
        p.triggerFrame = frame;
        p.triggered = true;
        g_api->TriggerCapture(); // captures the frame that starts now (next Present to the one after)
        TT2VR_LOG_INFO("RenderDoc capture triggered at frame {} ({})", frame, reinterpret_cast<const char*>(templ.c_str()));
        return;
    }
    const std::uint32_t count = g_api->GetNumCaptures();
    if (count > p.capturesBefore) {
        char path[1024] = {};
        std::uint32_t length = sizeof(path);
        std::uint64_t timestamp = 0;
        g_api->GetCapture(count - 1, path, &length, &timestamp);
        Finish(p, fmt::format("capture ok {}", path));
        g_pending.erase(g_pending.begin());
    }
    else if (frame - p.triggerFrame > 300) {
        Finish(p, "capture FAILED: RenderDoc reported no new capture within 300 frames");
        g_pending.erase(g_pending.begin());
    }
}

bool Busy()
{
    return !g_pending.empty();
}

} // namespace d3d::renderdoc
