// Pre-flight for an emulated_game launch, run BEFORE the game-session begin
// hook.
//
// The begin hook queues GameQuietMode's pause (qBittorrent pause_all + docker
// stop of the arr containers) and the end hook queues the resume. Both used
// to bracket the ROM/core validation too, so a launch that failed on a
// missing ROM or core still stopped and restarted three containers and
// paused every torrent — for a game that never ran. validate_game_launch()
// is everything that can fail without touching the system; Controller calls
// it first and only then fires the begin hook.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "app/app_state.h"
#include "app/game_launch_validation.h"

namespace fs = std::filesystem;

namespace {

// A real ROM file, because validation goes through the kiosk's own path
// resolver rather than a stub of it.
struct TempRom {
    fs::path dir;
    fs::path rom;
    TempRom() {
        dir = fs::temp_directory_path() / "mdb_game_launch_validation";
        fs::create_directories(dir);
        rom = dir / "game.sfc";
        std::ofstream(rom) << "rom";
    }
    ~TempRom() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

app::PlaylistItem game(const std::string& path, const std::string& core,
                       const std::string& system = "snes") {
    app::PlaylistItem item;
    item.title = "Test Game";
    item.source_type = "emulated_game";
    item.path = path;
    item.emulator_core = core;
    item.emulator_system = system;
    return item;
}

auto installed = [](const std::string&) { return true; };
auto not_installed = [](const std::string&) { return false; };

}  // namespace

TEST_CASE("a valid game resolves its core and ROM path", "[game_launch_validation]") {
    TempRom r;
    auto v = app::validate_game_launch(game(r.rom.string(), "snes9x2010_libretro"),
                                       "", installed);
    REQUIRE(v);
    CHECK(v.get().core_name == "snes9x2010_libretro");
    CHECK(fs::equivalent(v.get().rom_path, r.rom));
}

TEST_CASE("auto core resolves from the system", "[game_launch_validation]") {
    TempRom r;
    struct Case { const char* system; const char* core; };
    const Case cases[] = {
        {"genesis", "genesis_plus_gx"}, {"snes", "snes9x2010"},
        {"nes", "nestopia"},            {"ps1", "pcsx_rearmed"},
        {"psx", "pcsx_rearmed"},        {"atari7800", "prosystem"},
        {"pcengine", "mednafen_pce_fast"}, {"arcade", "fbneo"},
        {"n64", "mupen64plus_next"},    {"dreamcast", "flycast"},
    };
    for (const auto& c : cases) {
        auto v = app::validate_game_launch(game(r.rom.string(), "auto", c.system),
                                           "", installed);
        INFO(c.system);
        REQUIRE(v);
        CHECK(v.get().core_name == c.core);
    }
}

TEST_CASE("validation failures name the problem", "[game_launch_validation]") {
    TempRom r;

    auto no_core = app::validate_game_launch(game(r.rom.string(), ""), "", installed);
    REQUIRE_FALSE(no_core);
    CHECK(no_core.error().find("No emulator_core") != std::string::npos);

    auto bad_auto = app::validate_game_launch(
        game(r.rom.string(), "auto", "vectrex"), "", installed);
    REQUIRE_FALSE(bad_auto);
    CHECK(bad_auto.error().find("Could not resolve auto core") != std::string::npos);

    auto no_path = app::validate_game_launch(game("", "snes9x2010"), "", installed);
    REQUIRE_FALSE(no_path);
    CHECK(no_path.error().find("No ROM path") != std::string::npos);

    auto missing = app::validate_game_launch(
        game((r.dir / "nope" / "absent.sfc").string(), "snes9x2010"), "", installed);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().find("ROM file does not exist") != std::string::npos);

    auto no_so = app::validate_game_launch(game(r.rom.string(), "snes9x2010"), "",
                                           not_installed);
    REQUIRE_FALSE(no_so);
    CHECK(no_so.error() == "Emulator core not installed: snes9x2010_libretro");
}

TEST_CASE("the core check sees the RESOLVED core name", "[game_launch_validation]") {
    TempRom r;
    std::string asked;
    auto v = app::validate_game_launch(
        game(r.rom.string(), "auto", "n64"), "",
        [&](const std::string& core) { asked = core; return true; });
    REQUIRE(v);
    CHECK(asked == "mupen64plus_next");
}
