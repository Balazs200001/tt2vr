#include <doctest/doctest.h>

#include "core/sha256.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::span<const std::uint8_t> AsBytes(const std::string& s)
{
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

} // namespace

TEST_CASE("Sha256Hex known vectors")
{
    CHECK(core::Sha256Hex({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string abc = "abc";
    CHECK(core::Sha256Hex(AsBytes(abc)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("Sha256FileHex")
{
    const auto dir = std::filesystem::temp_directory_path();
    const auto file = dir / "tt2vr_test_sha256.bin";

    SUBCASE("matches Sha256Hex for a file larger than one chunk")
    {
        std::vector<std::uint8_t> data(2 * 1024 * 1024 + 123);
        for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i * 31 + (i >> 8));
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }
        const auto hex = core::Sha256FileHex(file);
        std::filesystem::remove(file);
        REQUIRE(hex.has_value());
        CHECK(hex->size() == 64);
        CHECK(*hex == core::Sha256Hex(data));
    }
    SUBCASE("small file")
    {
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << "abc";
        }
        const auto hex = core::Sha256FileHex(file);
        std::filesystem::remove(file);
        REQUIRE(hex.has_value());
        CHECK(*hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }
    SUBCASE("empty file")
    {
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
        }
        const auto hex = core::Sha256FileHex(file);
        std::filesystem::remove(file);
        REQUIRE(hex.has_value());
        CHECK(*hex == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    }
    SUBCASE("nonexistent file")
    {
        CHECK_FALSE(core::Sha256FileHex(dir / "tt2vr_test_does_not_exist.bin").has_value());
    }
}
