// A GStreamer error on a playlist item used to freeze an unattended kiosk
// forever: the errored stream never reached position >= duration (and an
// error before preroll left duration at 0), so the natural-end auto-advance
// never fired, and the stall watchdog only re-called play() on the dead
// pipeline. PlaybackErrorPolicy decides skip vs. give up; these tests pin
// both halves — skip a broken item, and never spin through a playlist in
// which every item is broken.

#include <catch2/catch_test_macros.hpp>

#include "video/playback_error_policy.h"

using video::PlaybackErrorPolicy;
using Decision = video::PlaybackErrorPolicy::Decision;

TEST_CASE("a healthy stream is never disturbed", "[video][error_policy]") {
    PlaybackErrorPolicy p;
    for (int i = 0; i < 100; ++i) {
        REQUIRE(p.on_frame(1, false, i * 0.1, 5) == Decision::None);
    }
}

TEST_CASE("an errored item advances exactly once per stream",
          "[video][error_policy]") {
    PlaybackErrorPolicy p;
    REQUIRE(p.on_frame(7, true, 0.0, 5) == Decision::Advance);
    // The same latched error is seen on every frame until the next load —
    // it must not trigger a second advance.
    for (int i = 0; i < 50; ++i) {
        REQUIRE(p.on_frame(7, true, 0.0, 5) == Decision::None);
    }
    // The next stream erroring is a NEW failure.
    REQUIRE(p.on_frame(8, true, 0.0, 5) == Decision::Advance);
    REQUIRE(p.consecutive_failures() == 2);
}

TEST_CASE("a playlist where every item is broken gives up instead of spinning",
          "[video][error_policy]") {
    PlaybackErrorPolicy p;
    const int budget = PlaybackErrorPolicy::failure_budget(3, false);
    REQUIRE(budget == 3);
    REQUIRE(p.on_frame(1, true, 0.0, budget) == Decision::Advance);
    REQUIRE(p.on_frame(2, true, 0.0, budget) == Decision::Advance);
    // Third consecutive failure = every item tried once: stop, show the UI.
    REQUIRE(p.on_frame(3, true, 0.0, budget) == Decision::GiveUp);
    // Giving up resets the run: a user-initiated retry gets a full budget.
    REQUIRE(p.consecutive_failures() == 0);
    REQUIRE(p.on_frame(4, true, 0.0, budget) == Decision::Advance);
}

TEST_CASE("a single-item playlist gives up at once rather than reloading itself",
          "[video][error_policy]") {
    PlaybackErrorPolicy p;
    REQUIRE(PlaybackErrorPolicy::failure_budget(1, false) == 1);
    REQUIRE(p.on_frame(1, true, 0.0, 1) == Decision::GiveUp);
}

TEST_CASE("an item that plays for a while ends the failure run",
          "[video][error_policy]") {
    PlaybackErrorPolicy p;
    const int budget = 3;
    REQUIRE(p.on_frame(1, true, 0.0, budget) == Decision::Advance);
    REQUIRE(p.on_frame(2, true, 0.0, budget) == Decision::Advance);
    // Item 3 plays fine past the healthy threshold...
    REQUIRE(p.on_frame(3, false, 1.0, budget) == Decision::None);
    REQUIRE(p.consecutive_failures() == 2);
    REQUIRE(p.on_frame(3, false, PlaybackErrorPolicy::kHealthyPlaySec, budget)
            == Decision::None);
    REQUIRE(p.consecutive_failures() == 0);
    // ...so a later, isolated error (truncated tail) is skipped, not fatal.
    REQUIRE(p.on_frame(3, true, 40.0, budget) == Decision::Advance);
    REQUIRE(p.on_frame(4, true, 0.0, budget) == Decision::Advance);
}

TEST_CASE("watchdog stall failures share the run with pipeline errors",
          "[video][error_policy]") {
    PlaybackErrorPolicy p;
    REQUIRE(p.on_failure(1, 2) == Decision::Advance);
    // A stall on the same stream is not counted twice.
    REQUIRE(p.on_failure(1, 2) == Decision::None);
    REQUIRE(p.on_frame(1, true, 0.0, 2) == Decision::None);
    REQUIRE(p.on_frame(2, true, 0.0, 2) == Decision::GiveUp);
}

TEST_CASE("failure budget bounds", "[video][error_policy]") {
    REQUIRE(PlaybackErrorPolicy::failure_budget(0, false) == 1);
    REQUIRE(PlaybackErrorPolicy::failure_budget(4, false) == 4);
    REQUIRE(PlaybackErrorPolicy::failure_budget(500, false) ==
            PlaybackErrorPolicy::kMaxFailureBudget);
    REQUIRE(PlaybackErrorPolicy::failure_budget(1, true) ==
            PlaybackErrorPolicy::kMaxFailureBudget);
}

TEST_CASE("Media Browser: an error ends a live session but never an ended one",
          "[video][error_policy][media_browser]") {
    // Live playback + error -> abort (toast, back to the previous screen).
    REQUIRE(video::mb_should_abort_on_error(true, false, false));
    // No error -> the natural-end detector stays in charge.
    REQUIRE_FALSE(video::mb_should_abort_on_error(false, false, false));
    // Natural EOS already latched (end-of-episode overlay up): ignore.
    REQUIRE_FALSE(video::mb_should_abort_on_error(true, true, false));
    // Exit already armed: nothing more to do.
    REQUIRE_FALSE(video::mb_should_abort_on_error(true, false, true));
}
