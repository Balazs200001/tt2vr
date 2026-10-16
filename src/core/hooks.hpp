#pragma once

#include <string>
#include <vector>

namespace core::hooks {

enum class Result {
    Ok,
    AlreadyHooked, // the target already carries one of our hooks (e.g. two vtables share a function)
    Failed,
};

struct HookInfo {
    std::string name;
    void* target = nullptr;
    bool enabled = false;
};

// MH_Initialize once. Returns false if MinHook is unusable (every Create then fails).
bool Init();

// Creates and enables a hook on `target`. `original` receives the trampoline. Never throws;
// failures are logged with `name` and leave the target untouched.
Result Create(const char* name, void* target, void* detour, void** original);

template <typename Fn>
Result Create(const char* name, void* target, Fn detour, Fn* original)
{
    return Create(name, target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
}

// Disables and removes every hook (process shutdown / emergency pass-through).
void RemoveAll();

// Snapshot of installed hooks, for the overlay.
std::vector<HookInfo> Installed();
int FailedCount();

} // namespace core::hooks
