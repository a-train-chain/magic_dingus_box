// Tests for PlaylistLoader::split_for_ui — the partition that decides
// which UI surface can reach which playlist items:
//
//   - the MAIN MENU (lean-back: auto-advance, next/prev, Master Shuffle)
//     sees only a playlist's non-game items — games must never launch
//     from a surface that plays unattended
//   - the SETTINGS game browser (lean-forward: deliberate selection)
//     sees only a playlist's emulated_game items
//
// A mixed video+game playlist therefore appears on BOTH surfaces, each
// side holding only its kind. Before this partition existed, a mixed
// playlist rode the main menu whole (is_video_playlist() is true if ANY
// item is a video), so a finished video could hand the TV to RetroArch
// with nobody in the room — and the game items were meanwhile invisible
// to the Settings browser (is_game_playlist() requires ALL items to be
// games).
//
// The rules under test:
//   - pure video playlists: video side unchanged, absent from games side
//   - pure game playlists: games side unchanged, absent from video side
//   - mixed playlists: on both sides, item order preserved within each
//   - the video side keeps non-game/non-video oddball items (matching
//     the old main-menu behavior) but only qualifies if at least one
//     REAL video item remains
//   - playlists with no real videos and no games appear nowhere
//   - playlist order is preserved on each side

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "app/playlist_loader.h"

using app::Playlist;
using app::PlaylistItem;
using app::PlaylistLoader;

namespace {

PlaylistItem game_item(const std::string& title) {
    PlaylistItem it;
    it.source_type = "emulated_game";
    it.title = title;
    it.emulator_system = "nes";
    it.path = "data/roms/nes/" + title + ".nes";
    return it;
}

PlaylistItem video_item(const std::string& title) {
    PlaylistItem it;
    it.source_type = "local";
    it.title = title;
    it.path = "data/media/" + title + ".mp4";
    return it;
}

PlaylistItem oddball_item(const std::string& title) {
    PlaylistItem it;
    it.source_type = "hologram";  // unknown to every consumer
    it.title = title;
    return it;
}

Playlist playlist(const std::string& title, std::vector<PlaylistItem> items) {
    Playlist pl;
    pl.title = title;
    pl.items = std::move(items);
    return pl;
}

} // namespace

TEST_CASE("pure video playlist: video side unchanged, not a game playlist") {
    std::vector<Playlist> in;
    in.push_back(playlist("Music Videos", {video_item("a"), video_item("b")}));

    auto split = PlaylistLoader::split_for_ui(in);

    REQUIRE(split.video.size() == 1);
    REQUIRE(split.video[0].title == "Music Videos");
    REQUIRE(split.video[0].items.size() == 2);
    REQUIRE(split.games.empty());
}

TEST_CASE("pure game playlist: games side unchanged, not on the main menu") {
    std::vector<Playlist> in;
    in.push_back(playlist("games_nes", {game_item("mario"), game_item("zelda")}));

    auto split = PlaylistLoader::split_for_ui(in);

    REQUIRE(split.games.size() == 1);
    REQUIRE(split.games[0].title == "games_nes");
    REQUIRE(split.games[0].items.size() == 2);
    REQUIRE(split.games[0].items[0].title == "mario");
    REQUIRE(split.video.empty());
}

TEST_CASE("mixed playlist appears on both sides, each holding only its kind") {
    std::vector<Playlist> in;
    in.push_back(playlist("Party Mix",
                          {video_item("intro"), game_item("mario"),
                           video_item("outro"), game_item("zelda")}));

    auto split = PlaylistLoader::split_for_ui(in);

    REQUIRE(split.video.size() == 1);
    REQUIRE(split.video[0].title == "Party Mix");
    REQUIRE(split.video[0].items.size() == 2);
    REQUIRE(split.video[0].items[0].title == "intro");
    REQUIRE(split.video[0].items[1].title == "outro");

    REQUIRE(split.games.size() == 1);
    REQUIRE(split.games[0].title == "Party Mix");
    REQUIRE(split.games[0].items.size() == 2);
    REQUIRE(split.games[0].items[0].title == "mario");
    REQUIRE(split.games[0].items[1].title == "zelda");
}

TEST_CASE("no game item survives on the video side of any playlist") {
    std::vector<Playlist> in;
    in.push_back(playlist("Party Mix", {video_item("v"), game_item("g")}));
    in.push_back(playlist("games_nes", {game_item("mario")}));

    auto split = PlaylistLoader::split_for_ui(in);

    for (const auto& pl : split.video) {
        for (const auto& item : pl.items) {
            REQUIRE(item.source_type != "emulated_game");
        }
    }
}

TEST_CASE("video side keeps oddball items when a real video anchors the playlist") {
    std::vector<Playlist> in;
    in.push_back(playlist("Weird Mix",
                          {video_item("v"), oddball_item("odd"), game_item("g")}));

    auto split = PlaylistLoader::split_for_ui(in);

    REQUIRE(split.video.size() == 1);
    REQUIRE(split.video[0].items.size() == 2);  // video + oddball, game removed
    REQUIRE(split.video[0].items[1].title == "odd");
}

TEST_CASE("playlist of only oddball items appears on neither side") {
    std::vector<Playlist> in;
    in.push_back(playlist("Holograms", {oddball_item("a"), oddball_item("b")}));

    auto split = PlaylistLoader::split_for_ui(in);

    REQUIRE(split.video.empty());
    REQUIRE(split.games.empty());
}

TEST_CASE("empty playlist appears on neither side") {
    std::vector<Playlist> in;
    in.push_back(playlist("Empty", {}));

    auto split = PlaylistLoader::split_for_ui(in);

    REQUIRE(split.video.empty());
    REQUIRE(split.games.empty());
}

TEST_CASE("playlist order is preserved on each side") {
    std::vector<Playlist> in;
    in.push_back(playlist("Videos A", {video_item("a")}));
    in.push_back(playlist("games_nes", {game_item("mario")}));
    in.push_back(playlist("Party Mix", {video_item("v"), game_item("g")}));
    in.push_back(playlist("Videos B", {video_item("b")}));

    auto split = PlaylistLoader::split_for_ui(in);

    REQUIRE(split.video.size() == 3);
    REQUIRE(split.video[0].title == "Videos A");
    REQUIRE(split.video[1].title == "Party Mix");
    REQUIRE(split.video[2].title == "Videos B");

    REQUIRE(split.games.size() == 2);
    REQUIRE(split.games[0].title == "games_nes");
    REQUIRE(split.games[1].title == "Party Mix");
}
