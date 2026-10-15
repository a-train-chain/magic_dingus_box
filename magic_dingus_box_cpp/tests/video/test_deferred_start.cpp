// DeferredStart: load_file(start > 0) without blocking the render thread.
//
// GstPlayer::load_file used to wait up to 3 s (gst_element_get_state) for
// preroll before seeking to `start`, on the render thread. That was harmless
// while no caller passed start > 0; playlist `start:` trims and Media Browser
// resume now both do, so every such load froze the kiosk for the preroll.
// The seek now waits for the per-frame state poll to see the pipeline
// prerolled. These tests pin the one-shot semantics GstPlayer relies on.

#include <catch2/catch_test_macros.hpp>

#include "video/deferred_start.h"

using video::DeferredStart;

TEST_CASE("a zero start never arms", "[deferred_start]") {
    DeferredStart d;
    d.arm(0.0);
    CHECK_FALSE(d.armed());
    CHECK_FALSE(d.take_if_prerolled(true).has_value());
}

TEST_CASE("the start seek waits for preroll", "[deferred_start]") {
    DeferredStart d;
    d.arm(42.5);
    REQUIRE(d.armed());
    // A seek before preroll can be dropped by some demuxers.
    CHECK_FALSE(d.take_if_prerolled(false).has_value());
    CHECK_FALSE(d.take_if_prerolled(false).has_value());
    CHECK(d.armed());

    auto t = d.take_if_prerolled(true);
    REQUIRE(t.has_value());
    CHECK(*t == 42.5);
}

TEST_CASE("the start seek fires exactly once", "[deferred_start]") {
    DeferredStart d;
    d.arm(10.0);
    REQUIRE(d.take_if_prerolled(true).has_value());
    CHECK_FALSE(d.armed());
    // The pipeline stays prerolled every later frame; seeking again would
    // yank playback back to the start trim forever.
    CHECK_FALSE(d.take_if_prerolled(true).has_value());
}

TEST_CASE("an explicit seek or stop cancels the pending start",
          "[deferred_start]") {
    // A user scrub before preroll must win, and a stop()/new load must not
    // inherit the previous file's start.
    DeferredStart d;
    d.arm(10.0);
    d.cancel();
    CHECK_FALSE(d.armed());
    CHECK_FALSE(d.take_if_prerolled(true).has_value());
}

TEST_CASE("while armed, the reported position is the start target",
          "[deferred_start]") {
    // Callers read position right after load_file(start) — Media Browser
    // watch checkpoints among them. Reporting the pre-seek 0 would let an
    // early checkpoint overwrite a resume point with "start of file".
    DeferredStart d;
    d.arm(600.0);
    CHECK(d.position_override().value_or(-1.0) == 600.0);
    d.take_if_prerolled(true);
    CHECK_FALSE(d.position_override().has_value());
}

TEST_CASE("re-arming replaces the target", "[deferred_start]") {
    DeferredStart d;
    d.arm(10.0);
    d.arm(20.0);
    auto t = d.take_if_prerolled(true);
    REQUIRE(t.has_value());
    CHECK(*t == 20.0);
    d.arm(30.0);
    d.arm(0.0);  // a later load from the beginning disarms
    CHECK_FALSE(d.armed());
}
