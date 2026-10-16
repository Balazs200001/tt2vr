#include <doctest/doctest.h>

#include "core/pattern.hpp"

#include <cstdint>
#include <vector>

using core::FindAllPatterns;
using core::FindPattern;
using core::ParsePattern;

namespace {
using Bytes = std::vector<std::uint8_t>;
}

TEST_CASE("ParsePattern accepts valid patterns")
{
    SUBCASE("hex bytes, either case")
    {
        const auto p = ParsePattern("48 8b 05 E8 aF");
        REQUIRE(p.has_value());
        CHECK(p->size() == 5);
        CHECK(p->bytes == Bytes{0x48, 0x8B, 0x05, 0xE8, 0xAF});
        for (size_t i = 0; i < p->size(); ++i) CHECK(p->mask[i]);
    }
    SUBCASE("? and ?? are wildcards")
    {
        const auto p = ParsePattern("48 ?? 05 ? E8");
        REQUIRE(p.has_value());
        REQUIRE(p->size() == 5);
        CHECK(p->mask[0]);
        CHECK_FALSE(p->mask[1]);
        CHECK(p->mask[2]);
        CHECK_FALSE(p->mask[3]);
        CHECK(p->mask[4]);
        CHECK(p->bytes[0] == 0x48);
        CHECK(p->bytes[4] == 0xE8);
    }
    SUBCASE("extra whitespace")
    {
        const auto p = ParsePattern("  48   8B\t05 \n E8  ");
        REQUIRE(p.has_value());
        CHECK(p->size() == 4);
    }
    SUBCASE("single byte")
    {
        const auto p = ParsePattern("CC");
        REQUIRE(p.has_value());
        CHECK(p->size() == 1);
    }
}

TEST_CASE("ParsePattern rejects invalid patterns")
{
    CHECK_FALSE(ParsePattern("").has_value());
    CHECK_FALSE(ParsePattern("   ").has_value());
    CHECK_FALSE(ParsePattern("4").has_value());
    CHECK_FALSE(ParsePattern("48 4").has_value());
    CHECK_FALSE(ParsePattern("GG").has_value());
    CHECK_FALSE(ParsePattern("48 GG").has_value());
    CHECK_FALSE(ParsePattern("???").has_value());
    CHECK_FALSE(ParsePattern("48 ???").has_value());
    CHECK_FALSE(ParsePattern("4?").has_value());
    CHECK_FALSE(ParsePattern("?4").has_value());
    CHECK_FALSE(ParsePattern("488B").has_value());
    CHECK_FALSE(ParsePattern("0x48").has_value());
    CHECK_FALSE(ParsePattern("??").has_value());
    CHECK_FALSE(ParsePattern("? ?? ?").has_value());
}

TEST_CASE("FindPattern")
{
    const Bytes hay = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};

    SUBCASE("match at start, middle and end")
    {
        CHECK(FindPattern(hay, *ParsePattern("10 20")) == 0);
        CHECK(FindPattern(hay, *ParsePattern("40 50 60")) == 3);
        CHECK(FindPattern(hay, *ParsePattern("70 80")) == 6);
        CHECK(FindPattern(hay, *ParsePattern("80")) == 7);
    }
    SUBCASE("whole haystack")
    {
        CHECK(FindPattern(hay, *ParsePattern("10 20 30 40 50 60 70 80")) == 0);
    }
    SUBCASE("no match")
    {
        CHECK_FALSE(FindPattern(hay, *ParsePattern("20 40")).has_value());
        CHECK_FALSE(FindPattern(hay, *ParsePattern("FF")).has_value());
        CHECK_FALSE(FindPattern(hay, *ParsePattern("80 90")).has_value());
    }
    SUBCASE("haystack shorter than pattern")
    {
        CHECK_FALSE(FindPattern(hay, *ParsePattern("10 20 30 40 50 60 70 80 90")).has_value());
        CHECK_FALSE(FindPattern(Bytes{}, *ParsePattern("10")).has_value());
    }
    SUBCASE("wildcards")
    {
        CHECK(FindPattern(hay, *ParsePattern("20 ?? 40")) == 1);
        CHECK(FindPattern(hay, *ParsePattern("?? 30 ? 50")) == 1);
        CHECK(FindPattern(hay, *ParsePattern("?? ?? 60")) == 3);
        CHECK(FindPattern(hay, *ParsePattern("70 ?")) == 6);
        CHECK(FindPattern(hay, *ParsePattern("? 10")).has_value() == false); // wildcard cannot reach before the buffer
        CHECK_FALSE(FindPattern(hay, *ParsePattern("20 ?? 50")).has_value());
    }
    SUBCASE("leading wildcards near the start")
    {
        CHECK(FindPattern(hay, *ParsePattern("?? 20")) == 0);
    }
    SUBCASE("start parameter")
    {
        const Bytes rep = {0xAA, 0xBB, 0x00, 0xAA, 0xBB, 0x00, 0xAA, 0xBB};
        const auto p = *ParsePattern("AA BB");
        CHECK(FindPattern(rep, p) == 0);
        CHECK(FindPattern(rep, p, 0) == 0);
        CHECK(FindPattern(rep, p, 1) == 3);
        CHECK(FindPattern(rep, p, 3) == 3);
        CHECK(FindPattern(rep, p, 4) == 6);
        CHECK(FindPattern(rep, p, 6) == 6);
        CHECK_FALSE(FindPattern(rep, p, 7).has_value());
        CHECK_FALSE(FindPattern(rep, p, 1000).has_value());
    }
    SUBCASE("a false anchor hit does not hide a later match")
    {
        const Bytes h = {0x48, 0x00, 0x48, 0x8B, 0x05};
        CHECK(FindPattern(h, *ParsePattern("48 8B 05")) == 2);
    }
    SUBCASE("anchor after leading wildcards")
    {
        const Bytes h = {0x01, 0x02, 0x03, 0x02, 0x03};
        CHECK(FindPattern(h, *ParsePattern("?? 02 03")) == 0);
        CHECK(FindPattern(h, *ParsePattern("?? 02 03"), 1) == 2);
        CHECK(FindPattern(h, *ParsePattern("?? 02 03"), 2) == 2);
        CHECK_FALSE(FindPattern(h, *ParsePattern("?? 02 03"), 3).has_value());
    }
}

TEST_CASE("FindPattern on a large buffer")
{
    Bytes big(15 * 1024 * 1024, 0x90);
    big[big.size() - 5] = 0xE8;
    big[big.size() - 4] = 0x01;
    big[big.size() - 1] = 0xC3;
    const auto hit = FindPattern(big, *ParsePattern("E8 01 ?? ?? C3"));
    REQUIRE(hit.has_value());
    CHECK(*hit == big.size() - 5);
}

TEST_CASE("FindAllPatterns")
{
    SUBCASE("overlapping matches")
    {
        const Bytes hay = {0xAA, 0xAA, 0xAA};
        const auto hits = FindAllPatterns(hay, *ParsePattern("AA AA"));
        CHECK(hits == std::vector<size_t>{0, 1});
    }
    SUBCASE("limit")
    {
        const Bytes hay(10, 0xAA);
        CHECK(FindAllPatterns(hay, *ParsePattern("AA"), 3) == std::vector<size_t>{0, 1, 2});
        CHECK(FindAllPatterns(hay, *ParsePattern("AA"), 0).empty());
        CHECK(FindAllPatterns(hay, *ParsePattern("AA")).size() == 10);
        CHECK(FindAllPatterns(hay, *ParsePattern("AA"), 100).size() == 10);
    }
    SUBCASE("default limit is 16")
    {
        const Bytes hay(40, 0xAA);
        CHECK(FindAllPatterns(hay, *ParsePattern("AA")).size() == 16);
    }
    SUBCASE("no matches")
    {
        const Bytes hay = {1, 2, 3};
        CHECK(FindAllPatterns(hay, *ParsePattern("FF")).empty());
    }
    SUBCASE("with wildcards")
    {
        const Bytes hay = {0x01, 0x02, 0x03, 0x01, 0x09, 0x03};
        CHECK(FindAllPatterns(hay, *ParsePattern("01 ?? 03")) == std::vector<size_t>{0, 3});
    }
}
