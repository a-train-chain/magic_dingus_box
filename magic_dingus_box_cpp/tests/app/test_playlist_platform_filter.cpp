// Tests for PlaylistLoader::filter_for_platform — the runtime gate that
// keeps Pi 5-only game systems (N64, Dreamcast) off a Pi 4B's menu when
// both boards are flashed from the SAME golden image.
//
// The rules under test:
//   - emulated_game items whose emulator_system is in the profile's
//     unsupported list are removed; everything else passes untouched
//   - a playlist that LOSES all its items is dropped entirely
//   - a playlist that was already empty on disk passes through (hiding
//     pre-existing empties is not this gate's business)
//   - Pi 5 / Unknown profiles filter nothing at all

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "app/playlist_loader.h"
#include "platform/platform_profile.h"

using app::Playlist;
using app::PlaylistItem;
using app::PlaylistLoader;

namespace {

PlaylistItem game_item(const std::string& system) {
    PlaylistItem it;
    it.source_type = "emulated_game";
    it.emulator_system = system;
    it.path = "data/roms/" + system + "/game.bin";
    return it;
}

PlaylistItem video_item() {
    PlaylistItem it;
    it.source_type = "local";
    it.path = "data/media/clip.mp4";
    return it;
}

Playlist playlist(const std::string& title, std::vector<PlaylistItem> items) {
    Playlist pl;
    pl.title = title;
    pl.items = std::move(items);
    return pl;
}

} // namespace

TEST_CASE("Pi 4 drops an all-N64 playlist entirely") {
    auto profile = platform::profile_for(platform::PiModel::Pi4);
    std::vector<Playlist> in;
    in.push_back(playlist("Nintendo 64 Classics",
                          {game_item("N64"), game_item("N64")}));
    auto out = PlaylistLoader::filter_for_platform(std::move(in), profile);
    REQUIRE(out.empty());
}

TEST_CASE("Pi 4 drops an all-Dreamcast playlist entirely") {
    auto profile = platform::profile_for(platform::PiModel::Pi4);
    std::vector<Playlist> in;
    in.push_back(playlist("Dreamcast Classics", {game_item("Dreamcast")}));
    auto out = PlaylistLoader::filter_for_platform(std::move(in), profile);
    REQUIRE(out.empty());
}

TEST_CASE("Pi 4 keeps the original seven systems untouched") {
    auto profile = platform::profile_for(platform::PiModel::Pi4);
    std::vector<Playlist> in;
    in.push_back(playlist("SNES", {game_item("SNES"), game_item("SNES")}));
    in.push_back(playlist("Arcade", {game_item("Arcade")}));
    auto out = PlaylistLoader::filter_for_platform(std::move(in), profile);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].items.size() == 2);
    REQUIRE(out[1].items.size() == 1);
}

TEST_CASE("Pi 4 removes only the unsupported items from a mixed playlist") {
    // An operator could hand-build a playlist mixing videos and games —
    // the gate is per-item, so the playlist survives minus the N64 row.
    auto profile = platform::profile_for(platform::PiModel::Pi4);
    std::vector<Playlist> in;
    in.push_back(playlist("Mixed Bag",
                          {video_item(), game_item("N64"), game_item("NES")}));
    auto out = PlaylistLoader::filter_for_platform(std::move(in), profile);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].items.size() == 2);
    REQUIRE(out[0].items[0].source_type == "local");
    REQUIRE(out[0].items[1].emulator_system == "NES");
}

TEST_CASE("non-game source types never match even with a scary system string") {
    // source_type gates first: a local video that happens to carry an
    // emulator_system value (writer bug, hand edit) must not be hidden.
    auto profile = platform::profile_for(platform::PiModel::Pi4);
    PlaylistItem odd = video_item();
    odd.emulator_system = "N64";
    std::vector<Playlist> in;
    in.push_back(playlist("Odd", {odd}));
    auto out = PlaylistLoader::filter_for_platform(std::move(in), profile);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].items.size() == 1);
}

TEST_CASE("already-empty playlists pass through on every board") {
    auto profile = platform::profile_for(platform::PiModel::Pi4);
    std::vector<Playlist> in;
    in.push_back(playlist("Empty On Disk", {}));
    auto out = PlaylistLoader::filter_for_platform(std::move(in), profile);
    REQUIRE(out.size() == 1);
}

TEST_CASE("Pi 5 filters nothing") {
    auto profile = platform::profile_for(platform::PiModel::Pi5);
    std::vector<Playlist> in;
    in.push_back(playlist("Nintendo 64 Classics", {game_item("N64")}));
    in.push_back(playlist("Dreamcast Classics", {game_item("Dreamcast")}));
    auto out = PlaylistLoader::filter_for_platform(std::move(in), profile);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].items.size() == 1);
    REQUIRE(out[1].items.size() == 1);
}

TEST_CASE("Unknown (dev machine) filters nothing") {
    auto profile = platform::profile_for(platform::PiModel::Unknown);
    std::vector<Playlist> in;
    in.push_back(playlist("Nintendo 64 Classics", {game_item("N64")}));
    auto out = PlaylistLoader::filter_for_platform(std::move(in), profile);
    REQUIRE(out.size() == 1);
}
