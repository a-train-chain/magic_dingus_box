// Tests for the per-item trim points (start/end) and the per-playlist loop flag.
//
// All three keys were written by both writers and present in every real
// playlist on the box, but the loader never parsed them and playback passed
// hardcoded 0.0/0.0/false — so trimming silently did nothing and the editor's
// Loop checkbox had no effect at all.
//
// These tests pin the parsing, including the defensive paths: a malformed value
// must not take the whole playlist down with it, since load_playlists() catches
// per file and a throwing item would drop every other item alongside it.

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "app/playlist_loader.h"

namespace fs = std::filesystem;

namespace {

// Writes `body` to a temp .yaml and loads it. Caller gets the parsed Playlist.
app::Playlist load_yaml(const std::string& body) {
    const fs::path dir = fs::temp_directory_path() / "mdb_trim_loop_test";
    fs::create_directories(dir);
    static int counter = 0;
    const fs::path file = dir / ("pl_" + std::to_string(counter++) + ".yaml");
    {
        std::ofstream out(file);
        out << body;
    }
    app::Playlist pl = app::PlaylistLoader::load_playlist(file.string());
    fs::remove(file);
    return pl;
}

const char* kTwoVideos = R"(title: Trim Test
curator: ''
items:
  - title: A
    source_type: local
    path: media/a.mp4
    start: 12
    end: 34.5
  - title: B
    source_type: local
    path: media/b.mp4
)";

}  // namespace

TEST_CASE("start and end are parsed from an item", "[playlist][trim]") {
    const app::Playlist pl = load_yaml(kTwoVideos);
    REQUIRE(pl.items.size() == 2);

    CHECK(pl.items[0].start == 12.0);
    CHECK(pl.items[0].end == 34.5);

    // An item that declares neither keeps the 0.0 sentinel, which playback
    // reads as "no trim".
    CHECK(pl.items[1].start == 0.0);
    CHECK(pl.items[1].end == 0.0);
}

TEST_CASE("an end at or before start is ignored rather than trusted",
          "[playlist][trim]") {
    // Both of these would otherwise stop playback the instant it began.
    const app::Playlist equal = load_yaml(R"(title: T
items:
  - path: media/a.mp4
    start: 10
    end: 10
)");
    REQUIRE(equal.items.size() == 1);
    CHECK(equal.items[0].end == 0.0);
    CHECK(equal.items[0].start == 10.0);

    const app::Playlist inverted = load_yaml(R"(title: T
items:
  - path: media/a.mp4
    start: 30
    end: 5
)");
    REQUIRE(inverted.items.size() == 1);
    CHECK(inverted.items[0].end == 0.0);
}

TEST_CASE("a negative start is clamped to zero", "[playlist][trim]") {
    const app::Playlist pl = load_yaml(R"(title: T
items:
  - path: media/a.mp4
    start: -5
)");
    REQUIRE(pl.items.size() == 1);
    CHECK(pl.items[0].start == 0.0);
}

TEST_CASE("a malformed trim value does not discard the playlist",
          "[playlist][trim]") {
    // yaml-cpp throws on a bad numeric cast. Unguarded that would propagate out
    // of load_playlist(), and load_playlists() would drop the ENTIRE file —
    // losing the other, perfectly good items with it.
    const app::Playlist pl = load_yaml(R"(title: T
items:
  - title: Bad
    path: media/a.mp4
    start: "not a number"
    end: [1, 2]
  - title: Good
    path: media/b.mp4
)");
    REQUIRE(pl.items.size() == 2);
    CHECK(pl.items[0].start == 0.0);
    CHECK(pl.items[0].end == 0.0);
    CHECK(pl.items[1].title == "Good");
}

TEST_CASE("the per-playlist loop flag is parsed", "[playlist][loop]") {
    CHECK(load_yaml("title: T\nloop: true\nitems: []\n").loop == true);
    CHECK(load_yaml("title: T\nloop: false\nitems: []\n").loop == false);
}

TEST_CASE("loop defaults to false when absent", "[playlist][loop]") {
    CHECK(load_yaml("title: T\nitems: []\n").loop == false);
}

TEST_CASE("a non-boolean loop value falls back to false rather than throwing",
          "[playlist][loop]") {
    const app::Playlist pl = load_yaml(R"(title: T
loop: "yes please"
items:
  - path: media/a.mp4
)");
    CHECK(pl.loop == false);
    // and the items still survived
    REQUIRE(pl.items.size() == 1);
}

TEST_CASE("trim parsing does not disturb the existing item fields",
          "[playlist][trim]") {
    const app::Playlist pl = load_yaml(R"(title: T
items:
  - title: Banjo
    source_type: emulated_game
    path: data/roms/n64/Banjo.z64
    emulator_core: mupen64plus_next_libretro
    emulator_system: n64
    start: 3
)");
    REQUIRE(pl.items.size() == 1);
    const auto& it = pl.items[0];
    CHECK(it.title == "Banjo");
    CHECK(it.source_type == "emulated_game");
    CHECK(it.emulator_core == "mupen64plus_next_libretro");
    CHECK(it.emulator_system == "n64");
    CHECK(it.start == 3.0);
}
