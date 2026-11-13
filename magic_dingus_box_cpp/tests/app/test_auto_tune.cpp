// app::decide_auto_tune — in the Channel Guide style "nothing playing" is
// never a resting state: one rule, evaluated every frame, re-tunes live.
// Design: docs/superpowers/specs/2026-11-12-channel-guide-design.md,
// "When nothing would be playing".

#include <catch2/catch_test_macros.hpp>

#include <optional>

#include "app/auto_tune.h"

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
    REQUIRE(decide_auto_tune(in) == std::optional<int>(2));

    SECTION("List style") { in.guide_style = false; }
    SECTION("the intro is not over") { in.intro_complete = false; }
    SECTION("off the main screen") { in.on_main_screen = false; }
    SECTION("a video plays") { in.video_active = true; }
    SECTION("a switch is pending") { in.switch_pending = true; }
    SECTION("a game is loading") { in.loading_game = true; }
    SECTION("the post-game restore is pending") { in.post_game_pending = true; }
    SECTION("a failed item's slot has not ended") {
        in.wait_until_utc = in.now_utc + 1.0;
    }

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
