// The playlist YAML fixture shared with the Content Manager's Python tests.
//
// tests/app/fixtures/playlist_all_fields.yaml carries every key the loader
// reads, a bare-string item, an item with no title, unknown top-level and
// per-item keys and the awkward strings (Yes, true, null, ~, 0x1F, a
// newline, '#', ': ', an emoji, a leading dash, a trailing space, a line
// longer than 80 columns). playlist_all_fields.emitted.yaml is what the web
// admin's format_playlist_yaml writes for it — committed, and kept in step by
// magic_dingus_box/web/tests/test_playlist_yaml_fixture.py, so this suite can
// read the Python-emitted form without running Python.
//
// Pinned here: the kiosk reads the hand-written file as intended, reads the
// emitted file IDENTICALLY, and the emitted form trips nothing in yaml-cpp
// (quoted scalars, a double-quoted escape, unknown keys it must ignore).

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "app/playlist_loader.h"
#include "app/rom_title.h"

namespace fs = std::filesystem;

namespace {

const fs::path kFixtures = fs::path(__FILE__).parent_path() / "fixtures";

app::Playlist load_fixture(const char* name) {
    const fs::path file = kFixtures / name;
    REQUIRE(fs::exists(file));
    return app::PlaylistLoader::load_playlist(file.string());
}

void check_all_fields(const app::Playlist& pl) {
    CHECK(pl.title == "All Fields");
    CHECK(pl.curator == "Fixture Curator");
    CHECK(pl.loop);
    REQUIRE(pl.items.size() == 10);

    const auto& full = pl.items[0];
    CHECK(full.title == "Full Video Item");
    CHECK(full.artist == "Some Artist");
    CHECK(full.source_type == "local");
    CHECK(full.path == "media/Full Video [abc123].mp4");
    CHECK(full.start == 12.0);
    CHECK(full.end == 34.5);
    CHECK(full.emulator_core.empty());

    const auto& game = pl.items[1];
    CHECK(game.title == "A Game");
    CHECK(game.source_type == "emulated_game");
    CHECK(game.path == "data/roms/nes/A Game (USA).nes");
    CHECK(game.emulator_core == "nestopia_libretro");
    CHECK(game.emulator_system == "NES");

    const auto& stream = pl.items[2];
    CHECK(stream.title == "A Stream");
    CHECK(stream.source_type == "youtube");
    CHECK(stream.path.empty());

    // A bare string is a local video titled from its filename.
    const auto& bare = pl.items[3];
    CHECK(bare.path == "media/Bare Path Video (USA).mp4");
    CHECK(bare.source_type == "local");
    CHECK(bare.title == app::title_from_rom_path(bare.path));
    CHECK(bare.artist.empty());

    // No title: derived from the filename, NOT "Untitled".
    const auto& untitled = pl.items[4];
    CHECK(untitled.path == "media/Untitled Clip (Europe).mp4");
    CHECK(untitled.title == app::title_from_rom_path(untitled.path));
    CHECK(untitled.title != "Untitled");
    CHECK(untitled.artist == "No Title Here");

    // as<std::string>() is the scalar's text, quoted or not.
    CHECK(pl.items[5].title == "Yes");
    CHECK(pl.items[5].artist == "0x1F");
    CHECK(pl.items[6].title == "true");
    CHECK(pl.items[6].artist == "null");
    CHECK(pl.items[7].title == "~");
    CHECK(pl.items[7].artist == "Yes");
    CHECK(pl.items[8].title == "0x1F");
    CHECK(pl.items[8].artist == "Line one\nLine two");
    CHECK(pl.items[8].path == "media/d #1: it's - fine \xF0\x9F\x8E\xB8.mp4");
    CHECK(pl.items[9].title == "- leading dash, trailing space ");
    CHECK(pl.items[9].artist ==
          "A very long artist name that goes on well past eighty columns so "
          "a folding emitter would wrap it");
}

}  // namespace

TEST_CASE("the hand-written all-fields fixture loads as intended",
          "[playlist][fixture]") {
    check_all_fields(load_fixture("playlist_all_fields.yaml"));
}

TEST_CASE("the web admin's emitted form of the fixture loads as intended",
          "[playlist][fixture]") {
    check_all_fields(load_fixture("playlist_all_fields.emitted.yaml"));
}

TEST_CASE("the emitted form means exactly what the source means",
          "[playlist][fixture]") {
    const app::Playlist src = load_fixture("playlist_all_fields.yaml");
    const app::Playlist out = load_fixture("playlist_all_fields.emitted.yaml");

    CHECK(out.title == src.title);
    CHECK(out.curator == src.curator);
    CHECK(out.loop == src.loop);
    REQUIRE(out.items.size() == src.items.size());
    for (size_t i = 0; i < src.items.size(); ++i) {
        INFO("item " << i);
        const auto& a = src.items[i];
        const auto& b = out.items[i];
        CHECK(b.path == a.path);
        CHECK(b.source_type == a.source_type);
        CHECK(b.title == a.title);
        CHECK(b.artist == a.artist);
        CHECK(b.emulator_core == a.emulator_core);
        CHECK(b.emulator_system == a.emulator_system);
        CHECK(b.start == a.start);
        CHECK(b.end == a.end);
    }
}
