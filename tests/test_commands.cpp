#include "core/commands.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

using core::CommandKind;

TEST_CASE("ParseCommands: keywords, aliases, comments, blank lines")
{
    const auto cmds = core::ParseCommands(
        "# request\n\nframetrace\r\nTRACE\n  dump  # with comment\nshaders\nfoo bar\nCamera\nxrprobe\n");
    REQUIRE(cmds.size() == 7);
    CHECK(cmds[0].kind == CommandKind::FrameTrace);
    CHECK(cmds[1].kind == CommandKind::FrameTrace);
    CHECK(cmds[2].kind == CommandKind::TextureDump);
    CHECK(cmds[2].text == "dump");
    CHECK(cmds[3].kind == CommandKind::ShaderRegistry);
    CHECK(cmds[4].kind == CommandKind::Unknown);
    CHECK(cmds[4].text == "foo bar");
    CHECK(cmds[5].kind == CommandKind::Camera);
    CHECK(cmds[6].kind == CommandKind::XrProbe);
}

TEST_CASE("ParseCommands: capture names are sanitized")
{
    const auto cmds = core::ParseCommands("capture snaefell_helmet-01\nrenderdoc ../evil name\ncapture\n");
    REQUIRE(cmds.size() == 3);
    CHECK(cmds[0].kind == CommandKind::RenderDocCapture);
    CHECK(cmds[0].arg == "snaefell_helmet-01");
    CHECK(cmds[1].kind == CommandKind::RenderDocCapture);
    CHECK(cmds[1].arg == "___evil_name");
    CHECK(cmds[2].arg.empty());
    const auto longName = core::ParseCommands("capture " + std::string(100, 'a'));
    CHECK(longName[0].arg.size() == 64);
}

TEST_CASE("TakeCommandFiles reads and deletes .txt requests; WriteDone writes .done")
{
    const auto dir = std::filesystem::temp_directory_path() / "tt2vr_test_commands";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "req1.txt") << "frametrace\ndump\n";
    std::ofstream(dir / "ignored.tmp") << "dump\n";

    auto files = core::TakeCommandFiles(dir);
    REQUIRE(files.size() == 1);
    CHECK(files[0].path.filename() == "req1.txt");
    CHECK(files[0].commands.size() == 2);
    CHECK_FALSE(std::filesystem::exists(dir / "req1.txt"));
    CHECK(std::filesystem::exists(dir / "ignored.tmp"));

    core::WriteDone(files[0].path, {"frametrace ok x", "dump ok y"});
    std::ifstream done(dir / "req1.done");
    std::string line1, line2;
    std::getline(done, line1);
    std::getline(done, line2);
    CHECK(line1 == "frametrace ok x");
    CHECK(line2 == "dump ok y");
    done.close();
    CHECK(core::TakeCommandFiles(dir).empty()); // .done is not a request
    std::filesystem::remove_all(dir);
}
