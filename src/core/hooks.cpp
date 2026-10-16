#include "core/hooks.hpp"

#include "core/log.hpp"

#include <MinHook.h>

#include <mutex>

namespace core::hooks {
namespace {

std::mutex g_mutex;
bool g_initialized = false;
std::vector<HookInfo> g_hooks;
int g_failed = 0;

} // namespace

bool Init()
{
    std::lock_guard lock(g_mutex);
    if (g_initialized) {
        return true;
    }
    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        TT2VR_LOG_ERROR("MH_Initialize failed: {}", MH_StatusToString(status));
        return false;
    }
    g_initialized = true;
    return true;
}

Result Create(const char* name, void* target, void* detour, void** original)
{
    std::lock_guard lock(g_mutex);
    if (!g_initialized || !target) {
        TT2VR_LOG_ERROR("hook {}: {}", name, g_initialized ? "null target" : "MinHook not initialized");
        ++g_failed;
        return Result::Failed;
    }
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status == MH_ERROR_ALREADY_CREATED) {
        return Result::AlreadyHooked;
    }
    if (status == MH_OK) {
        status = MH_EnableHook(target);
        if (status != MH_OK) {
            MH_RemoveHook(target);
        }
    }
    if (status != MH_OK) {
        TT2VR_LOG_ERROR("hook {} at {}: {}", name, target, MH_StatusToString(status));
        ++g_failed;
        return Result::Failed;
    }
    g_hooks.push_back({name, target, true});
    return Result::Ok;
}

void RemoveAll()
{
    std::lock_guard lock(g_mutex);
    if (!g_initialized) {
        return;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    for (const auto& h : g_hooks) {
        MH_RemoveHook(h.target);
    }
    g_hooks.clear();
}

std::vector<HookInfo> Installed()
{
    std::lock_guard lock(g_mutex);
    return g_hooks;
}

int FailedCount()
{
    std::lock_guard lock(g_mutex);
    return g_failed;
}

} // namespace core::hooks
