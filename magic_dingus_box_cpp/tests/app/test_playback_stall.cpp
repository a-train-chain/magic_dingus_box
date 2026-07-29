// Watchdog for the failure that wedged a live box on 2026-07-29.
//
// A burst of navigation input (a controller pinned against something, buttons
// repeating) tipped a playlist switch into its timeout path:
//
//   CRITICAL: Playlist switch timeout after 2016ms - clearing flag and
//             resetting state
//   Debug info: video_active=1, is_playing=1, current_playlist=1, current_item=0
//
// The recovery cleared the flags and restored "I am playing", but left the
// GStreamer pipeline PAUSED. Confirmed in PulseAudio: the kiosk's own
// sink-input read `Corked: yes` while the kiosk reported `is_paused = false`.
// A paused pipeline never advances, the idle sink suspends, and the TV shows a
// still frame. Nothing detected the mismatch, so it stayed that way for seven
// hours until the service was restarted by hand.
//
// The signal that separates "wedged" from "fine" is simply position: a video
// the kiosk believes is playing MUST advance. This watchdog is the pure logic
// behind that check, kept out of the render loop so it can be tested at all —
// the real thing is only reproducible by corking an audio stream.

#include <catch2/catch_test_macros.hpp>

#include "app/playback_stall_watchdog.h"

using app::PlaybackStallWatchdog;
using Action = app::PlaybackStallWatchdog::Action;

namespace {
// The watchdog is fed wall-clock seconds; tests drive it explicitly so they
// never sleep and never depend on real timing.
constexpr double kTick = 0.25;  // the kiosk polls far faster than the threshold

// Advance a stalled clock until recovery fires, and REQUIRE that it does.
//
// Every loop here is bounded on purpose. The first draft used
// `while (update(...) != Recover)`, which spins forever against an
// implementation that never recovers — so the suite HUNG instead of failing.
// A hanging test is strictly worse than a failing one: it wedges CI and tells
// you nothing. Bounded loops turn the same defect into a clean assertion.
double run_until_recovery(PlaybackStallWatchdog& watchdog, double position,
                          double now, int max_polls = 200) {
    for (int i = 0; i < max_polls; ++i) {
        now += kTick;
        if (watchdog.update(true, position, now) == Action::Recover) {
            return now;
        }
    }
    FAIL("watchdog never recovered within " << max_polls << " polls");
    return now;
}
}  // namespace

TEST_CASE("a video whose position advances is never disturbed",
          "[playback][watchdog]") {
    PlaybackStallWatchdog watchdog;
    double now = 100.0;
    double position = 0.0;
    for (int i = 0; i < 200; ++i) {
        now += kTick;
        position += kTick;  // real-time playback
        REQUIRE(watchdog.update(true, position, now) == Action::None);
    }
}

TEST_CASE("a position frozen while the kiosk believes it is playing recovers",
          "[playback][watchdog]") {
    PlaybackStallWatchdog watchdog;
    double now = 100.0;

    // Establish a baseline the watchdog can measure movement against.
    REQUIRE(watchdog.update(true, 12.0, now) == Action::None);

    // Now the pipeline corks: position never moves again.
    bool recovered = false;
    for (int i = 0; i < 100 && !recovered; ++i) {
        now += kTick;
        recovered = watchdog.update(true, 12.0, now) == Action::Recover;
    }
    REQUIRE(recovered);
    // And it must not have taken an absurd amount of time to notice.
    REQUIRE(now - 100.0 < 10.0);
}

TEST_CASE("a brief hitch does not trigger recovery", "[playback][watchdog]") {
    // Loading, seeking and decoder hiccups all stall position briefly. Firing
    // on those would restart video mid-playback, which is worse than the bug.
    PlaybackStallWatchdog watchdog;
    double now = 100.0;
    REQUIRE(watchdog.update(true, 30.0, now) == Action::None);

    now += 2.0;  // two full seconds of no movement
    REQUIRE(watchdog.update(true, 30.0, now) == Action::None);

    now += 0.5;
    REQUIRE(watchdog.update(true, 30.5, now) == Action::None);  // resumed
}

TEST_CASE("nothing fires when the kiosk is not trying to play",
          "[playback][watchdog]") {
    // Deliberately paused, sitting in a menu, or running a game: position is
    // meant to be still, and a "recovery" would start playback the user never
    // asked for.
    PlaybackStallWatchdog watchdog;
    double now = 100.0;
    for (int i = 0; i < 200; ++i) {
        now += kTick;
        REQUIRE(watchdog.update(false, 42.0, now) == Action::None);
    }
}

TEST_CASE("the watchdog does not fire again on the very next poll",
          "[playback][watchdog]") {
    // Recovery is asynchronous — the pipeline needs a moment to actually
    // start. Re-firing every frame would hammer play() forever and guarantee
    // it never recovers.
    PlaybackStallWatchdog watchdog;
    double now = 100.0;
    watchdog.update(true, 5.0, now);
    now = run_until_recovery(watchdog, 5.0, now);
    // Immediately after recovering, still stalled — must stay quiet a while.
    int fired_again = 0;
    for (int i = 0; i < 8; ++i) {
        now += kTick;
        if (watchdog.update(true, 5.0, now) == Action::Recover) ++fired_again;
    }
    REQUIRE(fired_again == 0);
}

TEST_CASE("a stall that outlives one recovery attempt is retried",
          "[playback][watchdog]") {
    // One play() may not be enough. The watchdog has to keep trying, just not
    // continuously.
    PlaybackStallWatchdog watchdog;
    double now = 100.0;
    int recoveries = 0;
    for (int i = 0; i < 400; ++i) {
        now += kTick;
        if (watchdog.update(true, 7.0, now) == Action::Recover) ++recoveries;
    }
    REQUIRE(recoveries >= 2);
    // ...but not on every poll. 400 polls must not mean 400 restarts.
    REQUIRE(recoveries < 20);
}

TEST_CASE("recovery stops once playback actually resumes",
          "[playback][watchdog]") {
    PlaybackStallWatchdog watchdog;
    double now = 100.0;
    double position = 3.0;
    now = run_until_recovery(watchdog, position, now);
    // The recovery worked; position starts moving again.
    for (int i = 0; i < 200; ++i) {
        now += kTick;
        position += kTick;
        REQUIRE(watchdog.update(true, position, now) == Action::None);
    }
}

TEST_CASE("loading a new item clears the stall timer", "[playback][watchdog]") {
    // A new video starts at 0.0, which looks identical to "frozen at 0.0"
    // unless the caller says a fresh item was loaded. This is exactly the
    // state the live wedge sat in — item after item confirmed at position 0.
    PlaybackStallWatchdog watchdog;
    double now = 100.0;
    watchdog.update(true, 88.0, now);
    now += 3.0;  // long enough that a stall would be pending

    watchdog.reset();
    REQUIRE(watchdog.update(true, 0.0, now) == Action::None);
    now += 0.5;
    REQUIRE(watchdog.update(true, 0.0, now) == Action::None);
}

TEST_CASE("position going backwards counts as movement, not a stall",
          "[playback][watchdog]") {
    // Seeking backwards, or looping to the top of a trimmed clip, moves
    // position down. That is a live pipeline and must not be restarted.
    PlaybackStallWatchdog watchdog;
    double now = 100.0;
    REQUIRE(watchdog.update(true, 90.0, now) == Action::None);
    now += 1.0;
    REQUIRE(watchdog.update(true, 10.0, now) == Action::None);
    now += 1.0;
    REQUIRE(watchdog.update(true, 11.0, now) == Action::None);
}
