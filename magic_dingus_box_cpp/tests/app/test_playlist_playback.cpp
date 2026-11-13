// PlaylistPlayback: main.cpp's main-menu playlist control, moved out of the
// main loop verbatim. These pin the ORDER of Controller calls (a playlist
// switch must stop, wait, then load), which calls happen at all on each
// branch, and the AppState each branch leaves behind — the things a
// refactor of this code can silently change. Runs against a recording fake
// transport with a hand-advanced clock (no sleeps: the switch's waits are
// per-frame ticks); the kiosk adapter (app/controller_transport.h)
// forwards 1:1.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "app/app_state.h"
#include "app/channel_schedule.h"
#include "app/media_info.h"
#include "app/playback_reset.h"
#include "app/playlist_playback.h"

using Catch::Approx;

namespace {

// A start of channel 01's and channel 02's schedule cycles (600 s and 50 s
// long in the guide tests below): the epoch plus 1000 x 600 s.
const double kW0 = app::kScheduleEpochUtc + 600.0 * 1000;

struct FakeTransport : app::PlaylistTransport {
    std::vector<std::string> calls;
    // is_playing() answers, front first; `playing_default` once exhausted.
    std::deque<bool> playing_answers;
    bool playing_default = false;
    int playing_queries = 0;
    bool paused = false;
    bool load_ok = true;
    uint64_t generation = 1;
    bool error = false;
    double position = 0.0;
    // The hand-advanced clock behind now(). Starts well past the epoch so
    // "now - 3 s" is still a valid time point.
    std::chrono::steady_clock::time_point clock{std::chrono::hours(1000)};
    // Milliseconds since `t0` at which each load-type call happened.
    std::chrono::steady_clock::time_point t0 = clock;
    std::vector<long long> load_at_ms;
    // The wall clock behind wall_now_utc() (UTC seconds). advance() moves it
    // with the steady clock; a test may also jump it on its own.
    double wall = kW0;
    // load_playlist_item_at's offset_s, and the wall clock at that call.
    std::vector<double> offsets;
    std::vector<double> load_wall;

    void advance(std::chrono::milliseconds d) {
        clock += d;
        wall += static_cast<double>(d.count()) / 1000.0;
    }
    // The last call / offset, or "" / -1 when there was none.
    std::string last() const { return calls.empty() ? "" : calls.back(); }
    double last_offset() const { return offsets.empty() ? -1.0 : offsets.back(); }
    long long elapsed_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(clock - t0).count();
    }

    void stop() override { calls.push_back("stop"); }
    void play() override { calls.push_back("play"); }
    void toggle_pause() override { calls.push_back("toggle_pause"); }
    void seek(double s) override { calls.push_back("seek " + std::to_string(static_cast<int>(s))); }
    bool is_playing() const override {
        auto* self = const_cast<FakeTransport*>(this);
        ++self->playing_queries;
        if (!self->playing_answers.empty()) {
            const bool v = self->playing_answers.front();
            self->playing_answers.pop_front();
            return v;
        }
        return playing_default;
    }
    bool is_paused() const override { return paused; }
    utils::Result<> load_playlist_item(app::AppState&, const app::Playlist& pl, int idx,
                                       const std::string& dir) override {
        calls.push_back("load " + pl.title + " " + std::to_string(idx) + " " + dir);
        load_at_ms.push_back(elapsed_ms());
        return load_ok ? utils::Result<>::ok() : utils::Result<>::fail("boom");
    }
    utils::Result<> load_playlist_item_at(app::AppState&, const app::Playlist& pl, int idx,
                                          const std::string&, double offset_s) override {
        calls.push_back("load_at " + pl.title + " " + std::to_string(idx));
        load_at_ms.push_back(elapsed_ms());
        offsets.push_back(offset_s);
        load_wall.push_back(wall);
        return load_ok ? utils::Result<>::ok() : utils::Result<>::fail("boom");
    }
    void load_next_item(app::AppState&, const std::string&) override { calls.push_back("next_item"); }
    void load_previous_item(app::AppState&, const std::string&) override { calls.push_back("prev_item"); }
    void play_random_global_video(app::AppState&, const std::string&) override {
        calls.push_back("random_global");
        load_at_ms.push_back(elapsed_ms());
    }
    void master_shuffle_advance(app::AppState&, const std::string&) override {
        calls.push_back("shuffle_advance");
    }
    void master_shuffle_back(app::AppState&, const std::string&) override {
        calls.push_back("shuffle_back");
    }
    void toggle_picture_crop() override { calls.push_back("toggle_picture"); }
    uint64_t stream_generation() const override { return generation; }
    bool has_error() const override { return error; }
    double player_position() const override { return position; }
    std::chrono::steady_clock::time_point now() const override { return clock; }
    double wall_now_utc() const override { return wall; }
};

// Redirects a stream for the scope; count() = lines containing `needle`.
struct StreamCapture {
    std::ostream& stream;
    std::ostringstream buf;
    std::streambuf* old;
    explicit StreamCapture(std::ostream& s) : stream(s), old(s.rdbuf(buf.rdbuf())) {}
    ~StreamCapture() { stream.rdbuf(old); }
    int count(const std::string& needle) const {
        int n = 0;
        std::istringstream in(buf.str());
        for (std::string line; std::getline(in, line);) {
            n += line.find(needle) != std::string::npos;
        }
        return n;
    }
};

// Drives the main loop's per-frame switch tick: one tick every `frame`
// until `total` has passed (the kiosk ticks at vsync, ~16 ms).
void run_frames(app::PlaylistPlayback& pb, FakeTransport& t,
                std::chrono::milliseconds total,
                std::chrono::milliseconds frame = std::chrono::milliseconds(16)) {
    const auto end = t.clock + total;
    while (t.clock < end) {
        t.advance(frame);
        pb.tick_switch_timeout();
    }
}

app::PlaylistItem video_item(const std::string& title) {
    app::PlaylistItem it;
    it.path = title + ".mp4";
    it.title = title;
    it.source_type = "local";
    return it;
}

app::PlaylistItem game_item(const std::string& title) {
    app::PlaylistItem it;
    it.path = title + ".sfc";
    it.title = title;
    it.source_type = "emulated_game";
    return it;
}

// Row 0 = virtual Master Shuffle, row 1 = "Cartoons" (3 videos), row 2 =
// "Movies" (1 video), row 3 = a game-only playlist, row 4 = empty.
void put_menu(app::AppState& s) {
    app::Playlist ms;
    ms.title = "Master Shuffle";
    ms.items.push_back({});
    app::Playlist cartoons;
    cartoons.title = "Cartoons";
    cartoons.path = "/p/cartoons.yaml";
    cartoons.items = {video_item("a"), video_item("b"), video_item("c")};
    app::Playlist movies;
    movies.title = "Movies";
    movies.path = "/p/movies.yaml";
    movies.items = {video_item("m")};
    app::Playlist games;
    games.title = "Games";
    games.path = "/p/games.yaml";
    games.items = {game_item("g")};
    app::Playlist empty;
    empty.title = "Empty";
    empty.path = "/p/empty.yaml";
    s.playlists = {ms, cartoons, movies, games, empty};
    s.intro_complete = true;
    s.showing_intro_video = false;
}

void put_playing(app::AppState& s, int playlist, int item) {
    s.current_playlist_index = playlist;
    s.current_item_index = item;
    s.video_active = true;
    s.ui_visible_when_playing = false;
}

const std::string kDir = "/data/playlists";

}  // namespace

// ── SELECT ───────────────────────────────────────────────────────────────

TEST_CASE("SELECT does nothing while the intro plays", "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    s.showing_intro_video = true;
    s.selected_index = 1;
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);
    pb.on_select();
    CHECK(t.calls.empty());
    CHECK(s.current_playlist_index == -1);
}

TEST_CASE("SELECT over hidden-UI playback only reveals the UI", "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 0);
    s.selected_index = 2;
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);
    pb.on_select();
    CHECK(t.calls.empty());
    CHECK(s.ui_visible_when_playing);
    CHECK(s.ui_visibility_timer == 3.0);
    CHECK(s.current_playlist_index == 1);
}

TEST_CASE("SELECT on the playing playlist toggles the UI with a fade",
          "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 2);
    s.ui_visible_when_playing = true;
    s.selected_index = 1;
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);
    pb.on_select();
    CHECK(t.calls.empty());
    CHECK_FALSE(s.ui_visible_when_playing);
    CHECK(s.is_fading);
    CHECK_FALSE(s.fade_target_ui_visible);

    SECTION("Master Shuffle row counts as the playing playlist") {
        s.master_shuffle_active = true;
        s.selected_index = 0;
        s.ui_visible_when_playing = true;
        pb.on_select();
        CHECK(t.calls.empty());
        CHECK_FALSE(s.ui_visible_when_playing);
    }
}

// ── The Picture (crop) control, one stop below the last playlist row ─────

TEST_CASE("SELECT on the Picture control toggles crop and keeps the overlay",
          "[playlist_playback][picture]") {
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 2);
    s.ui_visible_when_playing = true;
    s.selected_index = 4;  // the last row, under the control
    s.playlist_scroll_offset = 1;
    s.picture_control_available = true;
    s.picture_focus = true;
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);

    pb.on_select();

    // Never a playlist switch, never a UI toggle, never a playback call.
    CHECK(t.calls == std::vector<std::string>{"toggle_picture"});
    CHECK(s.ui_visible_when_playing);
    CHECK_FALSE(s.is_fading);
    CHECK_FALSE(s.is_switching_playlist);
    CHECK_FALSE(pb.switch_pending());
    CHECK(s.current_playlist_index == 1);
    CHECK(s.current_item_index == 2);
    CHECK(s.selected_index == 4);
    CHECK(s.playlist_scroll_offset == 1);
    CHECK(s.picture_focus);

    // Every press toggles again.
    pb.on_select();
    CHECK(t.calls == std::vector<std::string>{"toggle_picture", "toggle_picture"});
}

TEST_CASE("SELECT with the Picture focus over hidden UI only reveals the UI",
          "[playlist_playback][picture]") {
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 2);  // overlay hidden
    s.selected_index = 4;
    s.picture_control_available = true;
    s.picture_focus = true;
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);

    pb.on_select();

    CHECK(t.calls.empty());
    CHECK(s.ui_visible_when_playing);
    CHECK(s.picture_focus);
    CHECK(s.current_playlist_index == 1);
    CHECK(s.current_item_index == 2);
}

TEST_CASE("SELECT in the load gap between items still toggles, or only reveals",
          "[playlist_playback][picture]") {
    // NEXT / auto-advance: video_active is briefly false while the next
    // stream prerolls, and the focus stays on the control through it.
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 2);
    s.video_active = false;
    s.selected_index = 4;
    s.picture_focus = true;
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);

    SECTION("overlay up: the press toggles, nothing starts") {
        s.ui_visible_when_playing = true;
        pb.on_select();
        CHECK(t.calls == std::vector<std::string>{"toggle_picture"});
        CHECK(s.ui_visible_when_playing);
    }
    SECTION("overlay hidden: the press only reveals it") {
        s.ui_visible_when_playing = false;
        pb.on_select();
        CHECK(t.calls.empty());
        CHECK(s.ui_visible_when_playing);
        CHECK(s.ui_visibility_timer == 3.0);
    }
    CHECK(s.current_playlist_index == 1);
    CHECK(s.current_item_index == 2);
    CHECK_FALSE(s.is_switching_playlist);
    CHECK(s.picture_focus);
}

TEST_CASE("SELECT with a stale Picture focus does nothing once a game takes the pipeline",
          "[playlist_playback][picture]") {
    // The focus is cleared each frame when the control goes away; a press
    // landing before that (a game took the pipeline) must neither toggle a
    // video nobody is watching nor start the row under the control.
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 2);
    s.ui_visible_when_playing = true;
    s.is_loading_game = true;
    s.selected_index = 4;
    s.picture_control_available = true;
    s.picture_focus = true;
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);

    pb.on_select();

    CHECK(t.calls.empty());
    CHECK(s.current_playlist_index == 1);
    CHECK(s.ui_visible_when_playing);
    CHECK_FALSE(s.is_switching_playlist);
}

// ── Mid-playback switch: stop, cooperative wait, load ────────────────────
// The old code slept 200 ms + up to 10 x 50 ms inside on_select (on the
// render thread). The same waits now elapse across tick_switch_timeout()
// calls; these pin that the calls, their order, the poll count, the load
// time and the end states are the old ones.

TEST_CASE("a mid-playback switch stops at once and loads after the 200 ms settle",
          "[playlist_playback][switch]") {
    using std::chrono::milliseconds;
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 2);
    s.ui_visible_when_playing = true;
    s.selected_index = 2;
    s.last_advanced_item_index = 2;
    s.last_advanced_duration = 10.0;
    FakeTransport t;
    // Still playing on the first poll after the settle, stopped after.
    t.playing_answers = {true, false, false};
    app::PlaylistPlayback pb(s, t, kDir);

    pb.on_select();
    // on_select returned without waiting: only the stop, and the state
    // writes that precede it, have happened.
    CHECK(t.calls == std::vector<std::string>{"stop"});
    CHECK(pb.switch_pending());
    CHECK(t.playing_queries == 0);
    CHECK(s.is_switching_playlist);
    CHECK(s.playlist_switch_start_time == t.clock);
    CHECK(s.current_playlist_index == 2);
    CHECK(s.current_item_index == 0);
    CHECK(s.last_advanced_item_index == -1);
    CHECK(s.last_advanced_duration == 0.0);
    CHECK(s.ui_visible_when_playing);  // still the menu until the load

    t.advance(milliseconds(199));
    pb.tick_switch_timeout();
    CHECK(t.calls.size() == 1);
    CHECK(t.playing_queries == 0);  // settling: the player is not even asked

    t.advance(milliseconds(1));  // 200 ms: first poll — still playing
    pb.tick_switch_timeout();
    CHECK(t.calls.size() == 1);
    CHECK(t.playing_queries == 1);
    CHECK(pb.switch_pending());

    t.advance(milliseconds(49));  // the 50 ms re-poll is not due yet
    pb.tick_switch_timeout();
    CHECK(t.playing_queries == 1);

    t.advance(milliseconds(1));  // 250 ms: stopped -> warning check -> load
    pb.tick_switch_timeout();
    CHECK(t.calls == std::vector<std::string>{"stop", "load Movies 0 " + kDir});
    CHECK(t.load_at_ms == std::vector<long long>{250});
    // The old loop's queries exactly: poll (true), poll (false), warn check.
    CHECK(t.playing_queries == 3);
    CHECK_FALSE(pb.switch_pending());
    CHECK(s.is_switching_playlist);  // released later, as before
    CHECK(s.current_playlist_index == 2);
    CHECK_FALSE(s.master_shuffle_active);
    CHECK_FALSE(s.ui_visible_when_playing);

    // Nothing more happens on later frames (until the 2 s timeout).
    run_frames(pb, t, milliseconds(500));
    CHECK(t.calls.size() == 2);
}

TEST_CASE("a mid-playback switch completes when the NEW stream plays — no 2 s timeout",
          "[playlist_playback][switch]") {
    using std::chrono::milliseconds;
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 0);
    s.ui_visible_when_playing = true;
    s.selected_index = 2;
    FakeTransport t;
    t.playing_answers = {false, false};  // stopped at the first poll
    app::PlaylistPlayback pb(s, t, kDir);

    pb.on_select();
    run_frames(pb, t, milliseconds(250));
    REQUIRE(t.calls == std::vector<std::string>{"stop", "load Movies 0 " + kDir});
    CHECK(s.is_switching_playlist);

    SECTION("same stream still (old video's generation): flag held") {
        t.playing_default = true;
        s.set_duration(42.0);
        run_frames(pb, t, milliseconds(100));
        CHECK(s.is_switching_playlist);
    }
    SECTION("new stream loaded but not playing / no duration yet: flag held") {
        t.generation += 1;
        run_frames(pb, t, milliseconds(100));
        CHECK(s.is_switching_playlist);
        t.playing_default = true;
        run_frames(pb, t, milliseconds(100));
        CHECK(s.is_switching_playlist);  // duration still 0
    }
    SECTION("new stream playing with a duration: released at once, no CRITICAL") {
        StreamCapture err(std::cerr);
        t.generation += 1;
        t.playing_default = true;
        s.set_duration(42.0);
        pb.tick_switch_timeout();
        CHECK_FALSE(s.is_switching_playlist);
        run_frames(pb, t, milliseconds(3000));
        CHECK(err.count("CRITICAL: Playlist switch timeout") == 0);
        CHECK(t.calls.size() == 2);  // no recovery stop either
        // NEXT works again immediately (it is ignored while switching).
        pb.on_next();
        CHECK(t.calls.back() == "next_item");
    }
}

TEST_CASE("a pipeline that never stops is re-polled 10 x 50 ms, then loaded anyway",
          "[playlist_playback][switch]") {
    using std::chrono::milliseconds;
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 0);
    s.ui_visible_when_playing = true;
    s.selected_index = 2;
    FakeTransport t;
    t.playing_default = true;
    app::PlaylistPlayback pb(s, t, kDir);
    StreamCapture err(std::cerr);
    pb.on_select();
    run_frames(pb, t, milliseconds(1000), milliseconds(1));
    CHECK(t.calls == std::vector<std::string>{"stop", "load Movies 0 " + kDir});
    CHECK(t.load_at_ms == std::vector<long long>{700});  // 200 + 10 x 50
    // 11 loop checks (10 re-polls, then the bound) + the warning check.
    CHECK(t.playing_queries == 12);
    CHECK(err.count("did not stop cleanly after 500ms") == 1);
}

TEST_CASE("at frame rate the load lands on the first frame after the settle",
          "[playlist_playback][switch]") {
    using std::chrono::milliseconds;
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 0);
    s.ui_visible_when_playing = true;
    s.selected_index = 2;
    FakeTransport t;  // stopped as soon as asked
    app::PlaylistPlayback pb(s, t, kDir);
    pb.on_select();
    run_frames(pb, t, milliseconds(400));  // 16 ms frames
    REQUIRE(t.load_at_ms.size() == 1);
    CHECK(t.load_at_ms[0] >= 200);
    CHECK(t.load_at_ms[0] < 216);
    CHECK(t.playing_queries == 2);  // one poll + the warning check
}

TEST_CASE("switching to Master Shuffle mid-playback picks a random video",
          "[playlist_playback][switch]") {
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 0);
    s.ui_visible_when_playing = true;
    s.selected_index = 0;
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);
    pb.on_select();
    CHECK(t.calls == std::vector<std::string>{"stop"});
    CHECK_FALSE(s.master_shuffle_active);  // set with the load, as before
    run_frames(pb, t, std::chrono::milliseconds(300));
    CHECK(t.calls == std::vector<std::string>{"stop", "random_global"});
    CHECK(s.master_shuffle_active);
    CHECK_FALSE(s.ui_visible_when_playing);
    CHECK(s.is_switching_playlist);
}

TEST_CASE("a failed mid-playback switch lands on a drawable menu",
          "[playlist_playback][switch]") {
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 0);
    s.ui_visible_when_playing = true;
    FakeTransport t;

    SECTION("load error") {
        s.selected_index = 2;
        t.load_ok = false;
        app::PlaylistPlayback pb(s, t, kDir);
        pb.on_select();
        run_frames(pb, t, std::chrono::milliseconds(300));
        CHECK(s.error_message == "Could not load: Movies");
    }
    SECTION("game-only playlist") {
        s.selected_index = 3;
        app::PlaylistPlayback pb(s, t, kDir);
        pb.on_select();
        run_frames(pb, t, std::chrono::milliseconds(300));
        CHECK(s.error_message == "Use Settings to launch games");
    }
    SECTION("empty playlist") {
        s.selected_index = 4;
        app::PlaylistPlayback pb(s, t, kDir);
        pb.on_select();
        run_frames(pb, t, std::chrono::milliseconds(300));
        CHECK(s.error_message == "No content in playlist");
    }
    // stop_to_menu: indexes cleared (the renderer's blank-menu early-out),
    // switch flag released for a retry.
    CHECK(t.calls.front() == "stop");
    CHECK(s.current_playlist_index == -1);
    CHECK(s.current_item_index == -1);
    CHECK_FALSE(s.is_switching_playlist);
    CHECK_FALSE(s.video_active);
}

// ── Re-entrancy while a switch is pending ────────────────────────────────

TEST_CASE("inputs during a pending switch are dropped; the SELECTED playlist loads",
          "[playlist_playback][switch]") {
    using std::chrono::milliseconds;
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 0);
    s.ui_visible_when_playing = true;
    s.selected_index = 2;
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);
    pb.on_select();
    t.advance(milliseconds(50));
    pb.tick_switch_timeout();

    // The cursor moves on (rotary), then every playback input arrives.
    s.selected_index = 1;
    pb.on_select();      // would have been "same playlist? no -> switch again"
    s.selected_index = 2;
    pb.on_select();      // would have been "same playlist -> fade the menu out"
    pb.on_next();
    pb.on_prev();
    pb.on_play_pause();  // would toggle the STOPPED pipeline
    CHECK(t.calls == std::vector<std::string>{"stop"});
    CHECK(s.ui_visible_when_playing);
    CHECK_FALSE(s.is_fading);

    s.selected_index = 3;  // cursor parked elsewhere when the load runs
    run_frames(pb, t, milliseconds(300));
    CHECK(t.calls == std::vector<std::string>{"stop", "load Movies 0 " + kDir});
    CHECK(s.current_playlist_index == 2);

    // Once loaded, input works again.
    pb.on_play_pause();
    CHECK(t.calls.back() == "toggle_pause");
}

TEST_CASE("auto-advance never acts on the stopped stream of a pending switch",
          "[playlist_playback][switch]") {
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 0);
    s.ui_visible_when_playing = true;
    s.playback_started_ = true;
    s.update_playback_state(99.9, 100.0);  // the old item was at its end
    s.selected_index = 2;
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);
    pb.on_select();
    for (int i = 0; i < 10; ++i) pb.tick_auto_advance();
    CHECK(t.calls == std::vector<std::string>{"stop"});
}

TEST_CASE("Media Browser entry abandons a pending switch",
          "[playlist_playback][switch]") {
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 0);
    s.ui_visible_when_playing = true;
    s.selected_index = 2;
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);
    pb.on_select();
    app::reset_main_ui_for_media_browser(s);  // what MB entry does
    run_frames(pb, t, std::chrono::milliseconds(3000));
    CHECK(t.calls == std::vector<std::string>{"stop"});  // never loaded
    CHECK_FALSE(pb.switch_pending());
    CHECK_FALSE(s.is_switching_playlist);
    CHECK(s.current_playlist_index == -1);
}

TEST_CASE("a game session abandons a pending switch; the timeout recovers as before",
          "[playlist_playback][switch]") {
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 0);
    s.ui_visible_when_playing = true;
    s.selected_index = 2;
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);
    pb.on_select();
    // A game launched inside the wait; the session blocked the main loop
    // and its return reset the kiosk (prepare_kiosk_state_after_game).
    t.advance(std::chrono::seconds(30));
    s.current_playlist_index = -1;
    s.current_item_index = -1;
    s.video_active = false;
    pb.tick_switch_timeout();
    // No load into the returned-from-game kiosk; the stuck-switch timeout
    // releases the flag with its recovery stop(), exactly as it did when
    // the old switch's flag outlived a game.
    CHECK(t.calls == std::vector<std::string>{"stop", "stop"});
    CHECK_FALSE(pb.switch_pending());
    CHECK_FALSE(s.is_switching_playlist);
}

TEST_CASE("the stuck-switch timeout never cuts a pending switch short",
          "[playlist_playback][switch]") {
    using std::chrono::milliseconds;
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 0);
    s.ui_visible_when_playing = true;
    s.selected_index = 2;
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);
    pb.on_select();
    // As if stop() itself had blocked for 3 s: the switch is already
    // "older" than the timeout while its own settle is still running.
    s.playlist_switch_start_time = t.clock - std::chrono::seconds(3);
    t.advance(milliseconds(100));
    pb.tick_switch_timeout();
    CHECK(s.is_switching_playlist);
    CHECK(t.calls == std::vector<std::string>{"stop"});

    // Due: the load runs first, then the timeout check in the same tick
    // (the old order) — the old video was still flagged active, so only
    // the flag clears.
    t.advance(milliseconds(100));
    pb.tick_switch_timeout();
    CHECK(t.calls == std::vector<std::string>{"stop", "load Movies 0 " + kDir});
    CHECK_FALSE(s.is_switching_playlist);
}

TEST_CASE("SELECT from the stopped menu starts the playlist", "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    s.selected_index = 1;
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);
    pb.on_select();
    CHECK(t.calls == std::vector<std::string>{"load Cartoons 0 " + kDir});
    CHECK(s.is_switching_playlist);
    CHECK(s.current_playlist_index == 1);
    CHECK(s.current_item_index == 0);
    CHECK_FALSE(s.ui_visible_when_playing);
}

TEST_CASE("SELECT from the stopped menu on row 0 starts Master Shuffle",
          "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    s.selected_index = 0;
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);
    pb.on_select();
    CHECK(t.calls == std::vector<std::string>{"random_global"});
    CHECK(s.master_shuffle_active);
    CHECK(s.is_switching_playlist);
}

TEST_CASE("a failed start from the stopped menu releases the switch flag",
          "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    s.selected_index = 2;
    FakeTransport t;
    t.load_ok = false;
    app::PlaylistPlayback pb(s, t, kDir);
    pb.on_select();
    CHECK(s.error_message == "Could not load: Movies");
    CHECK_FALSE(s.is_switching_playlist);
    CHECK(s.current_playlist_index == -1);
}

TEST_CASE("SELECT is ignored while a switch is already in flight",
          "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    s.selected_index = 1;
    s.is_switching_playlist = true;
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);
    pb.on_select();
    CHECK(t.calls.empty());

    put_playing(s, 1, 0);
    s.ui_visible_when_playing = true;
    s.selected_index = 2;
    pb.on_select();
    CHECK(t.calls.empty());
}

// ── NEXT / PREV / play-pause ─────────────────────────────────────────────

TEST_CASE("NEXT and PREV step the playlist or Master Shuffle history",
          "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 1);
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);

    pb.on_next();
    pb.on_prev();
    s.master_shuffle_active = true;
    pb.on_next();
    pb.on_prev();
    CHECK(t.calls == std::vector<std::string>{
                         "next_item", "prev_item", "shuffle_advance", "shuffle_back"});
}

TEST_CASE("NEXT and PREV seek when no playlist owns the video",
          "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    s.video_active = true;  // e.g. Media Browser playback: no playlist index
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);
    pb.on_next();
    pb.on_prev();
    CHECK(t.calls == std::vector<std::string>{"seek 10", "seek -10"});
}

TEST_CASE("NEXT and PREV do nothing mid-switch or without video",
          "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);
    pb.on_next();
    pb.on_prev();
    put_playing(s, 1, 0);
    s.is_switching_playlist = true;
    pb.on_next();
    pb.on_prev();
    CHECK(t.calls.empty());
}

TEST_CASE("play/pause needs a finished intro and an active video",
          "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);
    pb.on_play_pause();
    s.video_active = true;
    s.intro_complete = false;
    pb.on_play_pause();
    CHECK(t.calls.empty());
    s.intro_complete = true;
    pb.on_play_pause();
    CHECK(t.calls == std::vector<std::string>{"toggle_pause"});
}

// ── Switch timeout ───────────────────────────────────────────────────────

TEST_CASE("a switch stuck for over 2 s is released", "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    s.is_switching_playlist = true;
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);

    SECTION("young switch is left alone") {
        s.playlist_switch_start_time = t.clock;
        t.advance(std::chrono::milliseconds(2000));  // not OVER 2 s yet
        pb.tick_switch_timeout();
        CHECK(s.is_switching_playlist);
        CHECK(t.calls.empty());
    }
    SECTION("stuck with nothing playing: stop, then a 200 ms settle") {
        s.playlist_switch_start_time = t.clock - std::chrono::seconds(3);
        pb.tick_switch_timeout();
        CHECK_FALSE(s.is_switching_playlist);
        // The stop, and no sleep: the call returned at once.
        CHECK(t.calls == std::vector<std::string>{"stop"});

        // The old 200 ms settle sleep is now a window in which SELECT
        // cannot start a load right behind the recovery stop.
        s.selected_index = 1;
        pb.on_select();
        t.advance(std::chrono::milliseconds(199));
        pb.on_select();
        CHECK(t.calls == std::vector<std::string>{"stop"});
        t.advance(std::chrono::milliseconds(1));
        pb.on_select();
        CHECK(t.calls == std::vector<std::string>{"stop", "load Cartoons 0 " + kDir});
    }
    SECTION("stuck but playing: only the flag clears") {
        s.playlist_switch_start_time = t.clock - std::chrono::seconds(3);
        t.playing_default = true;
        pb.tick_switch_timeout();
        CHECK_FALSE(s.is_switching_playlist);
        CHECK(t.calls.empty());
        // No settle window either: SELECT works at once.
        s.selected_index = 1;
        pb.on_select();
        CHECK(t.calls == std::vector<std::string>{"load Cartoons 0 " + kDir});
    }
}

// ── Failed items ─────────────────────────────────────────────────────────

TEST_CASE("only a main-menu playlist item owns the pipeline", "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);
    CHECK_FALSE(pb.owns_pipeline());
    put_playing(s, 1, 0);
    CHECK(pb.owns_pipeline());
    s.is_switching_playlist = true;
    CHECK_FALSE(pb.owns_pipeline());
    s.is_switching_playlist = false;
    s.is_loading_game = true;
    CHECK_FALSE(pb.owns_pipeline());
    s.is_loading_game = false;
    s.showing_intro_video = true;
    CHECK_FALSE(pb.owns_pipeline());
    s.showing_intro_video = false;
#ifdef MEDIA_BROWSER_ENABLED
    s.current_screen = app::AppScreen::MediaBrowser;
    CHECK_FALSE(pb.owns_pipeline());
#endif
}

TEST_CASE("a pipeline error skips the item exactly once per stream",
          "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 0);  // 3-item playlist: budget 3
    FakeTransport t;
    t.error = true;
    app::PlaylistPlayback pb(s, t, kDir);
    pb.tick_pipeline_error();
    pb.tick_pipeline_error();  // same generation: already handled
    CHECK(t.calls == std::vector<std::string>{"next_item"});

    s.master_shuffle_active = true;
    t.generation = 2;
    pb.tick_pipeline_error();
    CHECK(t.calls.back() == "shuffle_advance");
}

TEST_CASE("a single-item playlist that errors gives up to the menu",
          "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    put_playing(s, 2, 0);  // "Movies": budget 1
    FakeTransport t;
    t.error = true;
    app::PlaylistPlayback pb(s, t, kDir);
    CHECK(pb.failure_budget() == 1);
    pb.tick_pipeline_error();
    CHECK(t.calls == std::vector<std::string>{"stop"});
    CHECK(s.error_message == "Couldn't play these videos");
    CHECK(s.current_playlist_index == -1);
    CHECK_FALSE(s.video_active);
}

TEST_CASE("pipeline errors outside a playlist are not the playlist's to handle",
          "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    s.video_active = true;
    FakeTransport t;
    t.error = true;
    app::PlaylistPlayback pb(s, t, kDir);
    pb.tick_pipeline_error();
    CHECK(t.calls.empty());
}

TEST_CASE("a load-next hand-off is skipped on next frame, once per hand-off",
          "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 0);  // 3 items: budget 3
    FakeTransport t;       // no pipeline error, same generation throughout
    app::PlaylistPlayback pb(s, t, kDir);

    s.playlist_item_load_failed = true;
    pb.tick_pipeline_error();
    CHECK_FALSE(s.playlist_item_load_failed);   // consumed
    pb.tick_pipeline_error();                    // nothing new: no action
    CHECK(t.calls == std::vector<std::string>{"next_item"});

    // Each hand-off counts, though no stream generation changed.
    s.playlist_item_load_failed = true;
    pb.tick_pipeline_error();
    CHECK(t.calls.back() == "next_item");
    s.playlist_item_load_failed = true;
    pb.tick_pipeline_error();
    CHECK(t.calls.back() == "stop");             // third in a row: give up
    CHECK(s.error_message == "Couldn't play these videos");
}

TEST_CASE("a load-next hand-off nobody owns any more is dropped",
          "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 0);
    s.is_switching_playlist = true;   // e.g. a playlist switch took over
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);

    s.playlist_item_load_failed = true;
    pb.tick_pipeline_error();
    CHECK_FALSE(s.playlist_item_load_failed);
    s.is_switching_playlist = false;
    pb.tick_pipeline_error();
    CHECK(t.calls.empty());
}

// ── Auto-advance ─────────────────────────────────────────────────────────

TEST_CASE("auto-advance at the item's end loads the next item once",
          "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 0);
    s.playback_started_ = true;
    s.update_playback_state(99.8, 100.0);
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);

    pb.tick_auto_advance();
    CHECK(t.calls == std::vector<std::string>{"next_item"});
    CHECK(s.last_advanced_item_index == 0);
    CHECK(s.last_advanced_duration == 100.0);

    // Next frame, same item still reported (the load has not landed): held.
    pb.tick_auto_advance();
    CHECK(t.calls.size() == 1);

    SECTION("well before the end the guard re-arms") {
        s.update_playback_state(10.0, 100.0);
        pb.tick_auto_advance();
        CHECK(s.last_advanced_item_index == -1);
        CHECK(s.last_advanced_duration == 0.0);
    }
}

TEST_CASE("auto-advance honors the item's end trim and Master Shuffle",
          "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    s.playlists[1].items[0].end = 30.0;
    put_playing(s, 1, 0);
    s.playback_started_ = true;
    s.master_shuffle_active = true;
    s.update_playback_state(29.9, 100.0);
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);
    pb.tick_auto_advance();
    CHECK(t.calls == std::vector<std::string>{"shuffle_advance"});
}

// ── Stall watchdog ───────────────────────────────────────────────────────

TEST_CASE("a frozen playlist video is restarted, then skipped",
          "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 0);
    s.update_playback_state(5.0, 100.0);
    FakeTransport t;
    t.playing_default = true;
    app::PlaylistPlayback pb(s, t, kDir);

    double now = 1000.0;
    int plays = 0;
    // Position never moves. Restart attempts come first; after
    // kMaxRecoveriesBeforeAdvance of them the item is skipped.
    for (int i = 0; i < 200; ++i) {
        pb.tick_stall_watchdog(now);
        now += 1.0;
        if (!t.calls.empty() && t.calls.back() == "next_item") break;
    }
    for (const auto& c : t.calls) plays += (c == "play");
    CHECK(plays == app::PlaybackStallWatchdog::kMaxRecoveriesBeforeAdvance);
    REQUIRE_FALSE(t.calls.empty());
    CHECK(t.calls.back() == "next_item");
}

TEST_CASE("a frozen non-playlist video is only ever restarted",
          "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    s.video_active = true;  // Media Browser-style playback: no playlist owns it
    s.update_playback_state(5.0, 100.0);
    FakeTransport t;
    t.playing_default = true;
    app::PlaylistPlayback pb(s, t, kDir);
    double now = 1000.0;
    for (int i = 0; i < 120; ++i) {
        pb.tick_stall_watchdog(now);
        now += 1.0;
    }
    REQUIRE_FALSE(t.calls.empty());
    for (const auto& c : t.calls) CHECK(c == "play");
}

TEST_CASE("a paused video is never treated as stalled", "[playlist_playback]") {
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 0);
    s.update_playback_state(5.0, 100.0);
    FakeTransport t;
    t.playing_default = true;
    t.paused = true;
    app::PlaylistPlayback pb(s, t, kDir);
    for (int i = 0; i < 60; ++i) pb.tick_stall_watchdog(1000.0 + i);
    CHECK(t.calls.empty());
}

// ── Log volume ───────────────────────────────────────────────────────────

namespace {

struct CoutCapture : StreamCapture {
    CoutCapture() : StreamCapture(std::cout) {}
};

}  // namespace

TEST_CASE("a hold at the item's end is logged once, not once per frame",
          "[playlist_playback][log]") {
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 0);
    s.playback_started_ = true;
    s.last_advanced_item_index = 0;  // this item already advanced
    s.update_playback_state(99.8, 100.0);
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);

    CoutCapture cap;
    for (int i = 0; i < 120; ++i) pb.tick_auto_advance();  // two seconds of frames
    CHECK(t.calls.empty());
    CHECK(cap.count("NOT auto-advancing") == 1);

    SECTION("a change of reason logs again, once") {
        s.playback_started_ = false;
        for (int i = 0; i < 60; ++i) pb.tick_auto_advance();
        CHECK(cap.count("NOT auto-advancing") == 2);
        CHECK(cap.count("playback_started=0") == 1);
    }
    SECTION("leaving the hold re-arms the log for the next one") {
        s.update_playback_state(10.0, 100.0);  // seek back: ResetGuard
        pb.tick_auto_advance();
        s.update_playback_state(99.8, 100.0);
        s.last_advanced_item_index = 0;
        for (int i = 0; i < 60; ++i) pb.tick_auto_advance();
        CHECK(cap.count("NOT auto-advancing") == 2);
    }
    SECTION("a frame with no active video re-arms it too") {
        s.video_active = false;
        pb.tick_auto_advance();
        s.video_active = true;
        for (int i = 0; i < 60; ++i) pb.tick_auto_advance();
        CHECK(cap.count("NOT auto-advancing") == 2);
    }
}

TEST_CASE("Master Shuffle holds are silent", "[playlist_playback][log]") {
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 0);
    s.master_shuffle_active = true;
    s.playback_started_ = false;  // Held in shuffle: playback not confirmed
    s.update_playback_state(99.8, 100.0);
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);
    CoutCapture cap;
    for (int i = 0; i < 60; ++i) pb.tick_auto_advance();
    CHECK(cap.count("NOT auto-advancing") == 0);
    CHECK(t.calls.empty());
}

// ── Channel Guide style: live channels ───────────────────────────────────
// Every video playlist is a channel on a wall-clock schedule
// (app/channel_schedule.h). A tune reuses the switch above (stop, settle,
// re-poll, load) and joins whatever is on air when the load RUNS; NEXT/PREV
// step channels; an item's end rejoins the schedule.

namespace {

// Known lengths for put_menu()'s videos: channel 01 (Cartoons) is a 100 s,
// b 200 s, c 300 s — a 600 s cycle; 02 (Movies) is m, 50 s; 03 (games only)
// and 04 (empty) have no schedule; 00 mixes all four videos.
app::MediaInfoMap guide_lengths() {
    app::MediaInfoMap info;
    info["a.mp4"].duration_s = 100.0;
    info["b.mp4"].duration_s = 200.0;
    info["c.mp4"].duration_s = 300.0;
    info["m.mp4"].duration_s = 50.0;
    return info;
}

struct Guide {
    app::AppState s;
    FakeTransport t;
    std::vector<app::Channel> channels;
    app::PlaylistPlayback pb{s, t, kDir};

    Guide() {
        put_menu(s);
        channels = app::build_channels(s.playlists, guide_lengths());
        pb.set_channels(&channels);
    }
    // The tuned stream is up: what Controller::update_state and the switch
    // completion do on the frames after a load.
    void playing() {
        s.video_active = true;
        s.is_switching_playlist = false;
        s.playback_started_ = true;
    }
};

const auto kSettled = std::chrono::milliseconds(300);

}  // namespace

TEST_CASE("a tune joins the on-air item after the settle, at the offset when the load runs",
          "[playlist_playback][guide]") {
    using std::chrono::milliseconds;
    Guide g;
    put_playing(g.s, 2, 0);
    g.s.ui_visible_when_playing = true;  // the guide is up
    g.t.wall = kW0 + 130;                // 01: b (100-300 s), 30 s in

    g.pb.tune(1, /*keep_overlay=*/false);
    CHECK(g.t.calls == std::vector<std::string>{"stop"});
    CHECK(g.pb.switch_pending());
    CHECK(g.s.is_switching_playlist);
    CHECK(g.s.tuned_channel == -1);  // set when the load runs

    g.t.advance(milliseconds(199));
    g.pb.tick_switch_timeout();
    CHECK(g.t.calls.size() == 1);
    CHECK(g.t.offsets.empty());

    g.t.wall += 20.0;  // the wall clock moves on while the switch waits
    g.t.advance(milliseconds(1));
    g.pb.tick_switch_timeout();
    CHECK(g.t.calls == std::vector<std::string>{"stop", "load_at Cartoons 1"});
    CHECK(g.t.load_at_ms == std::vector<long long>{200});
    REQUIRE(g.t.offsets.size() == 1);
    // 30 s in at the press, plus the 20.2 s that passed before the load.
    CHECK(g.t.last_offset() == Approx(50.2));
    CHECK(g.s.tuned_channel == 1);
    CHECK(g.s.current_playlist_index == 1);
    CHECK(g.s.current_item_index == 1);
    CHECK(g.s.last_channel_id == "path:/p/cartoons.yaml");
    CHECK_FALSE(g.s.master_shuffle_active);
    CHECK_FALSE(g.s.ui_visible_when_playing);  // a tune closes the guide
    CHECK(g.s.is_switching_playlist);          // released when the stream plays
    CHECK_FALSE(g.pb.switch_pending());
}

TEST_CASE("NEXT and PREV change channel live and leave the guide as it is",
          "[playlist_playback][guide]") {
    Guide g;
    put_playing(g.s, 1, 0);
    g.s.tuned_channel = 1;
    g.t.wall = kW0 + 10;

    SECTION("guide up: channel up tunes 02 and the guide stays up") {
        g.s.ui_visible_when_playing = true;
        g.pb.on_next();
        CHECK(g.t.calls == std::vector<std::string>{"stop"});  // the switch's stop
        run_frames(g.pb, g.t, kSettled);
        CHECK(g.t.calls == std::vector<std::string>{"stop", "load_at Movies 0"});
        CHECK(g.t.last_offset() == Approx(10.2).margin(0.05));
        CHECK(g.s.tuned_channel == 2);
        CHECK(g.s.ui_visible_when_playing);
    }
    SECTION("guide hidden: channel up keeps it hidden") {
        g.pb.on_next();
        run_frames(g.pb, g.t, kSettled);
        CHECK(g.s.tuned_channel == 2);
        CHECK_FALSE(g.s.ui_visible_when_playing);
    }
    SECTION("channel down from 01 is the 00 mix") {
        g.pb.on_prev();
        run_frames(g.pb, g.t, kSettled);
        CHECK(g.s.tuned_channel == 0);
    }
}

TEST_CASE("channel up/down wraps and skips channels with no schedule",
          "[playlist_playback][guide]") {
    Guide g;
    REQUIRE(g.channels[3].items.empty());  // games only
    REQUIRE(g.channels[4].items.empty());  // no items
    put_playing(g.s, 2, 0);

    SECTION("up from 02 passes 03 and 04 and wraps to 00") {
        g.s.tuned_channel = 2;
        g.pb.on_next();
        run_frames(g.pb, g.t, kSettled);
        CHECK(g.s.tuned_channel == 0);
    }
    SECTION("down from 00 wraps past 04 and 03 to 02") {
        g.s.tuned_channel = 0;
        g.pb.on_prev();
        run_frames(g.pb, g.t, kSettled);
        CHECK(g.s.tuned_channel == 2);
        CHECK(g.t.last() == "load_at Movies 0");
    }
    SECTION("nothing tuned: up starts at 00, down at the last channel with a schedule") {
        g.pb.on_next();
        run_frames(g.pb, g.t, kSettled);
        CHECK(g.s.tuned_channel == 0);
        g.playing();
        g.s.tuned_channel = -1;
        g.pb.on_prev();
        run_frames(g.pb, g.t, kSettled);
        CHECK(g.s.tuned_channel == 2);
    }
    SECTION("a channel with no schedule cannot be tuned directly") {
        g.pb.tune(3, false);
        g.pb.tune(4, false);
        g.pb.tune(5, false);
        g.pb.tune(-1, false);
        CHECK(g.t.calls.empty());
        CHECK_FALSE(g.s.is_switching_playlist);
    }
}

TEST_CASE("channel changes while a tune is pending are dropped",
          "[playlist_playback][guide]") {
    Guide g;
    put_playing(g.s, 1, 0);
    g.s.tuned_channel = 1;
    g.pb.on_next();  // to 02
    g.pb.on_next();
    g.pb.on_prev();
    g.pb.tune(0, false);
    g.pb.on_play_pause();
    CHECK(g.t.calls == std::vector<std::string>{"stop"});
    run_frames(g.pb, g.t, kSettled);
    CHECK(g.t.calls == std::vector<std::string>{"stop", "load_at Movies 0"});
    CHECK(g.s.tuned_channel == 2);
}

TEST_CASE("tuning 00 joins the mix: tuned channel 00, the source playlist plays",
          "[playlist_playback][guide]") {
    Guide g;
    // 383 s into the mix's 650 s cycle: every item boundary in it is a
    // multiple of 50 s, so this is well inside an item.
    g.t.wall = kW0 + 333;
    g.pb.tune(0, false);
    run_frames(g.pb, g.t, kSettled);
    REQUIRE(g.t.load_wall.size() == 1);
    const auto oa = app::on_air(g.channels[0], g.t.load_wall[0]);
    REQUIRE(oa);
    const app::ScheduledItem& want = g.channels[0].items[oa->index];

    CHECK(g.s.tuned_channel == 0);
    CHECK(g.s.current_playlist_index == want.playlist);
    CHECK(g.s.current_playlist_index >= 1);  // a real playlist, not the virtual row
    CHECK(g.s.current_item_index == want.item);
    CHECK(g.t.last_offset() == Approx(oa->offset_s));
    CHECK(g.s.last_channel_id == "shuffle");
    // The List style's Master Shuffle stepping is not used on 00.
    CHECK_FALSE(g.s.master_shuffle_active);
    CHECK(g.pb.failure_budget() == video::PlaybackErrorPolicy::kMaxFailureBudget);
}

TEST_CASE("at an item's end the channel rejoins its schedule",
          "[playlist_playback][guide]") {
    Guide g;
    g.t.wall = kW0 + 130;  // 01: b, scheduled to end at kW0 + 300
    g.pb.tune(1, false);
    run_frames(g.pb, g.t, kSettled);
    REQUIRE(g.t.last() == "load_at Cartoons 1");
    g.playing();
    g.s.update_playback_state(199.6, 200.0);  // b's end, as decide_auto_advance sees it

    SECTION("on time: the next item, as far in as playback lags") {
        g.t.wall = kW0 + 300.4;
        g.pb.tick_auto_advance();
        CHECK(g.t.last() == "load_at Cartoons 2");
        CHECK(g.t.last_offset() == Approx(0.4));
        CHECK(g.s.current_item_index == 2);
        CHECK(g.s.last_advanced_item_index == 1);
    }
    SECTION("behind schedule (a pause, a seek back): whatever is on now") {
        g.t.wall = kW0 + 300 + 120;
        g.pb.tick_auto_advance();
        CHECK(g.t.last() == "load_at Cartoons 2");
        CHECK(g.t.last_offset() == Approx(120.0));
    }
    SECTION("far behind: whole items are skipped, as on live TV") {
        g.t.wall = kW0 + 600 + 150;  // the next cycle's b, 50 s in
        g.pb.tick_auto_advance();
        CHECK(g.t.last() == "load_at Cartoons 1");
        CHECK(g.t.last_offset() == Approx(50.0));
    }
    SECTION("an end reached a hair early joins the next item from its start") {
        // Never b again with 0.3 s left: it would be inside its own end
        // window with the once-per-item guard already set.
        g.t.wall = kW0 + 299.7;
        g.pb.tick_auto_advance();
        CHECK(g.t.last() == "load_at Cartoons 2");
        CHECK(g.t.last_offset() == 0.0);
    }
    CHECK(g.s.tuned_channel == 1);
    CHECK(g.t.offsets.size() == 2);
    CHECK(g.s.ui_visible_when_playing == false);  // a boundary never opens the guide
}

TEST_CASE("a rebuilt channel list is used at the next boundary",
          "[playlist_playback][guide]") {
    Guide g;
    g.t.wall = kW0 + 130;  // 01: b
    g.pb.tune(1, false);
    run_frames(g.pb, g.t, kSettled);
    g.playing();

    // Mid-item the library changes: c loses its length, so 01 is a, b — a
    // 300 s cycle. The old list goes away entirely.
    auto info = guide_lengths();
    info.erase("c.mp4");
    std::vector<app::Channel> rebuilt = app::build_channels(g.s.playlists, info);
    REQUIRE(rebuilt[1].items.size() == 2);
    g.pb.set_channels(&rebuilt);
    g.channels.clear();
    CHECK(g.s.tuned_channel == 1);

    g.s.update_playback_state(199.6, 200.0);
    g.t.wall = kW0 + 300.5;  // old list: c, 0.5 s in; new list: a, 0.5 s in
    g.pb.tick_auto_advance();
    CHECK(g.t.last() == "load_at Cartoons 0");
    CHECK(g.t.last_offset() == Approx(0.5));
}

TEST_CASE("a reload that renumbers the rows keeps the tuned channel",
          "[playlist_playback][guide]") {
    Guide g;
    g.t.wall = kW0 + 130;
    g.pb.tune(1, false);
    run_frames(g.pb, g.t, kSettled);
    g.playing();

    SECTION("a playlist imported ahead of it: 01 is now 02") {
        app::Playlist news;
        news.title = "News";
        news.path = "/p/news.yaml";
        news.items = {video_item("n")};
        g.s.playlists.insert(g.s.playlists.begin() + 1, news);
        g.s.current_playlist_index = 2;  // apply_reloaded_playlists re-anchors it
        std::vector<app::Channel> rebuilt = app::build_channels(g.s.playlists, guide_lengths());
        g.pb.set_channels(&rebuilt);
        CHECK(g.s.tuned_channel == 2);

        g.s.update_playback_state(199.6, 200.0);
        g.t.wall = kW0 + 300.4;
        g.pb.tick_auto_advance();
        CHECK(g.t.last() == "load_at Cartoons 2");
        CHECK(g.s.tuned_channel == 2);
    }
    SECTION("its playlist deleted: nothing is tuned") {
        g.s.playlists.erase(g.s.playlists.begin() + 1);
        std::vector<app::Channel> rebuilt = app::build_channels(g.s.playlists, guide_lengths());
        g.pb.set_channels(&rebuilt);
        CHECK(g.s.tuned_channel == -1);
    }
}

TEST_CASE("an item that fails in guide style waits for the channel's next item",
          "[playlist_playback][guide]") {
    Guide g;
    g.t.wall = kW0 + 130;  // 01: b, scheduled to end at kW0 + 300

    SECTION("its load fails") {
        g.t.load_ok = false;
        g.pb.tune(1, false);
        run_frames(g.pb, g.t, kSettled);
    }
    SECTION("its pipeline errors mid-item") {
        g.pb.tune(1, false);
        run_frames(g.pb, g.t, kSettled);
        g.playing();
        g.t.error = true;
        g.pb.tick_pipeline_error();
    }
    // Stopped, not skipped ahead to c: the channel is dark until b's slot
    // ends, when the nothing-playing rule re-tunes it.
    CHECK(g.t.calls == std::vector<std::string>{"stop", "load_at Cartoons 1", "stop"});
    CHECK(g.s.guide_wait_until_utc == Approx(kW0 + 300).epsilon(0).margin(1e-6));
    CHECK(g.s.tuned_channel == 1);
    CHECK(g.s.last_channel_id == "path:/p/cartoons.yaml");
    CHECK(g.s.guide_gave_up.empty());
    CHECK(g.s.error_message.empty());
    CHECK(g.s.current_playlist_index == -1);  // stop_to_menu: a drawable menu
    CHECK_FALSE(g.s.video_active);
    CHECK_FALSE(g.s.is_switching_playlist);
    CHECK_FALSE(g.pb.switch_pending());
}

TEST_CASE("a channel that keeps failing is given up: nothing tuned, the channel noted",
          "[playlist_playback][guide]") {
    Guide g;

    SECTION("one item: its first failure is the whole budget") {
        g.t.wall = kW0 + 10;
        g.pb.tune(2, false);
        run_frames(g.pb, g.t, kSettled);
        g.playing();
        CHECK(g.pb.failure_budget() == 1);
        g.t.error = true;
        g.pb.tick_pipeline_error();
        CHECK(g.t.calls == std::vector<std::string>{"stop", "load_at Movies 0", "stop"});
        CHECK(g.s.guide_gave_up == std::vector<int>{2});
        CHECK(g.s.last_channel_id == "path:/p/movies.yaml");
    }
    SECTION("three items: every failure counts, waits included") {
        g.t.load_ok = false;
        g.t.wall = kW0 + 130;
        CHECK(g.channels[1].items.size() == 3);
        for (int i = 0; i < 3; ++i) {
            g.pb.tune(1, false);  // what the nothing-playing rule does
            run_frames(g.pb, g.t, kSettled);
            if (i < 2) {
                REQUIRE(g.s.tuned_channel == 1);
                g.t.wall = g.s.guide_wait_until_utc + 0.5;
            }
        }
        CHECK(g.t.calls == std::vector<std::string>{
                               "stop", "load_at Cartoons 1", "stop",
                               "stop", "load_at Cartoons 2", "stop",
                               "stop", "load_at Cartoons 0", "stop"});
        CHECK(g.s.guide_gave_up == std::vector<int>{1});
    }
    CHECK(g.s.tuned_channel == -1);
    CHECK(g.s.error_message == "Couldn't play these videos");
    CHECK(g.s.current_playlist_index == -1);
    CHECK_FALSE(g.s.video_active);
}

TEST_CASE("List style: no tuning, and NEXT/PREV step the playlist as before",
          "[playlist_playback][guide]") {
    app::AppState s;
    put_menu(s);
    put_playing(s, 1, 1);
    FakeTransport t;
    app::PlaylistPlayback pb(s, t, kDir);
    CHECK_FALSE(pb.guide_style());

    pb.tune(1, false);
    pb.on_channel_step(1);
    CHECK(t.calls.empty());
    pb.on_next();
    pb.on_prev();
    CHECK(t.calls == std::vector<std::string>{"next_item", "prev_item"});
    CHECK(s.tuned_channel == -1);

    SECTION("leaving guide style forgets the tuned channel") {
        std::vector<app::Channel> channels = app::build_channels(s.playlists, guide_lengths());
        pb.set_channels(&channels);
        CHECK(pb.guide_style());
        s.tuned_channel = 1;
        s.guide_gave_up = {2};
        s.guide_wait_until_utc = kW0;
        pb.set_channels(nullptr);
        CHECK_FALSE(pb.guide_style());
        CHECK(s.tuned_channel == -1);
        CHECK(s.guide_gave_up.empty());
        CHECK(s.guide_wait_until_utc == 0.0);
    }
}

// ── Give-up: only a channel that PLAYS earns the given-up channels a retry ─

namespace {

// Tunes `channel` at `wall` and brings its new stream up (a new stream
// generation, as load_file makes), then runs the stream's first frame with
// the player at the position the load asked for: the item's own start trim
// plus its scheduled offset. Returns that position.
double tune_and_play(Guide& g, int channel, double wall) {
    g.t.wall = wall;
    g.pb.tune(channel, false);
    run_frames(g.pb, g.t, kSettled);
    REQUIRE(g.t.last().rfind("load_at ", 0) == 0);
    REQUIRE(g.s.current_playlist_index >= 1);
    g.t.generation += 1;
    g.playing();
    const auto& item = g.s.playlists[g.s.current_playlist_index].items[g.s.current_item_index];
    const double join = item.start + g.t.last_offset();
    g.t.position = join;
    g.pb.tick_pipeline_error();
    return join;
}

}  // namespace

TEST_CASE("channels whose items load but never play are all given up; loads never clear the list",
          "[playlist_playback][guide][give_up]") {
    // Every file loads, plays 1 s past its join point, then errors — the
    // decode failure the give-up exists for. A driver shaped like the
    // nothing-playing rule (the lowest channel with a schedule that is not
    // given up, once any wait is over) must run out of channels.
    Guide g;
    SECTION("untrimmed items") {}
    SECTION("items with a start trim: progress counts from the join point") {
        // A file position past the failure policy's 3 s "healthy" mark from
        // the first frame would end every failure run at once — the channel
        // would never be given up.
        for (auto& pl : g.s.playlists) {
            for (auto& it : pl.items) it.start = 10.0;
        }
        g.channels = app::build_channels(g.s.playlists, guide_lengths());
        g.pb.set_channels(&g.channels);
    }
    const std::vector<int> tunable = {0, 1, 2};
    auto given_up = [&](int c) {
        return std::find(g.s.guide_gave_up.begin(), g.s.guide_gave_up.end(), c) !=
               g.s.guide_gave_up.end();
    };
    size_t seen = 0;
    int loads = 0;
    for (int round = 0; round < 100; ++round) {
        int target = -1;
        for (int c : tunable) {
            if (!given_up(c)) {
                target = c;
                break;
            }
        }
        if (target < 0) break;
        const double join =
            tune_and_play(g, target, std::max(g.t.wall, g.s.guide_wait_until_utc + 0.5));
        ++loads;
        CHECK(g.s.guide_gave_up.size() == seen);  // a successful load clears nothing

        g.t.position = join + 1.0;
        g.pb.tick_pipeline_error();  // 1 s in: neither healthy nor proven
        CHECK(g.s.guide_gave_up.size() == seen);
        g.t.error = true;
        g.pb.tick_pipeline_error();
        g.t.error = false;
        REQUIRE(g.s.guide_gave_up.size() >= seen);  // never cleared
        seen = g.s.guide_gave_up.size();
    }
    std::vector<int> all = g.s.guide_gave_up;
    std::sort(all.begin(), all.end());
    CHECK(all == tunable);
    // Each channel got exactly its budget: the 00 mix 10, 01 three, 02 one.
    CHECK(loads == video::PlaybackErrorPolicy::kMaxFailureBudget + 3 + 1);
    CHECK(g.s.tuned_channel == -1);
}

TEST_CASE("a channel that plays 5 s past its join point clears the give-up list",
          "[playlist_playback][guide][give_up]") {
    Guide g;
    g.s.guide_gave_up = {2};  // 02 was given up earlier

    SECTION("5 s of progress proves it; the load alone does not") {
        const double join = tune_and_play(g, 1, kW0 + 130);  // 01: b, 30 s in
        CHECK(g.s.guide_gave_up == std::vector<int>{2});
        g.t.position = join + 4.9;
        g.pb.tick_pipeline_error();
        CHECK(g.s.guide_gave_up == std::vector<int>{2});
        g.t.position = join + 5.0;
        g.pb.tick_pipeline_error();
        CHECK(g.s.guide_gave_up.empty());
    }
    SECTION("an error before 5 s proves nothing") {
        const double join = tune_and_play(g, 1, kW0 + 130);
        g.t.position = join + 4.0;
        g.pb.tick_pipeline_error();
        g.t.error = true;
        g.pb.tick_pipeline_error();
        CHECK(g.s.guide_gave_up == std::vector<int>{2});
        CHECK(g.s.tuned_channel == 1);  // waiting for its next item (budget 3)
    }
    SECTION("progress counts from the item's start trim plus the offset") {
        for (auto& it : g.s.playlists[1].items) it.start = 20.0;
        g.channels = app::build_channels(g.s.playlists, guide_lengths());
        g.pb.set_channels(&g.channels);
        const double join = tune_and_play(g, 1, kW0 + 130);
        g.t.position = g.t.last_offset() + 5.0;  // 5 s past the offset alone
        g.pb.tick_pipeline_error();
        CHECK(g.s.guide_gave_up == std::vector<int>{2});
        g.t.position = join + 5.0;
        g.pb.tick_pipeline_error();
        CHECK(g.s.guide_gave_up.empty());
    }
}

TEST_CASE("a channel item that reaches its natural end clears the give-up list",
          "[playlist_playback][guide][give_up]") {
    Guide g;
    g.s.guide_gave_up = {2};
    // Joined with 3 s of b left: its natural end comes before 5 s of progress.
    const double join = tune_and_play(g, 1, kW0 + 297);
    g.t.position = join + 1.0;
    g.pb.tick_pipeline_error();
    CHECK(g.s.guide_gave_up == std::vector<int>{2});

    g.s.update_playback_state(199.6, 200.0);
    g.t.wall = kW0 + 300.4;
    g.pb.tick_auto_advance();
    CHECK(g.s.guide_gave_up.empty());
    CHECK(g.t.last() == "load_at Cartoons 2");
}

// ── Review follow-ups ────────────────────────────────────────────────────

TEST_CASE("a tuned item that never starts is a failed item for its channel",
          "[playlist_playback][guide][give_up]") {
    // The load returned OK but no stream ever came up: the 2 s stuck-switch
    // timeout is the only thing that notices. Counted like a failed load,
    // so the nothing-playing rule cannot re-tune a dead channel forever.
    using std::chrono::milliseconds;
    Guide g;
    StreamCapture err(std::cerr);

    SECTION("01: the channel waits for its next item") {
        g.t.wall = kW0 + 130;  // b, scheduled to end at kW0 + 300
        g.pb.tune(1, false);
        run_frames(g.pb, g.t, milliseconds(2100));
        CHECK(g.t.calls == std::vector<std::string>{"stop", "load_at Cartoons 1", "stop"});
        CHECK(g.s.guide_wait_until_utc == Approx(kW0 + 300).epsilon(0).margin(1e-6));
        CHECK(g.s.tuned_channel == 1);
        CHECK(g.s.guide_gave_up.empty());
    }
    SECTION("02, one item: its only failure gives it up") {
        g.t.wall = kW0 + 10;
        g.pb.tune(2, false);
        run_frames(g.pb, g.t, milliseconds(2100));
        CHECK(g.t.calls == std::vector<std::string>{"stop", "load_at Movies 0", "stop"});
        CHECK(g.s.guide_gave_up == std::vector<int>{2});
        CHECK(g.s.tuned_channel == -1);
        CHECK(g.s.error_message == "Couldn't play these videos");
    }
    CHECK(err.count("CRITICAL: Playlist switch timeout") == 1);
    CHECK_FALSE(g.s.is_switching_playlist);
    CHECK(g.s.current_playlist_index == -1);  // a drawable menu
}

TEST_CASE("a tuned stream taken away by a game session is not the channel's failure",
          "[playlist_playback][guide][give_up]") {
    Guide g;
    StreamCapture err(std::cerr);
    g.t.wall = kW0 + 130;
    g.pb.tune(1, false);
    run_frames(g.pb, g.t, kSettled);
    REQUIRE(g.t.last() == "load_at Cartoons 1");
    // A game launched before the stream came up; its return reset the
    // kiosk (prepare_kiosk_state_after_game) and left the switch flag up.
    g.t.advance(std::chrono::seconds(30));
    g.s.current_playlist_index = -1;
    g.s.current_item_index = -1;
    g.pb.tick_switch_timeout();
    // The stuck-switch timeout's own recovery, as for any switch.
    CHECK(err.count("MPV appears stuck") == 1);
    CHECK(g.t.calls == std::vector<std::string>{"stop", "load_at Cartoons 1", "stop"});
    CHECK_FALSE(g.s.is_switching_playlist);
    CHECK(g.s.guide_wait_until_utc == 0.0);  // no failure counted
    CHECK(g.s.guide_gave_up.empty());
}

TEST_CASE("a tuned stream that comes up is not counted when a later switch times out",
          "[playlist_playback][guide][give_up]") {
    using std::chrono::milliseconds;
    Guide g;
    g.t.wall = kW0 + 130;
    g.pb.tune(1, false);
    run_frames(g.pb, g.t, kSettled);
    g.playing();  // update_state saw it start and released the switch flag
    run_frames(g.pb, g.t, milliseconds(3000));
    CHECK(g.t.calls == std::vector<std::string>{"stop", "load_at Cartoons 1"});
    CHECK(g.s.tuned_channel == 1);
    CHECK(g.s.guide_wait_until_utc == 0.0);
}

TEST_CASE("each channel gets its own failure run",
          "[playlist_playback][guide][give_up]") {
    // Three failures on 00 (budget 10), then channel up to 01 (budget 3):
    // 01's first failure is its first, not the run's fourth.
    Guide g;
    g.t.load_ok = false;
    g.t.wall = kW0 + 333;
    for (int i = 0; i < 3; ++i) {
        g.pb.tune(0, false);
        run_frames(g.pb, g.t, kSettled);
        REQUIRE(g.s.tuned_channel == 0);
        g.t.wall = g.s.guide_wait_until_utc + 0.5;
    }
    REQUIRE(g.s.guide_gave_up.empty());

    g.pb.on_next();
    run_frames(g.pb, g.t, kSettled);
    CHECK(g.s.tuned_channel == 1);
    CHECK(g.s.guide_gave_up.empty());
    CHECK(g.s.error_message.empty());
    CHECK(g.s.guide_wait_until_utc > 0.0);  // waiting for 01's next item
}

TEST_CASE("progress counts from the first position the playing stream reports",
          "[playlist_playback][guide][give_up]") {
    // The start seek snaps to a keyframe (KEY_UNIT | SNAP_NEAREST): here
    // the stream comes up 4 s past the offset it was asked for. Those 4 s
    // were never played and must not count toward the 5 s proof.
    Guide g;
    g.s.guide_gave_up = {2};
    g.t.wall = kW0 + 130;
    g.pb.tune(1, false);
    run_frames(g.pb, g.t, kSettled);
    g.t.generation += 1;
    const double asked = g.t.last_offset();  // b has no start trim

    // The switch is released but the stream is not up yet: GstPlayer still
    // reports the requested start. Not an anchor.
    g.s.is_switching_playlist = false;
    g.t.position = asked;
    g.pb.tick_pipeline_error();

    g.playing();
    g.t.position = asked + 4.0;  // the first position once it plays
    g.pb.tick_pipeline_error();
    g.t.position = asked + 5.0;  // 1 s played
    g.pb.tick_pipeline_error();
    CHECK(g.s.guide_gave_up == std::vector<int>{2});
    g.t.position = asked + 8.9;
    g.pb.tick_pipeline_error();
    CHECK(g.s.guide_gave_up == std::vector<int>{2});
    g.t.position = asked + 9.0;  // 5 s played
    g.pb.tick_pipeline_error();
    CHECK(g.s.guide_gave_up.empty());
}

TEST_CASE("guide_gave_up holds each channel once and only rows the list has",
          "[playlist_playback][guide][give_up]") {
    Guide g;

    SECTION("giving up a channel already given up does not add it twice") {
        g.s.guide_gave_up = {2};
        g.t.load_ok = false;
        g.pb.tune(2, false);  // a press on a given-up channel; one item: gives up
        run_frames(g.pb, g.t, kSettled);
        CHECK(g.s.error_message == "Couldn't play these videos");
        CHECK(g.s.guide_gave_up == std::vector<int>{2});
    }
    SECTION("a rebuilt list drops rows past its end") {
        g.s.guide_gave_up = {1, 4};
        g.s.playlists.pop_back();  // the empty playlist (row 4) is deleted
        std::vector<app::Channel> rebuilt = app::build_channels(g.s.playlists, guide_lengths());
        g.pb.set_channels(&rebuilt);
        CHECK(g.s.guide_gave_up == std::vector<int>{1});
    }
}

TEST_CASE("a tune whose load cannot run still lands on its channel",
          "[playlist_playback][guide]") {
    Guide g;
    g.t.wall = kW0 + 10;  // 01: a, 10 s in
    put_playing(g.s, 2, 0);
    g.s.tuned_channel = 2;
    g.s.last_channel_id = "path:/p/movies.yaml";

    SECTION("the channel lost its schedule during the wait") {
        g.pb.tune(1, false);
        auto info = guide_lengths();
        info.erase("a.mp4");
        info.erase("b.mp4");
        info.erase("c.mp4");
        std::vector<app::Channel> rebuilt = app::build_channels(g.s.playlists, info);
        REQUIRE(rebuilt[1].items.empty());
        g.pb.set_channels(&rebuilt);
        run_frames(g.pb, g.t, kSettled);
        CHECK(g.t.calls == std::vector<std::string>{"stop"});
    }
    SECTION("its schedule points past the playlist (a list older than the playlists)") {
        g.channels[1].items[0].item = 99;
        g.pb.tune(1, false);
        run_frames(g.pb, g.t, kSettled);
        CHECK(g.t.calls == std::vector<std::string>{"stop", "stop"});
    }
    CHECK(g.s.tuned_channel == 1);
    CHECK(g.s.last_channel_id == "path:/p/cartoons.yaml");
    CHECK(g.s.current_playlist_index == -1);
    CHECK_FALSE(g.s.is_switching_playlist);
}

TEST_CASE("Media Browser entry abandons a pending tune", "[playlist_playback][guide]") {
    Guide g;
    put_playing(g.s, 1, 0);
    g.s.tuned_channel = 1;
    g.pb.on_next();  // to 02
    app::reset_main_ui_for_media_browser(g.s);
    run_frames(g.pb, g.t, std::chrono::milliseconds(3000));
    CHECK(g.t.calls == std::vector<std::string>{"stop"});  // never loaded
    CHECK_FALSE(g.pb.switch_pending());
    CHECK_FALSE(g.s.is_switching_playlist);
    CHECK(g.s.tuned_channel == 1);  // the tune never ran
    CHECK(g.s.guide_gave_up.empty());
}
