// game/signatures.hpp: the table itself (patterns parse, names unique, validated on a known build).
#include <doctest/doctest.h>

#include "core/pattern.hpp"
#include "game/signatures.hpp"
#include "game/version.hpp"

#include <set>
#include <string>
#include <string_view>

TEST_CASE("Signature table entries are well-formed")
{
    std::set<std::string> names;
    for (const game::Signature& sig : game::kSignatures) {
        CAPTURE(sig.name);
        CHECK(names.insert(sig.name).second);
        CHECK((sig.exportName != nullptr || sig.pattern != nullptr));
        if (sig.exportName) {
            CHECK(std::string_view(sig.exportName).starts_with("?"));
        }
        if (sig.pattern) {
            const auto p = core::ParsePattern(sig.pattern);
            REQUIRE(p.has_value());
            CHECK(p->size() >= 16);
        }
        CHECK(game::ClassifyExe(sig.validatedOn, false).status == game::VersionStatus::Known);
        CHECK(std::string_view(sig.howFound).size() > 0);
    }
}
