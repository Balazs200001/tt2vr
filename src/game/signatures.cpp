#include "game/signatures.hpp"

#include "core/log.hpp"
#include "core/pattern.hpp"

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace game {
namespace {

std::array<void*, static_cast<std::size_t>(Sig::Count)> g_addresses{};

bool PatternMatchesAt(const core::Pattern& p, const std::uint8_t* at, std::span<const std::uint8_t> text)
{
    if (at < text.data() || at + p.size() > text.data() + text.size()) {
        return false;
    }
    for (std::size_t i = 0; i < p.size(); ++i) {
        if (p.mask[i] && at[i] != p.bytes[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

int ResolveSignatures()
{
    HMODULE exe = GetModuleHandleW(nullptr);
    const std::span<const std::uint8_t> text = core::ModuleSection(exe, ".text");
    const auto base = reinterpret_cast<std::uintptr_t>(exe);
    int missing = 0;
    for (std::size_t i = 0; i < std::size(kSignatures); ++i) {
        const Signature& sig = kSignatures[i];
        void* address = nullptr;
        const char* via = "none";
        std::optional<core::Pattern> pattern;
        if (sig.pattern) {
            pattern = core::ParsePattern(sig.pattern);
            if (!pattern) {
                TT2VR_LOG_ERROR("signature {}: invalid pattern", sig.name);
            }
        }
        if (sig.exportName) {
            address = reinterpret_cast<void*>(GetProcAddress(exe, sig.exportName));
            if (address) {
                via = "export";
                if (pattern && !PatternMatchesAt(*pattern, static_cast<const std::uint8_t*>(address), text)) {
                    TT2VR_LOG_WARN("signature {}: pattern does not match at the export address (fallback is stale)",
                        sig.name);
                }
            }
        }
        if (!address && pattern && !text.empty()) {
            const auto hits = core::FindAllPatterns(text, *pattern, 2);
            if (hits.size() == 1) {
                address = const_cast<std::uint8_t*>(text.data() + hits[0] + sig.offset);
                via = "pattern";
            }
            else if (hits.size() > 1) {
                TT2VR_LOG_ERROR("signature {}: pattern is ambiguous (2+ matches)", sig.name);
            }
        }
        g_addresses[i] = address;
        if (address) {
            TT2VR_LOG_INFO("signature {} -> TT2.exe+0x{:x} ({})", sig.name,
                reinterpret_cast<std::uintptr_t>(address) - base, via);
        }
        else {
            ++missing;
            TT2VR_LOG_ERROR("signature {}: NOT FOUND (export and pattern); dependent hooks stay off", sig.name);
        }
    }
    return missing;
}

void* SignatureAddress(Sig id)
{
    return g_addresses[static_cast<std::size_t>(id)];
}

} // namespace game
