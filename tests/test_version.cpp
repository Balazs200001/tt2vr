#include "core/config.hpp"
#include <doctest/doctest.h>

#include "game/version.hpp"

#include <string>

using game::ClassifyExe;
using game::VersionStatus;

namespace {
const std::string kKnown = "103d4b2e4d86786a7eb1810faa280a41cec6cadce1e8865165c3de8ba87a601a";
}

TEST_CASE("ClassifyExe known build")
{
    const auto v = ClassifyExe(kKnown, false);
    CHECK(v.status == VersionStatus::Known);
    CHECK(v.steamBuild == "4989319");
    CHECK(v.sha256 == kKnown);
    CHECK_FALSE(v.faked);
}

TEST_CASE("ClassifyExe is case-insensitive and stores the lower-case hash")
{
    std::string upper = kKnown;
    for (char& c : upper) {
        if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
    }
    const auto v = ClassifyExe(upper, false);
    CHECK(v.status == VersionStatus::Known);
    CHECK(v.steamBuild == "4989319");
    CHECK(v.sha256 == kKnown);
}

TEST_CASE("ClassifyExe unknown hashes")
{
    const auto v = ClassifyExe(std::string(64, 'a'), false);
    CHECK(v.status == VersionStatus::Unknown);
    CHECK(v.steamBuild.empty());
    CHECK(v.sha256 == std::string(64, 'a'));

    CHECK(ClassifyExe("", false).status == VersionStatus::Unknown);
    CHECK(ClassifyExe(kKnown.substr(0, 63), false).status == VersionStatus::Unknown);
}

TEST_CASE("ClassifyExe preserves the faked flag")
{
    CHECK(ClassifyExe(kKnown, true).faked);
    CHECK(ClassifyExe(kKnown, true).status == VersionStatus::Known);
    const auto v = ClassifyExe(std::string(64, '0'), true);
    CHECK(v.faked);
    CHECK(v.status == VersionStatus::Unknown);
}

TEST_CASE("VersionInfo defaults to NotChecked")
{
    const game::VersionInfo v;
    CHECK(v.status == VersionStatus::NotChecked);
    CHECK_FALSE(v.faked);
}

TEST_CASE("Faking the hash with 64 zeros from the config gives an unknown version")
{
    const auto r = core::ParseConfig("[debug]\nfake_exe_sha256 = \"" + std::string(64, '0') + "\"\n");
    CHECK(r.warnings.empty());
    REQUIRE(r.config.debug.fake_exe_sha256 == std::string(64, '0'));
    const auto info = game::ClassifyExe(r.config.debug.fake_exe_sha256, true);
    CHECK(info.status == game::VersionStatus::Unknown);
    CHECK(info.faked);

    // Common mistakes are rejected with a warning (and the real hash is used).
    CHECK_FALSE(core::ParseConfig("[debug]\nfake_exe_sha256 = \"" + std::string(63, '0') + "\"\n").warnings.empty());
    CHECK_FALSE(core::ParseConfig("[debug]\nfake_exe_sha256 = 0\n").warnings.empty());
}
