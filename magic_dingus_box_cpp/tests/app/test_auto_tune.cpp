// app::decide_auto_tune — in the Channel Guide style "nothing playing" is
// never a resting state: one rule, evaluated every frame, re-tunes live.
// Design: docs/superpowers/specs/2026-11-12-channel-guide-design.md,
// "When nothing would be playing".

#include <catch2/catch_test_macros.hpp>

#include <optional>

#include "app/app_state.h"
#include "app/auto_tune.h"
#include "app/playback_reset.h"

using app::AutoTuneInput;
using app::decide_auto_tune;

namespace {

// Every gate open: guide style, the intro over, on the main screen, nothing
// playing or pending, no wait. Five channels (00-04), all with a schedule;
// the last channel watched is 02.
AutoTuneInput ready() {
    AutoTuneInput in;
    in.guide_style = true;
    in.intro_complete = true;
    in.media_info_read = true;
    in.on_main_screen = true;
    in.video_active = false;
    in.switch_pending = false;
    in.loading_game = false;
    in.post_game_pending = false;
    in.now_utc = 1800000000.0;
    in.wait_until_utc = 0.0;
    in.last_channel = 2;
    in.tunable = {true, true, true, true, true};
    in.gave_up = {};
    return in;
}

constexpr std::optional<int> none = std::nullopt;

}  // namespace

TEST_CASE("auto-tune: each gate alone blocks", "[auto_tune]") {
    AutoTuneInput in = ready();
    REQUIRE(app::auto_tune_gates_open(in));
    REQUIRE(decide_auto_tune(in) == std::optional<int>(2));

    SECTION("List style") { in.guide_style = false; }
    SECTION("the intro is not over") { in.intro_complete = false; }
    SECTION("the media info store's first read has not landed") {
        // No intro (or a failed one): the rule would run before any length
        // but the trimmed items' is known, and tune the wrong channel.
        in.media_info_read = false;
    }
    SECTION("off the main screen") { in.on_main_screen = false; }
    SECTION("a video plays") { in.video_active = true; }
    SECTION("a switch is pending") { in.switch_pending = true; }
    SECTION("a game is loading") { in.loading_game = true; }
    SECTION("the post-game restore is pending") { in.post_game_pending = true; }
    SECTION("a failed item's slot has not ended") {
        in.wait_until_utc = in.now_utc + 1.0;
    }

    CHECK_FALSE(app::auto_tune_gates_open(in));
    CHECK(decide_auto_tune(in) == none);
}

TEST_CASE("auto-tune: the gates read only the flags and the clock", "[auto_tune]") {
    // main.cpp resolves last_channel / tunable / gave_up (an identity scan
    // that builds a string per row) only once the gates are open.
    AutoTuneInput in = ready();
    in.last_channel = -1;
    in.tunable.clear();
    in.gave_up = {0, 1};
    CHECK(app::auto_tune_gates_open(in));
    CHECK(decide_auto_tune(in) == none);
}

TEST_CASE("auto-tune: a failed item's wait, then the same channel", "[auto_tune]") {
    AutoTuneInput in = ready();
    in.wait_until_utc = in.now_utc + 30.0;
    CHECK(decide_auto_tune(in) == none);
    in.now_utc = in.wait_until_utc - 0.5;
    CHECK(decide_auto_tune(in) == none);
    in.now_utc = in.wait_until_utc;
    CHECK(decide_auto_tune(in) == std::optional<int>(2));
    in.now_utc = in.wait_until_utc + 600.0;
    CHECK(decide_auto_tune(in) == std::optional<int>(2));
}

TEST_CASE("auto-tune: the last channel watched, when it has a schedule", "[auto_tune]") {
    AutoTuneInput in = ready();
    in.last_channel = 3;
    CHECK(decide_auto_tune(in) == std::optional<int>(3));
    in.last_channel = 0;  // 00, the Master Shuffle mix
    CHECK(decide_auto_tune(in) == std::optional<int>(0));
}

TEST_CASE("auto-tune: no last channel tunes channel 01", "[auto_tune]") {
    AutoTuneInput in = ready();
    in.last_channel = -1;
    CHECK(decide_auto_tune(in) == std::optional<int>(1));
}

TEST_CASE("auto-tune: a last channel without a schedule falls back to 01",
          "[auto_tune]") {
    AutoTuneInput in = ready();
    in.last_channel = 3;
    in.tunable[3] = false;  // its items lost their lengths
    CHECK(decide_auto_tune(in) == std::optional<int>(1));
    in.last_channel = 9;    // past the list (a reload removed rows)
    CHECK(decide_auto_tune(in) == std::optional<int>(1));
}

TEST_CASE("auto-tune: channel 01 empty tunes the lowest channel with a schedule",
          "[auto_tune]") {
    AutoTuneInput in = ready();
    in.last_channel = -1;
    in.tunable = {true, false, true, true};
    CHECK(decide_auto_tune(in) == std::optional<int>(0));
    in.tunable = {false, false, false, true, true};
    CHECK(decide_auto_tune(in) == std::optional<int>(3));
}

TEST_CASE("auto-tune: after a give-up, the next channel up", "[auto_tune]") {
    AutoTuneInput in = ready();
    in.last_channel = 3;
    in.gave_up = {3};
    CHECK(decide_auto_tune(in) == std::optional<int>(4));
}

TEST_CASE("auto-tune: after a give-up, channels given up or without a schedule are passed",
          "[auto_tune]") {
    AutoTuneInput in = ready();
    in.tunable = {true, true, true, true, false, true, true};
    in.last_channel = 3;
    in.gave_up = {3};
    CHECK(decide_auto_tune(in) == std::optional<int>(5));
    in.tunable[4] = true;
    in.last_channel = 4;
    in.gave_up = {3, 4};
    CHECK(decide_auto_tune(in) == std::optional<int>(5));
}

TEST_CASE("auto-tune: a give-up on the last row wraps to 00", "[auto_tune]") {
    AutoTuneInput in = ready();
    in.last_channel = 4;
    in.gave_up = {4};
    CHECK(decide_auto_tune(in) == std::optional<int>(0));
    in.tunable[0] = false;  // 00 has no schedule: on to 01
    CHECK(decide_auto_tune(in) == std::optional<int>(1));
}

TEST_CASE("auto-tune: a channel given up again is the most recent give-up",
          "[auto_tune]") {
    // guide_gave_up keeps a channel where it was first given up; the last
    // channel watched is the one that just failed.
    AutoTuneInput in = ready();
    in.tunable = std::vector<bool>(10, true);
    in.last_channel = 3;
    in.gave_up = {3, 7};
    CHECK(decide_auto_tune(in) == std::optional<int>(4));
}

TEST_CASE("auto-tune: a give-up with no last channel moves on from the newest",
          "[auto_tune]") {
    AutoTuneInput in = ready();
    in.last_channel = -1;  // its playlist was removed
    in.gave_up = {1, 2};
    CHECK(decide_auto_tune(in) == std::optional<int>(3));
}

TEST_CASE("auto-tune: a channel tuned after a give-up keeps its place", "[auto_tune]") {
    // 03 was given up, 04 was tuned next and its item failed: once the slot
    // ends, 04 comes back — not 05.
    AutoTuneInput in = ready();
    in.last_channel = 4;
    in.gave_up = {3};
    CHECK(decide_auto_tune(in) == std::optional<int>(4));
}

TEST_CASE("auto-tune: every channel given up tunes nothing", "[auto_tune]") {
    AutoTuneInput in = ready();
    in.last_channel = 1;
    in.gave_up = {3, 0, 4, 2, 1};
    CHECK(decide_auto_tune(in) == none);
    // The channels without a schedule never counted.
    in.tunable = {true, false, true, false, true};
    in.gave_up = {0, 4, 2};
    in.last_channel = 2;
    CHECK(decide_auto_tune(in) == none);
}

TEST_CASE("auto-tune: nothing with a schedule tunes nothing", "[auto_tune]") {
    AutoTuneInput in = ready();
    in.last_channel = -1;
    in.tunable = {false, false, false};
    CHECK(decide_auto_tune(in) == none);
    in.last_channel = 1;
    CHECK(decide_auto_tune(in) == none);
    in.tunable.clear();  // no playlists at all
    in.last_channel = -1;
    CHECK(decide_auto_tune(in) == none);
}

TEST_CASE("auto-tune: the last channel's row is resolved only when it can change",
          "[auto_tune]") {
    // Every channel given up: the gates stay open every frame. The identity
    // scan behind the row must not run (and allocate) on each of them.
    app::LastChannelRowCache cache;
    int scans = 0;
    auto resolve = [&scans](const std::string& id) {
        ++scans;
        return id == "path:/p/a.yaml" ? 3 : -1;
    };
    for (int frame = 0; frame < 100; ++frame) {
        CHECK(cache.get("path:/p/a.yaml", 7, resolve) == 3);
    }
    CHECK(scans == 1);
    CHECK(cache.get("path:/p/a.yaml", 8, resolve) == 3);  // a rebuild
    CHECK(scans == 2);
    CHECK(cache.get("path:/p/b.yaml", 8, resolve) == -1);  // a new id
    CHECK(scans == 3);
    CHECK(cache.get("path:/p/b.yaml", 8, resolve) == -1);
    CHECK(scans == 3);
}

// ── An orphan stream ──────────────────────────────────────────────────────
// Pi 5, 2026-11-13 13:02: after a game the player described a torn-down
// stream (duration 40.1 s, position 1.68 s, not playing), so video_active
// came up with no channel playing. The rule above read that as "a video
// plays" and never tuned; no EOS or stall could end it. The guard stops
// such a stream after a short bound so the rule runs.

namespace {

// The 13:02 state: guide style, on the main screen after the intro,
// video_active with no playing indexes and the player not playing, nothing
// pending.
app::OrphanStreamInput orphan_1302() {
    app::OrphanStreamInput in;
    in.guide_style = true;
    in.intro_complete = true;
    in.on_main_screen = true;
    in.video_active = true;
    in.indexes_set = false;
    in.playing = false;
    in.switch_pending = false;
    in.loading_game = false;
    in.post_game_pending = false;
    return in;
}

}  // namespace

TEST_CASE("orphan stream: video_active, no channel playing, not playing", "[auto_tune]") {
    app::OrphanStreamInput in = orphan_1302();
    REQUIRE(app::is_orphan_stream(in));

    SECTION("List style: unchanged, nothing is ever stopped") { in.guide_style = false; }
    SECTION("the intro (no indexes by design) is not over") { in.intro_complete = false; }
    SECTION("off the main screen: Media Browser playback has no indexes either") {
        in.on_main_screen = false;
    }
    SECTION("no video") { in.video_active = false; }
    SECTION("a channel owns it: between items, or paused by the viewer") {
        in.indexes_set = true;
    }
    SECTION("it plays") { in.playing = true; }
    SECTION("a switch or tune has not got its stream up") { in.switch_pending = true; }
    SECTION("a game is loading") { in.loading_game = true; }
    SECTION("the post-game restore is not done") { in.post_game_pending = true; }

    CHECK_FALSE(app::is_orphan_stream(in));
}

TEST_CASE("orphan stream: stopped once it has lasted the bound", "[auto_tune]") {
    using app::OrphanStreamGuard;
    OrphanStreamGuard guard;
    const double t0 = 1000.0;
    const double frame = 1.0 / 64.0;  // a frame; exact in binary

    // Not on the first frame, nor at any frame short of the bound: a real
    // stop's video_active debounce (a few frames) must never trip it.
    CHECK_FALSE(guard.should_stop(true, t0));
    CHECK_FALSE(guard.should_stop(true, t0 + 0.2));
    CHECK_FALSE(guard.should_stop(true, t0 + OrphanStreamGuard::kBoundS - frame));
    // At the bound: once.
    CHECK(guard.should_stop(true, t0 + OrphanStreamGuard::kBoundS));
    // The stop restarts the count: an orphan that survives it gets a whole
    // new bound before the next stop, not one per frame.
    CHECK_FALSE(guard.should_stop(true, t0 + OrphanStreamGuard::kBoundS + frame));
    CHECK_FALSE(guard.should_stop(true, t0 + 2 * OrphanStreamGuard::kBoundS));
    CHECK(guard.should_stop(true, t0 + 2 * OrphanStreamGuard::kBoundS + frame));
}

TEST_CASE("orphan stream: any frame that is not one restarts the count", "[auto_tune]") {
    using app::OrphanStreamGuard;
    OrphanStreamGuard guard;
    const double t0 = 50.0;
    CHECK_FALSE(guard.should_stop(true, t0));
    CHECK_FALSE(guard.should_stop(true, t0 + 1.5));
    CHECK_FALSE(guard.should_stop(false, t0 + 1.625));  // a channel took it over
    CHECK_FALSE(guard.should_stop(true, t0 + 1.75));
    CHECK_FALSE(guard.should_stop(true, t0 + 1.75 + OrphanStreamGuard::kBoundS - 0.25));
    CHECK(guard.should_stop(true, t0 + 1.75 + OrphanStreamGuard::kBoundS));
}

TEST_CASE("orphan stream: the guard's stop lets the nothing-playing rule tune",
          "[auto_tune]") {
    // main.cpp's action: controller.stop(), then app::stop_to_menu. On the
    // same frame the rule's video_active (state.video_active || indexes set)
    // must read false, so it tunes the last channel watched.
    app::AppState state;
    state.video_active = true;
    state.current_playlist_index = -1;
    state.current_item_index = -1;
    state.update_playback_state(1.680522, 40.1);

    app::stop_to_menu(state);

    AutoTuneInput in = ready();
    in.video_active = state.video_active ||
                      (state.current_playlist_index >= 0 && state.current_item_index >= 0);
    CHECK(decide_auto_tune(in) == std::optional<int>(2));
    CHECK(state.get_duration() == 0.0);
    CHECK(state.get_position() == 0.0);
}
