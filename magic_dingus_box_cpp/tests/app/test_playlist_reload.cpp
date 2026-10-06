// The web-admin playlist reload (data/playlists_reload_request): swapping
// the playlist vectors under a running kiosk and repairing every index that
// pointed into the old ones. Moved out of main.cpp verbatim
// (app/playlist_reload.h); the cases below are the failure modes its
// comments name — an import renumbering playlists under the playing video,
// the playing playlist deleted, Master Shuffle losing its source or its
// whole pool, and the Settings game list vanishing under the operator.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "app/app_state.h"
#include "app/playlist_reload.h"

namespace {

app::PlaylistItem item(const std::string& title, const std::string& type = "local") {
    app::PlaylistItem it;
    it.title = title;
    it.path = title + (type == "emulated_game" ? ".sfc" : ".mp4");
    it.source_type = type;
    return it;
}

app::Playlist playlist(const std::string& name, std::vector<app::PlaylistItem> items) {
    app::Playlist p;
    p.title = name;
    p.path = "/data/playlists/" + name + ".yaml";
    p.items = std::move(items);
    return p;
}

// What the loader returns from disk (before the UI split).
std::vector<app::Playlist> disk_v1() {
    return {
        playlist("cartoons", {item("a"), item("b"), item("c")}),
        playlist("movies", {item("m1"), item("m2")}),
        playlist("snes", {item("mario", "emulated_game"), item("zelda", "emulated_game")}),
    };
}

// Boot state from disk_v1, through the same split boot uses.
void boot(app::AppState& s, const std::vector<app::Playlist>& disk) {
    auto split = app::split_for_ui_with_master_shuffle(disk);
    s.playlists = std::move(split.video);
    s.game_playlists = std::move(split.games);
}

int stops = 0;
const std::function<void()> kStop = []() { ++stops; };

void reload(app::AppState& s, const std::vector<app::Playlist>& disk,
            int open_game_index = -1) {
    const auto snap = app::snapshot_for_reload(s, open_game_index);
    app::apply_reloaded_playlists(s, app::split_for_ui_with_master_shuffle(disk),
                                  snap, kStop);
}

}  // namespace

TEST_CASE("the UI split prepends the virtual Master Shuffle row", "[playlist_reload]") {
    auto split = app::split_for_ui_with_master_shuffle(disk_v1());
    REQUIRE(split.video.size() == 3);
    CHECK(split.video[0].title == "Master Shuffle");
    CHECK(split.video[0].path.empty());
    CHECK(split.video[0].items.size() == 1);
    CHECK(split.video[1].title == "cartoons");
    CHECK(split.video[2].title == "movies");
    REQUIRE(split.games.size() == 1);
    CHECK(split.games[0].title == "snes");
}

TEST_CASE("playlist identity is the file path, the title only for the virtual row",
          "[playlist_reload]") {
    auto split = app::split_for_ui_with_master_shuffle(disk_v1());
    CHECK(app::playlist_identity(split.video[0]) == "title:Master Shuffle");
    CHECK(app::playlist_identity(split.video[1]) == "path:/data/playlists/cartoons.yaml");
    CHECK(app::find_playlist_by_identity(split.video, "path:/data/playlists/movies.yaml") == 2);
    CHECK(app::find_playlist_by_identity(split.video, "path:/nope.yaml") == -1);
}

TEST_CASE("an import before the playing playlist re-anchors playback",
          "[playlist_reload]") {
    stops = 0;
    app::AppState s;
    boot(s, disk_v1());
    s.current_playlist_index = 2;  // movies
    s.current_item_index = 1;      // m2
    s.video_active = true;
    s.selected_index = 2;
    s.last_advanced_item_index = 1;
    s.last_advanced_duration = 90.0;
    s.shuffle_queue = {2, 0, 1};
    s.shuffle_queue_playlist_id = 2;
    s.master_shuffle_queue = {{1, 0}};
    s.shuffle_history = {{1, 2}};

    auto disk = disk_v1();
    disk.insert(disk.begin(), playlist("anime", {item("x")}));
    reload(s, disk);

    CHECK(stops == 0);
    CHECK(s.video_active);  // never interrupted
    CHECK(s.playlists[s.current_playlist_index].title == "movies");
    CHECK(s.current_playlist_index == 3);
    CHECK(s.current_item_index == 1);
    CHECK(s.selected_index == 3);  // the cursor follows its playlist
    CHECK(s.last_advanced_item_index == -1);
    CHECK(s.last_advanced_duration == 0.0);
    CHECK(s.shuffle_queue.empty());
    CHECK(s.shuffle_queue_playlist_id == -1);
    CHECK(s.master_shuffle_queue.empty());
    CHECK(s.shuffle_history.empty());
}

TEST_CASE("an item edited out of the playing playlist clamps the cursor",
          "[playlist_reload]") {
    stops = 0;
    app::AppState s;
    boot(s, disk_v1());
    s.current_playlist_index = 1;  // cartoons
    s.current_item_index = 2;      // c
    s.video_active = true;

    auto disk = disk_v1();
    disk[0].items = {item("a"), item("b")};  // "c" deleted
    reload(s, disk);

    CHECK(stops == 0);
    CHECK(s.current_playlist_index == 1);
    CHECK(s.current_item_index == 1);  // clamped to the last item
}

TEST_CASE("deleting the playing playlist stops to the menu", "[playlist_reload]") {
    stops = 0;
    app::AppState s;
    boot(s, disk_v1());
    s.current_playlist_index = 2;  // movies
    s.current_item_index = 0;
    s.video_active = true;
    s.ui_visible_when_playing = false;

    auto disk = disk_v1();
    disk.erase(disk.begin() + 1);  // movies gone
    reload(s, disk);

    CHECK(stops == 1);
    CHECK_FALSE(s.video_active);
    CHECK(s.current_playlist_index == -1);
    CHECK(s.current_item_index == -1);
    CHECK(s.ui_visible_when_playing);
    CHECK_FALSE(s.master_shuffle_active);
}

TEST_CASE("Master Shuffle survives losing its source playlist while a pool remains",
          "[playlist_reload]") {
    stops = 0;
    app::AppState s;
    boot(s, disk_v1());
    s.master_shuffle_active = true;
    s.current_playlist_index = 2;  // the source of the current random pick
    s.current_item_index = 0;
    s.video_active = true;

    auto disk = disk_v1();
    disk.erase(disk.begin() + 1);  // movies gone, cartoons remain
    reload(s, disk);

    CHECK(stops == 0);
    CHECK(s.master_shuffle_active);
    CHECK(s.video_active);
    CHECK(s.current_playlist_index == 0);  // parked on the virtual row
    CHECK(s.current_item_index == 0);
}

TEST_CASE("Master Shuffle with no pool left stops instead of spinning",
          "[playlist_reload]") {
    stops = 0;
    app::AppState s;
    boot(s, disk_v1());
    s.master_shuffle_active = true;
    s.current_playlist_index = 1;
    s.current_item_index = 0;
    s.video_active = true;

    // Only the game playlist is left: the video side is the virtual row alone.
    reload(s, {disk_v1()[2]});

    CHECK(stops == 1);
    CHECK_FALSE(s.master_shuffle_active);
    CHECK(s.current_playlist_index == -1);
}

TEST_CASE("nothing playing: no stop, the auto-advance guard is left alone",
          "[playlist_reload]") {
    stops = 0;
    app::AppState s;
    boot(s, disk_v1());
    s.last_advanced_item_index = 4;
    reload(s, disk_v1());
    CHECK(stops == 0);
    CHECK(s.last_advanced_item_index == 4);
}

TEST_CASE("the menu cursor clamps when its playlist is gone, and the scroll follows",
          "[playlist_reload]") {
    stops = 0;
    std::vector<app::Playlist> many;
    for (int i = 0; i < 20; ++i) {
        many.push_back(playlist("p" + std::to_string(i), {item("v" + std::to_string(i))}));
    }
    app::AppState s;
    boot(s, many);
    s.playlist_max_visible = 8;
    s.selected_index = 20;  // p19, the last row
    s.playlist_scroll_offset = 13;

    // p19 deleted along with ten others: 9 playlists + the virtual row.
    many.resize(9);
    reload(s, many);

    CHECK(s.selected_index == 9);  // old index, clamped to the new last row
    CHECK(s.playlist_scroll_offset == 2);  // 10 rows - 8 visible
}

TEST_CASE("the open Settings game list is re-found by identity", "[playlist_reload]") {
    app::AppState s;
    boot(s, disk_v1());
    auto disk = disk_v1();
    disk.insert(disk.begin(), playlist("genesis", {item("sonic", "emulated_game")}));

    const auto snap = app::snapshot_for_reload(s, /*open_game_playlist_index=*/0);
    CHECK(snap.open_game_id == "path:/data/playlists/snes.yaml");
    app::apply_reloaded_playlists(s, app::split_for_ui_with_master_shuffle(disk),
                                  snap, kStop);
    REQUIRE(s.game_playlists.size() == 2);

    SECTION("moved: re-enter at its new index") {
        const auto r = app::remap_open_game_list(s.game_playlists, snap.open_game_id, 0);
        CHECK(r.action == app::OpenGameListRemap::Action::Enter);
        CHECK(r.index == 1);
    }
    SECTION("unmoved: keep") {
        const auto r = app::remap_open_game_list(s.game_playlists, snap.open_game_id, 1);
        CHECK(r.action == app::OpenGameListRemap::Action::Keep);
    }
    SECTION("deleted: back out to the list") {
        const auto r = app::remap_open_game_list(
            s.game_playlists, "path:/data/playlists/gone.yaml", 0);
        CHECK(r.action == app::OpenGameListRemap::Action::Exit);
    }
    CHECK(app::games_in_game_playlist(s.game_playlists, 1) == 2);
    CHECK(app::games_in_game_playlist(s.game_playlists, 5) == 0);
}

TEST_CASE("no open game list: nothing to re-find", "[playlist_reload]") {
    app::AppState s;
    boot(s, disk_v1());
    const auto snap = app::snapshot_for_reload(s, -1);
    CHECK(snap.open_game_id.empty());
    CHECK(snap.playing_id.empty());
    const auto r = app::remap_open_game_list(s.game_playlists, snap.open_game_id, 0);
    CHECK(r.action == app::OpenGameListRemap::Action::Exit);
}
