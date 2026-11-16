// app::tube_redraw — what a drawing tube asks of the main loop: Lean60
// motion rides the CRT field-rate path, Rich30 motion draws continuously
// and holds each flip to every second vblank (design section 4).

#include <catch2/catch_test_macros.hpp>

#include "app/tube_redraw.h"

#include <chrono>
#include <cstdint>
#include <vector>

TEST_CASE("tube redraw: what each class asks of the loop", "[tube]") {
    using tube::FrameClass;
    auto none = app::tube_redraw(false, FrameClass::Rich30, true, 60);
    REQUIRE_FALSE(none.field_rate_motion); REQUIRE_FALSE(none.continuous_motion); REQUIRE_FALSE(none.hold_present);
    auto lean = app::tube_redraw(true, FrameClass::Lean60, true, 60);
    REQUIRE(lean.field_rate_motion); REQUIRE_FALSE(lean.hold_present);
    auto rich = app::tube_redraw(true, FrameClass::Rich30, false, 60);
    REQUIRE_FALSE(rich.continuous_motion); REQUIRE(rich.hold_present);
    REQUIRE(app::tube_redraw(true, FrameClass::Rich30, true, 120).continuous_motion);
    REQUIRE_FALSE(app::tube_redraw(true, FrameClass::Rich30, true, 30).hold_present);
}

TEST_CASE("tube redraw: a still tube asks for nothing; lean never holds at 50/60 Hz", "[tube]") {
    using tube::FrameClass;
    const auto lean_still = app::tube_redraw(true, FrameClass::Lean60, false, 60);
    CHECK_FALSE(lean_still.field_rate_motion);
    CHECK_FALSE(lean_still.continuous_motion);
    CHECK_FALSE(lean_still.hold_present);
    const auto lean_moving = app::tube_redraw(true, FrameClass::Lean60, true, 60);
    CHECK_FALSE(lean_moving.continuous_motion);
    CHECK_FALSE(lean_moving.hold_present);
    const auto rich_moving = app::tube_redraw(true, FrameClass::Rich30, true, 60);
    CHECK_FALSE(rich_moving.field_rate_motion);
    CHECK(app::tube_redraw(true, FrameClass::Rich30, true, 48).hold_present);
    CHECK_FALSE(app::tube_redraw(true, FrameClass::Rich30, true, 47).hold_present);
    CHECK_FALSE(app::tube_redraw(false, FrameClass::Lean60, true, 60).field_rate_motion);
}

TEST_CASE("tube redraw: lean holds where the 60 fps cap spans two vblanks", "[tube]") {
    using tube::FrameClass;
    for (const int hz : {0, 24, 30, 50, 59, 60, 75}) {
        const auto r = app::tube_redraw(true, FrameClass::Lean60, true, hz);
        CHECK_FALSE(r.hold_present);
        CHECK(r.hold_fps == 0);
    }
    for (const int hz : {100, 120, 144}) {
        const auto moving = app::tube_redraw(true, FrameClass::Lean60, true, hz);
        CHECK(moving.hold_present);
        CHECK(moving.hold_fps == 60);
        CHECK(moving.field_rate_motion);           // the motion path is unchanged
        CHECK_FALSE(moving.continuous_motion);
        CHECK(app::tube_redraw(true, FrameClass::Lean60, false, hz).hold_present);
        CHECK_FALSE(app::tube_redraw(false, FrameClass::Lean60, true, hz).hold_present);
    }
    // Rich holds at its own rate.
    CHECK(app::tube_redraw(true, FrameClass::Rich30, true, 60).hold_fps == 30);
    CHECK(app::tube_redraw(true, FrameClass::Rich30, true, 120).hold_fps == 30);
    CHECK(app::tube_redraw(true, FrameClass::Rich30, true, 30).hold_fps == 0);
}

namespace {

std::int64_t next_vblank(std::int64_t t, std::int64_t period) { return (t / period + 1) * period; }

// main.cpp's loop for one drawing tube with a constant render cost, every
// iteration drawing (video playing): tube_redraw decides; a held frame sleeps
// utils::capped_present_hold_at after the swap and utils::held_frame_sleep_at
// at the bottom, an unheld one sleeps the ordinary 60 fps cap
// (utils::frame_cap_sleep). Blocking flips. Returns each flip's interval.
std::vector<std::int64_t> run_loop(tube::FrameClass cls, int hz, std::int64_t render, int frames) {
    using std::chrono::microseconds;
    const auto redraw = app::tube_redraw(true, cls, true, hz);
    const auto cap = utils::frame_pacing_for(60, hz);
    const std::int64_t period = 1000000 / hz;
    std::vector<std::int64_t> intervals;
    std::int64_t last_flip = 0, t = 0;
    for (int i = 0; i < frames; ++i) {
        const std::int64_t start = t;
        t += render;
        if (redraw.hold_present)
            t += utils::capped_present_hold_at(redraw.hold_fps, hz, microseconds{t - last_flip}).count();
        t = next_vblank(t, period);
        intervals.push_back(t - last_flip);
        last_flip = t;
        t += (redraw.hold_present ? utils::held_frame_sleep_at(redraw.hold_fps, hz, microseconds{t - start})
                                  : utils::frame_cap_sleep(cap, microseconds{t - start}))
                 .count();
    }
    return intervals;
}

}  // namespace

TEST_CASE("tube redraw: a lean frame at 120 Hz lands on the 2nd vblank after the last flip",
          "[tube]") {
    using tube::FrameClass;
    const std::int64_t two_vblanks = 2 * (1000000 / 120);   // 16.67 ms
    for (const std::int64_t render : {5000, 10000, 12000}) {
        const auto iv = run_loop(FrameClass::Lean60, 120, render, 60);
        for (std::size_t i = 1; i < iv.size(); ++i) CHECK(iv[i] == two_vblanks);
    }
}

TEST_CASE("tube redraw: a lean frame at 50 and 60 Hz is not held and flips every vblank",
          "[tube]") {
    using tube::FrameClass;
    for (const int hz : {50, 60}) {
        REQUIRE_FALSE(app::tube_redraw(true, FrameClass::Lean60, true, hz).hold_present);
        const std::int64_t period = 1000000 / hz;
        for (const std::int64_t render : {5000, 10000, 12000}) {
            const auto iv = run_loop(FrameClass::Lean60, hz, render, 60);
            for (std::size_t i = 1; i < iv.size(); ++i) CHECK(iv[i] == period);
        }
    }
}
