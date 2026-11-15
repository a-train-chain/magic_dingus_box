// app::Controller's portable half (src/app/controller_playback.cpp) against
// a fake video::VideoPlayer: the video_active derivation and its debounce in
// update_state(), and NEXT / PREV / Master Shuffle stepping including the
// load-next retry loop. controller.cpp (kiosk-only) is not linked; its
// load_item_media is replaced by the sleep-free double in
// controller_test_doubles.cpp, which records "settle" where the kiosk waits
// 200 ms. Playlist items point at real temp files (present or deliberately
// absent), because the existence check is part of what is under test.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "app/app_state.h"
#include "app/controller.h"
#include "app/playlist_playback.h"
#include "controller_test_doubles.h"
#include "video/playback_error_policy.h"

namespace fs = std::filesystem;
using test_doubles::count;
using test_doubles::events;
using test_doubles::FakeVideoPlayer;

namespace {

struct Rig {
    fs::path dir;
    FakeVideoPlayer player;
    app::Controller controller{&player};
    app::AppState state;

    Rig() {
        std::random_device rd;
        dir = fs::temp_directory_path() /
              ("mdb_controller_playback_" + std::to_string(rd()));
        fs::create_directories(dir);
        events().clear();
        test_doubles::game_launch_ok() = true;
        controller.seed_shuffle_rng(1234);
        state.intro_complete = true;
        state.showing_intro_video = false;
        // Row 0 is the virtual Master Shuffle row: one dummy item, no file.
        app::Playlist ms;
        ms.title = "Master Shuffle";
        ms.items.push_back({});
        state.playlists.push_back(ms);
    }
    ~Rig() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // A local video item; the file is created only when `exists`.
    app::PlaylistItem video(const std::string& name, bool exists = true) {
        const fs::path p = dir / (name + ".mp4");
        if (exists) std::ofstream(p) << "x";
        app::PlaylistItem it;
        it.path = p.string();
        it.title = name;
        it.source_type = "local";
        return it;
    }

    // Adds a playlist of items named <prefix>0..N-1; `missing` lists the
    // indexes whose files are absent. Returns its playlist index.
    int add_playlist(const std::string& title, int n, std::vector<int> missing = {},
                     bool loop = false) {
        app::Playlist pl;
        pl.title = title;
        pl.loop = loop;
        for (int i = 0; i < n; ++i) {
            const bool absent =
                std::find(missing.begin(), missing.end(), i) != missing.end();
            pl.items.push_back(video(title + std::to_string(i), !absent));
        }
        state.playlists.push_back(pl);
        return static_cast<int>(state.playlists.size()) - 1;
    }

    void playing(int playlist, int item) {
        state.current_playlist_index = playlist;
        state.current_item_index = item;
    }

    // The basename load_file was last called with ("" if never).
    std::string last_loaded() const {
        for (auto it = events().rbegin(); it != events().rend(); ++it) {
            if (it->rfind("load_file ", 0) == 0) return it->substr(10);
        }
        return "";
    }
};

// ── update_state: video_active ───────────────────────────────────────────

TEST_CASE("update_state: rising edge on the first frame with a loaded, playing stream",
          "[controller][video_active]") {
    Rig r;
    r.player.duration = 100.0;
    r.player.playing = true;
    r.player.volume = 37.0;
    r.state.is_switching_playlist = true;
    r.state.last_advanced_item_index = 4;
    r.state.last_advanced_duration = 55.0;

    r.controller.update_state(r.state);

    CHECK(r.state.video_active);
    CHECK(r.state.original_volume == 37.0);       // read, not forced to 100
    CHECK_FALSE(r.state.is_switching_playlist);   // switch completed
    CHECK(r.state.last_advanced_item_index == -1);
    CHECK(r.state.last_advanced_duration == 100.0);
}

TEST_CASE("update_state: no video_active without a duration, or at EOS",
          "[controller][video_active]") {
    Rig r;
    SECTION("playing but duration 0 (not prerolled)") {
        r.player.playing = true;
        r.player.duration = 0.0;
        r.controller.update_state(r.state);
        CHECK_FALSE(r.state.video_active);
    }
    SECTION("position > 0 at EOS") {
        r.player.duration = 100.0;
        r.player.position = 100.0;
        r.player.eos = true;
        r.controller.update_state(r.state);
        CHECK_FALSE(r.state.video_active);
    }
    SECTION("paused mid-stream counts as active (position > 0)") {
        r.player.duration = 100.0;
        r.player.position = 12.0;
        r.player.paused = true;
        r.controller.update_state(r.state);
        CHECK(r.state.video_active);
        CHECK(r.state.paused);
    }
}

TEST_CASE("update_state: falling edge is debounced to the 4th consecutive negative frame",
          "[controller][video_active]") {
    Rig r;
    r.player.duration = 100.0;
    r.player.playing = true;
    r.controller.update_state(r.state);
    REQUIRE(r.state.video_active);

    r.player.playing = false;   // position 0, not playing -> should_be_active false
    for (int frame = 1; frame <= 3; ++frame) {
        r.controller.update_state(r.state);
        INFO("negative frame " << frame);
        CHECK(r.state.video_active);
    }
    r.controller.update_state(r.state);
    CHECK_FALSE(r.state.video_active);
}

TEST_CASE("update_state: one positive frame resets the debounce count",
          "[controller][video_active]") {
    Rig r;
    r.player.duration = 100.0;
    r.player.playing = true;
    r.controller.update_state(r.state);

    r.player.playing = false;
    for (int i = 0; i < 3; ++i) r.controller.update_state(r.state);
    REQUIRE(r.state.video_active);

    r.player.playing = true;                 // the transient ends
    r.controller.update_state(r.state);
    r.player.playing = false;
    for (int i = 0; i < 3; ++i) r.controller.update_state(r.state);
    CHECK(r.state.video_active);             // the count restarted
    r.controller.update_state(r.state);
    CHECK_FALSE(r.state.video_active);
}

TEST_CASE("update_state: a playlist switch never drops video_active, and the held "
          "negatives count against the debounce once it ends",
          "[controller][video_active]") {
    Rig r;
    r.player.duration = 100.0;
    r.player.playing = true;
    r.controller.update_state(r.state);
    r.state.is_switching_playlist = true;

    r.player.playing = false;
    for (int i = 0; i < 6; ++i) r.controller.update_state(r.state);
    CHECK(r.state.video_active);

    r.state.is_switching_playlist = false;
    r.controller.update_state(r.state);
    CHECK_FALSE(r.state.video_active);       // no fresh 4-frame grace
}

TEST_CASE("update_state: natural EOS ends video_active after the debounce",
          "[controller][video_active]") {
    Rig r;
    r.player.duration = 100.0;
    r.player.position = 50.0;
    r.player.playing = true;
    r.controller.update_state(r.state);
    REQUIRE(r.state.video_active);

    r.player.position = 100.0;
    r.player.playing = false;
    r.player.eos = true;
    for (int i = 0; i < 3; ++i) r.controller.update_state(r.state);
    CHECK(r.state.video_active);
    r.controller.update_state(r.state);
    CHECK_FALSE(r.state.video_active);
}

TEST_CASE("update_state: a real stop with no playlist clears the indexes and now-playing",
          "[controller][video_active]") {
    Rig r;
    r.player.duration = 100.0;
    r.player.playing = true;
    r.controller.update_state(r.state);
    r.state.current_playlist_index = -1;
    r.state.current_item_index = 3;
    r.state.now_playing_title = "Song";
    r.state.now_playing_kind = "video";

    r.player.playing = false;
    for (int i = 0; i < 4; ++i) r.controller.update_state(r.state);

    CHECK_FALSE(r.state.video_active);
    CHECK(r.state.current_item_index == -1);
    CHECK(r.state.now_playing_title.empty());
    CHECK(r.state.now_playing_kind.empty());
}

TEST_CASE("update_state: a stop with a playlist index keeps the indexes (switch in flight)",
          "[controller][video_active]") {
    Rig r;
    const int pl = r.add_playlist("v", 2);
    r.playing(pl, 1);
    r.player.duration = 100.0;
    r.player.playing = true;
    r.controller.update_state(r.state);
    r.state.now_playing_title = "Song";

    r.player.playing = false;
    for (int i = 0; i < 4; ++i) r.controller.update_state(r.state);

    CHECK_FALSE(r.state.video_active);
    CHECK(r.state.current_item_index == 1);
    CHECK(r.state.now_playing_title == "Song");
}

TEST_CASE("update_state: playback_started_ confirms once clear of the end",
          "[controller][video_active]") {
    Rig r;
    const int pl = r.add_playlist("v", 1);
    r.playing(pl, 0);
    r.player.duration = 100.0;
    r.player.playing = true;
    r.state.playback_started_ = false;

    r.player.position = 99.6;                // within the end window: not yet
    r.controller.update_state(r.state);
    CHECK_FALSE(r.state.playback_started_.load());

    r.player.position = 10.0;
    r.controller.update_state(r.state);
    CHECK(r.state.playback_started_.load());
}

// ── NEXT / PREV ──────────────────────────────────────────────────────────

TEST_CASE("load_next_item: steps to the following item", "[controller][next]") {
    Rig r;
    const int pl = r.add_playlist("v", 3);
    r.playing(pl, 0);
    r.state.playback_started_ = true;

    r.controller.load_next_item(r.state, "");

    CHECK(r.state.current_item_index == 1);
    CHECK(r.last_loaded() == "v1.mp4");
    CHECK(r.state.last_advanced_item_index == 0);
    CHECK_FALSE(r.state.playback_started_.load());
    CHECK(r.state.now_playing_title == "v1");
}

TEST_CASE("load_next_item: the end of a non-looping playlist goes back to the menu",
          "[controller][next]") {
    Rig r;
    const int pl = r.add_playlist("v", 3, {}, /*loop=*/false);
    r.playing(pl, 2);

    r.controller.load_next_item(r.state, "");

    CHECK(r.state.current_playlist_index == -1);
    CHECK(r.state.current_item_index == -1);
    CHECK(r.state.ui_visible_when_playing);
    CHECK(count("stop") == 1);
    CHECK(r.last_loaded().empty());
}

TEST_CASE("load_next_item: the end of a looping playlist wraps to item 0",
          "[controller][next]") {
    Rig r;
    const int pl = r.add_playlist("v", 3, {}, /*loop=*/true);
    r.playing(pl, 2);

    r.controller.load_next_item(r.state, "");

    CHECK(r.state.current_playlist_index == pl);
    CHECK(r.state.current_item_index == 0);
    CHECK(r.last_loaded() == "v0.mp4");
}

TEST_CASE("load_next_item: a game playlist uses the global loop setting",
          "[controller][next]") {
    Rig r;
    app::Playlist games;
    games.title = "games";
    games.loop = false;
    for (int i = 0; i < 2; ++i) {
        app::PlaylistItem g;
        g.title = "g" + std::to_string(i);
        g.source_type = "emulated_game";
        games.items.push_back(g);
    }
    r.state.playlists.push_back(games);
    r.playing(1, 1);
    r.state.playlist_loop = true;

    r.controller.load_next_item(r.state, "");

    CHECK(r.state.current_item_index == 0);
    CHECK(count("game g0") == 1);
}

TEST_CASE("load_previous_item: wraps from item 0 to the last item", "[controller][prev]") {
    Rig r;
    const int pl = r.add_playlist("v", 3);
    r.playing(pl, 0);

    r.controller.load_previous_item(r.state, "");

    CHECK(r.state.current_item_index == 2);
    CHECK(r.last_loaded() == "v2.mp4");
    CHECK(r.state.last_advanced_item_index == 0);
}

TEST_CASE("load_previous_item: a failed item is retried once, and only once",
          "[controller][prev]") {
    Rig r;
    const int pl = r.add_playlist("v", 4);
    r.player.fail_paths.insert(r.state.playlists[pl].items[1].path);
    r.playing(pl, 2);

    r.controller.load_previous_item(r.state, "");

    // Current behaviour, pinned as is: the retry reverts to the item that
    // was playing (2) and steps back from THERE — i.e. onto the same failed
    // item 1 again, not item 0. Bounded at two attempts either way.
    CHECK(r.state.current_item_index == 1);
    CHECK(count("load_file v1.mp4") == 2);
    CHECK(count("load_file v0.mp4") == 0);
}

// ── The load-next retry loop ─────────────────────────────────────────────

TEST_CASE("load_next_item: skips a run of missing files to the next playable item",
          "[controller][next][retry]") {
    Rig r;
    const int pl = r.add_playlist("v", 6, {1, 2, 3});
    r.playing(pl, 0);

    r.controller.load_next_item(r.state, "");

    CHECK(r.state.current_item_index == 4);
    CHECK(r.last_loaded() == "v4.mp4");
    CHECK(count("load_file v4.mp4") == 1);
    CHECK(count("play") == 1);
    CHECK_FALSE(r.state.playback_started_.load());
}

TEST_CASE("load_next_item: an item whose load fails is skipped like a missing one",
          "[controller][next][retry]") {
    Rig r;
    const int pl = r.add_playlist("v", 4);
    r.player.fail_paths.insert(r.state.playlists[pl].items[1].path);
    r.playing(pl, 0);

    r.controller.load_next_item(r.state, "");

    CHECK(r.state.current_item_index == 2);
    CHECK(r.last_loaded() == "v2.mp4");
}

TEST_CASE("load_next_item: retries wrap but stop before the item that was playing",
          "[controller][next][retry]") {
    Rig r;
    const int pl = r.add_playlist("v", 5, {0, 3, 4}, /*loop=*/true);
    r.playing(pl, 2);

    r.controller.load_next_item(r.state, "");

    CHECK(r.state.current_item_index == 1);   // 3, 4, 0 missing; 1 plays
    CHECK(r.last_loaded() == "v1.mp4");
}

TEST_CASE("load_next_item: when every other item fails, back to the menu with an error",
          "[controller][next][retry]") {
    Rig r;
    const int pl = r.add_playlist("v", 4, {1, 2, 3}, /*loop=*/true);
    r.playing(pl, 0);

    r.controller.load_next_item(r.state, "");

    CHECK(r.state.current_playlist_index == -1);
    CHECK(r.state.current_item_index == -1);
    CHECK(r.state.error_message == "No playable content in playlist");
    CHECK(count("play") == 0);
}

// ── Missing files must not block the render thread ───────────────────────
// Every load used to stop() the stream and wait 200 ms BEFORE finding out
// the file was missing, and load_next_item retried every remaining item
// inside one call: ~48 missing items in a row held the render thread past
// the 10 s systemd watchdog, and the kiosk was SIGABRTed on every NEXT or
// auto-advance into the run.

TEST_CASE("load_next_item: a missing file costs no stop and no settle",
          "[controller][next][retry][missing]") {
    Rig r;
    const int pl = r.add_playlist("v", 6, {1, 2, 3});
    r.playing(pl, 0);

    r.controller.load_next_item(r.state, "");

    CHECK(r.state.current_item_index == 4);
    // Only the load that plays pays: the retry loop's pre-stop and the
    // load's own stop + settle.
    CHECK(count("settle") == 1);
    CHECK(count("stop") == 2);
}

TEST_CASE("load_next_item: 48 missing items in a row cost one settle, not 48",
          "[controller][next][retry][missing]") {
    Rig r;
    std::vector<int> missing;
    for (int i = 1; i <= 48; ++i) missing.push_back(i);
    const int pl = r.add_playlist("v", 60, missing);
    r.playing(pl, 0);

    r.controller.load_next_item(r.state, "");

    CHECK(r.state.current_item_index == 49);
    CHECK(r.last_loaded() == "v49.mp4");
    CHECK(count("settle") == 1);
}

TEST_CASE("load_playlist_item: a missing file leaves the playing stream alone",
          "[controller][missing]") {
    Rig r;
    const int pl = r.add_playlist("v", 2, {1});
    r.player.playing = true;

    auto result = r.controller.load_playlist_item(r.state, r.state.playlists[pl], 1, "");

    CHECK_FALSE(result);
    CHECK(count("stop") == 0);
    CHECK(count("settle") == 0);
    CHECK(r.player.playing);
}

TEST_CASE("load_playlist_item: a start offset joins the item that far past its own start",
          "[controller][controller_playback][guide]") {
    // The Channel Guide joins an item mid-way: its scheduled offset counts
    // from the item's `start:` trim, not from the file's beginning.
    Rig r;
    const int pl = r.add_playlist("v", 1);
    r.state.playlists[pl].items[0].start = 10.0;

    SECTION("with an offset") {
        auto result = r.controller.load_playlist_item(r.state, r.state.playlists[pl], 0, "",
                                                      nullptr, 12.5);
        REQUIRE(result);
        CHECK(r.player.last_start == 22.5);
    }
    SECTION("without one: the item's start, as before") {
        auto result = r.controller.load_playlist_item(r.state, r.state.playlists[pl], 0, "");
        REQUIRE(result);
        CHECK(r.player.last_start == 10.0);
    }
    CHECK(r.last_loaded() == "v0.mp4");
    CHECK(count("play") == 1);
}

TEST_CASE("load_next_item: unloadable items are retried at most kMaxSyncLoadRetries "
          "times per call, then handed to the per-frame policy",
          "[controller][next][retry][bound]") {
    Rig r;
    const int pl = r.add_playlist("v", 20);
    for (int i = 1; i <= 12; ++i) {
        r.player.fail_paths.insert(r.state.playlists[pl].items[i].path);
    }
    r.playing(pl, 0);
    constexpr int K = app::Controller::kMaxSyncLoadRetries;

    r.controller.load_next_item(r.state, "");

    CHECK(count("settle") == 1 + K);                 // first attempt + K retries
    CHECK(r.state.current_item_index == 1 + K);      // the last one that failed
    CHECK(r.state.playlist_item_load_failed);
    CHECK(r.state.current_playlist_index == pl);     // not back to the menu
    CHECK(r.state.error_message.empty());
    CHECK(count("play") == 0);
}

TEST_CASE("load_next_item: missing files do not count against the retry bound",
          "[controller][next][retry][bound]") {
    Rig r;
    const int pl = r.add_playlist("v", 30, {2, 3, 4, 5, 6, 7, 8, 9});
    r.player.fail_paths.insert(r.state.playlists[pl].items[1].path);
    r.player.fail_paths.insert(r.state.playlists[pl].items[10].path);
    r.playing(pl, 0);

    r.controller.load_next_item(r.state, "");

    CHECK(r.state.current_item_index == 11);
    CHECK(r.last_loaded() == "v11.mp4");
    CHECK_FALSE(r.state.playlist_item_load_failed);
}

// The kiosk's ControllerTransport, over the real Controller + fake player:
// PlaylistPlayback driving Controller end to end, a frame at a time.
struct ControllerAdapter final : app::PlaylistTransport {
    app::Controller& c;
    FakeVideoPlayer& p;
    ControllerAdapter(app::Controller& controller, FakeVideoPlayer& player)
        : c(controller), p(player) {}
    void stop() override { c.stop(); }
    void play() override { c.play(); }
    void toggle_pause() override { c.toggle_pause(); }
    void seek(double s) override { c.seek(s); }
    bool is_playing() const override { return c.is_playing(); }
    bool is_paused() const override { return c.is_paused(); }
    utils::Result<> load_playlist_item(app::AppState& s, const app::Playlist& pl, int i,
                                       const std::string& d) override {
        return c.load_playlist_item(s, pl, i, d);
    }
    utils::Result<> load_playlist_item_at(app::AppState& s, const app::Playlist& pl, int i,
                                          const std::string& d, double offset_s) override {
        return c.load_playlist_item(s, pl, i, d, nullptr, offset_s);
    }
    void load_next_item(app::AppState& s, const std::string& d) override { c.load_next_item(s, d); }
    void load_previous_item(app::AppState& s, const std::string& d) override {
        c.load_previous_item(s, d);
    }
    void play_random_global_video(app::AppState& s, const std::string& d) override {
        c.play_random_global_video(s, d);
    }
    void master_shuffle_advance(app::AppState& s, const std::string& d) override {
        c.master_shuffle_advance(s, d);
    }
    void master_shuffle_back(app::AppState& s, const std::string& d) override {
        c.master_shuffle_back(s, d);
    }
    void toggle_picture_crop() override {}  // the media info store: not in this rig
    uint64_t stream_generation() const override { return p.generation; }
    bool has_error() const override { return p.has_error(); }
    double player_position() const override { return p.get_position(); }
    std::chrono::steady_clock::time_point now() const override {
        return std::chrono::steady_clock::time_point{std::chrono::hours(1000)};
    }
    double wall_now_utc() const override { return 0.0; }  // no schedule in this rig
};

TEST_CASE("a long run of unloadable items is walked a bounded chunk per frame",
          "[controller][retry][bound][playback]") {
    Rig r;
    const int pl = r.add_playlist("v", 30);
    for (int i = 1; i <= 20; ++i) {
        r.player.fail_paths.insert(r.state.playlists[pl].items[i].path);
    }
    r.playing(pl, 0);
    ControllerAdapter t(r.controller, r.player);
    const std::string dir;
    app::PlaylistPlayback pb(r.state, t, dir);

    r.controller.load_next_item(r.state, dir);   // NEXT / auto-advance
    int frames = 0;
    int max_settles_per_frame = count("settle");
    while (r.state.playlist_item_load_failed && frames < 50) {
        const int before = count("settle");
        pb.tick_pipeline_error();
        max_settles_per_frame = std::max(max_settles_per_frame, count("settle") - before);
        ++frames;
    }

    CHECK(r.state.current_item_index == 21);
    CHECK(r.last_loaded() == "v21.mp4");
    CHECK(count("play") == 1);
    CHECK(max_settles_per_frame <= 1 + app::Controller::kMaxSyncLoadRetries);
    CHECK(frames == 20 / (1 + app::Controller::kMaxSyncLoadRetries));
    CHECK(r.state.error_message.empty());
}

TEST_CASE("a playlist that never loads gives up after the policy's budget",
          "[controller][retry][bound][playback]") {
    Rig r;
    const int pl = r.add_playlist("v", 60);
    for (const auto& it : r.state.playlists[pl].items) r.player.fail_paths.insert(it.path);
    r.playing(pl, 0);
    ControllerAdapter t(r.controller, r.player);
    const std::string dir;
    app::PlaylistPlayback pb(r.state, t, dir);

    r.controller.load_next_item(r.state, dir);
    int frames = 0;
    while (r.state.playlist_item_load_failed && frames < 50) {
        pb.tick_pipeline_error();
        ++frames;
    }

    // failure_budget(60 items) = kMaxFailureBudget = 10 hand-offs.
    CHECK(frames == video::PlaybackErrorPolicy::kMaxFailureBudget);
    CHECK(r.state.current_playlist_index == -1);
    CHECK(r.state.error_message == "Couldn't play these videos");
    CHECK(count("play") == 0);
}

// ── Master Shuffle ───────────────────────────────────────────────────────

TEST_CASE("play_random_global_video: picks a real source item and activates the mode",
          "[controller][master_shuffle]") {
    Rig r;
    r.add_playlist("a", 2);
    r.add_playlist("b", 3);
    r.state.playback_started_ = true;
    r.state.last_advanced_item_index = 7;

    r.controller.play_random_global_video(r.state, "");

    CHECK(r.state.master_shuffle_active);
    CHECK(r.state.current_playlist_index >= 1);
    const auto& pl = r.state.playlists[r.state.current_playlist_index];
    REQUIRE(r.state.current_item_index >= 0);
    REQUIRE(r.state.current_item_index < static_cast<int>(pl.items.size()));
    CHECK(r.last_loaded() == pl.title + std::to_string(r.state.current_item_index) + ".mp4");
    CHECK(r.state.master_shuffle_queue.size() == 5);  // every source item once
    CHECK(r.state.master_shuffle_queue_position == 1);
    CHECK(r.state.last_advanced_item_index == -1);
    CHECK_FALSE(r.state.playback_started_.load());
}

TEST_CASE("play_random_global_video: nothing to play with only the virtual row",
          "[controller][master_shuffle]") {
    Rig r;
    r.controller.play_random_global_video(r.state, "");
    CHECK_FALSE(r.state.master_shuffle_active);
    CHECK(events().empty());
}

TEST_CASE("play_random_global_video: a queue entry gone stale after a reload regenerates",
          "[controller][master_shuffle]") {
    Rig r;
    r.add_playlist("a", 2);
    r.state.master_shuffle_queue = {{1, 9}};   // item 9 no longer exists
    r.state.master_shuffle_queue_position = 0;

    r.controller.play_random_global_video(r.state, "");

    CHECK(r.state.current_playlist_index == 1);
    CHECK(r.state.current_item_index >= 0);
    CHECK(r.state.current_item_index < 2);
    CHECK(count("play") == 1);
}

TEST_CASE("Master Shuffle: advance records history, back returns to it",
          "[controller][master_shuffle]") {
    Rig r;
    r.add_playlist("a", 3);
    r.add_playlist("b", 3);
    r.controller.play_random_global_video(r.state, "");
    const int first_pl = r.state.current_playlist_index;
    const int first_item = r.state.current_item_index;

    r.controller.master_shuffle_advance(r.state, "");
    REQUIRE(r.state.shuffle_history.size() == 1);
    CHECK(r.state.shuffle_history.back() == std::make_pair(first_pl, first_item));
    CHECK(std::make_pair(r.state.current_playlist_index, r.state.current_item_index) !=
          std::make_pair(first_pl, first_item));   // queue = no repeats

    r.state.playback_started_ = true;
    r.controller.master_shuffle_back(r.state, "");
    CHECK(r.state.current_playlist_index == first_pl);
    CHECK(r.state.current_item_index == first_item);
    CHECK(r.state.shuffle_history.empty());
    CHECK_FALSE(r.state.playback_started_.load());
    CHECK(r.last_loaded() == r.state.playlists[first_pl].title +
                                 std::to_string(first_item) + ".mp4");
}

TEST_CASE("Master Shuffle: back with no history picks another random video",
          "[controller][master_shuffle]") {
    Rig r;
    r.add_playlist("a", 3);
    r.controller.master_shuffle_back(r.state, "");
    CHECK(r.state.master_shuffle_active);
    CHECK(r.state.current_playlist_index == 1);
    CHECK(count("play") == 1);
}

TEST_CASE("Master Shuffle: back skips history entries a reload invalidated",
          "[controller][master_shuffle]") {
    Rig r;
    r.add_playlist("a", 3);
    r.state.master_shuffle_active = true;
    r.state.shuffle_history = {{1, 2}, {1, 8}, {5, 0}};   // newest last

    r.controller.master_shuffle_back(r.state, "");

    CHECK(r.state.current_playlist_index == 1);
    CHECK(r.state.current_item_index == 2);
    CHECK(r.last_loaded() == "a2.mp4");
}

TEST_CASE("load_next_item: in Master Shuffle it takes the next queued pick",
          "[controller][master_shuffle]") {
    Rig r;
    r.add_playlist("a", 2);
    r.add_playlist("b", 2);
    r.controller.play_random_global_video(r.state, "");
    const auto before = r.state.master_shuffle_queue_position;

    r.controller.load_next_item(r.state, "");

    CHECK(r.state.master_shuffle_queue_position == before + 1);
    CHECK(count("play") == 2);
}

// ── current_media_key (the per-video setting's key, app/media_info.h) ─────

TEST_CASE("current_media_key names the loaded video inside the data directory",
          "[controller][media_info]") {
    Rig r;
    const fs::path data = r.dir / "data";
    fs::create_directories(data / "media");
    fs::create_directories(data / "intro");
    fs::create_directories(data / "playlists");
    const fs::path a = data / "media" / "a.mp4";
    const fs::path intro = data / "intro" / "intro.mp4";
    const fs::path outside = r.dir / "outside.mp4";
    for (const auto& p : {a, intro, outside}) std::ofstream(p) << "x";
    r.controller.set_media_data_dir(data.string());

    CHECK(r.controller.current_media_key().empty());  // nothing loaded yet

    REQUIRE(r.controller.load_file_with_resolution(a.string(), ""));
    CHECK(r.controller.current_media_key() == "media/a.mp4");

    // However the playlist spelled the path (a relative step back out of
    // the playlists directory, as the resolver produces for `media/x.mp4`).
    REQUIRE(r.controller.load_file_with_resolution(
        (data / "playlists" / ".." / "media" / "a.mp4").string(), ""));
    CHECK(r.controller.current_media_key() == "media/a.mp4");

    // The boot intro is not a library video: no setting applies to it.
    REQUIRE(r.controller.load_file_with_resolution(intro.string(), ""));
    CHECK(r.controller.current_media_key().empty());

    REQUIRE(r.controller.load_file_with_resolution(a.string(), ""));
    REQUIRE(r.controller.load_file_with_resolution(outside.string(), ""));
    CHECK(r.controller.current_media_key().empty());

    // A failed load leaves no key behind from the video before it.
    REQUIRE(r.controller.load_file_with_resolution(a.string(), ""));
    CHECK_FALSE(r.controller.load_file_with_resolution(
        (data / "media" / "gone.mp4").string(), ""));
    CHECK(r.controller.current_media_key().empty());

    REQUIRE(r.controller.load_file_with_resolution(a.string(), ""));
    r.player.fail_paths.insert(a.string());
    CHECK_FALSE(r.controller.load_file_with_resolution(a.string(), ""));
    CHECK(r.controller.current_media_key().empty());
}

}  // namespace
