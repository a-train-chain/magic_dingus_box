// PlaylistPlayback: main.cpp's main-menu playlist control, moved out of the
// main loop verbatim. These pin the ORDER of Controller calls (a playlist
// switch must stop, wait, then load), which calls happen at all on each
// branch, and the AppState each branch leaves behind — the things a
// refactor of this code can silently change. Runs against a recording fake
// transport with a hand-advanced clock (no sleeps: the switch's waits are
// per-frame ticks); the kiosk adapter (app/controller_transport.h)
// forwards 1:1.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <deque>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "app/app_state.h"
#include "app/playback_reset.h"
#include "app/playlist_playback.h"

namespace {

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

    void advance(std::chrono::milliseconds d) { clock += d; }
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
    uint64_t stream_generation() const override { return generation; }
    bool has_error() const override { return error; }
    double player_position() const override { return position; }
    std::chrono::steady_clock::time_point now() const override { return clock; }
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
