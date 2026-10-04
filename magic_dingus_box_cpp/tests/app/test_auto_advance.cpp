// Auto-advance decision for playlist items, including the `end:` trim.
//
// The defect: Controller::update_state computed an effective duration from
// the item's `end:` trim but used it only for a local flag; the real
// auto-advance in main.cpp compared position against the FULL file duration.
// So a trimmed item played to the end of its file and the trim did nothing.
// Both sites now ask this one function.

#include <catch2/catch_test_macros.hpp>

#include "app/auto_advance.h"

using app::AutoAdvance;
using app::AutoAdvanceInput;
using app::decide_auto_advance;
using app::effective_end;

namespace {

// A 120 s file, playing normally, nothing advanced yet.
AutoAdvanceInput playing(double position, double item_end = 0.0) {
    AutoAdvanceInput in;
    in.position = position;
    in.duration = 120.0;
    in.item_end = item_end;
    in.playback_started = true;
    in.master_shuffle = false;
    in.current_item = 2;
    in.last_advanced_item = -1;
    return in;
}

}  // namespace

TEST_CASE("effective_end honors an end trim inside the file",
          "[auto_advance][trim]") {
    CHECK(effective_end(120.0, 45.0) == 45.0);
}

TEST_CASE("effective_end falls back to the real duration", "[auto_advance][trim]") {
    // No trim.
    CHECK(effective_end(120.0, 0.0) == 120.0);
    // A trim past the end of the media would never be reached and would hang
    // the item — clamp to the real end instead.
    CHECK(effective_end(120.0, 500.0) == 120.0);
    CHECK(effective_end(120.0, 120.0) == 120.0);
    // Defensive: a negative trim is "unset" (the loader also normalizes it).
    CHECK(effective_end(120.0, -3.0) == 120.0);
}

TEST_CASE("effective_end with an unknown duration keeps it unknown",
          "[auto_advance][trim]") {
    // Duration 0 means "not prerolled yet"; a trim must not invent one, or an
    // item would look ended before its file has even opened.
    CHECK(effective_end(0.0, 45.0) == 0.0);
}

TEST_CASE("an untrimmed item advances at the end of its file", "[auto_advance]") {
    CHECK(decide_auto_advance(playing(119.6)) == AutoAdvance::Advance);
    CHECK(decide_auto_advance(playing(60.0)) == AutoAdvance::ResetGuard);
}

TEST_CASE("a trimmed item advances at its end trim, not the end of the file",
          "[auto_advance][trim]") {
    // The regression: at 45 s of a 120 s file with end: 45, the old check
    // (position >= duration - 0.5) said "keep playing".
    CHECK(decide_auto_advance(playing(44.6, 45.0)) == AutoAdvance::Advance);
    CHECK(decide_auto_advance(playing(50.0, 45.0)) == AutoAdvance::Advance);
    CHECK(decide_auto_advance(playing(30.0, 45.0)) == AutoAdvance::ResetGuard);
}

TEST_CASE("the guard window between end-1 and end-0.5 does nothing",
          "[auto_advance]") {
    // Neither advance nor reset the once-per-item guard: the item is about to
    // end, and resetting here could let a stale end re-fire.
    CHECK(decide_auto_advance(playing(44.2, 45.0)) == AutoAdvance::None);
}

TEST_CASE("an item advances only once", "[auto_advance]") {
    AutoAdvanceInput in = playing(119.8);
    in.last_advanced_item = 2;
    CHECK(decide_auto_advance(in) == AutoAdvance::Held);
}

TEST_CASE("no advance before the new item has actually started",
          "[auto_advance]") {
    // Stale end-of-previous-item position must not skip the next item.
    AutoAdvanceInput in = playing(119.8);
    in.playback_started = false;
    CHECK(decide_auto_advance(in) == AutoAdvance::Held);
}

TEST_CASE("master shuffle advances on every end once playback started",
          "[auto_advance]") {
    AutoAdvanceInput in = playing(119.8);
    in.master_shuffle = true;
    in.last_advanced_item = 2;  // ignored in master shuffle
    CHECK(decide_auto_advance(in) == AutoAdvance::Advance);
    in.playback_started = false;
    CHECK(decide_auto_advance(in) == AutoAdvance::Held);
}

TEST_CASE("unknown duration never advances", "[auto_advance]") {
    AutoAdvanceInput in = playing(0.0, 45.0);
    in.duration = 0.0;
    CHECK(decide_auto_advance(in) == AutoAdvance::None);
}
