// app::tube_redraw — what a drawing tube asks of the main loop: Lean60
// motion rides the CRT field-rate path, Rich30 motion draws continuously
// and holds each flip to every second vblank (design section 4).

#include <catch2/catch_test_macros.hpp>

#include "app/tube_redraw.h"

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

TEST_CASE("tube redraw: a still tube asks for nothing; lean never holds", "[tube]") {
    using tube::FrameClass;
    const auto lean_still = app::tube_redraw(true, FrameClass::Lean60, false, 60);
    CHECK_FALSE(lean_still.field_rate_motion);
    CHECK_FALSE(lean_still.continuous_motion);
    CHECK_FALSE(lean_still.hold_present);
    const auto lean_moving = app::tube_redraw(true, FrameClass::Lean60, true, 120);
    CHECK_FALSE(lean_moving.continuous_motion);
    CHECK_FALSE(lean_moving.hold_present);
    const auto rich_moving = app::tube_redraw(true, FrameClass::Rich30, true, 60);
    CHECK_FALSE(rich_moving.field_rate_motion);
    CHECK(app::tube_redraw(true, FrameClass::Rich30, true, 48).hold_present);
    CHECK_FALSE(app::tube_redraw(true, FrameClass::Rich30, true, 47).hold_present);
    CHECK_FALSE(app::tube_redraw(false, FrameClass::Lean60, true, 60).field_rate_motion);
}
