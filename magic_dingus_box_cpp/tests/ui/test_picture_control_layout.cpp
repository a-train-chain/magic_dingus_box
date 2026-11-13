// Tests for ui::place_picture_control (ui/picture_control_layout.h) — where
// the overlay's CROP checkbox goes in the bottom-right now-playing block.
//
// The control sits to the LEFT of the video title on the title's line. A long
// title pushes it left; once it would cross into the left half of the canvas
// (where the playlist list is) it moves to its own line above the title,
// right-aligned with the block. Checked on both logical canvases: 960 wide
// (Modern TV) and 640 wide (CRT Native). Design:
// docs/superpowers/specs/2026-11-12-video-crop-toggle-design.md, "Layout".

#include <catch2/catch_test_macros.hpp>

#include "ui/picture_control_layout.h"

namespace {

// Realistic numbers at font_small_size (14): a 14 px box, a 6 px gap and
// "CROP" ~38 px wide; the footer's title baseline at 720 tall; the footer's
// 18 px line advance.
constexpr float kControlW = 58.0f;
constexpr float kBaseline = 671.0f;
constexpr float kLineH = 18.0f;

// render_footer right-aligns the title 80 px from the canvas edge.
float title_x_for(float canvas_w, float title_w) { return canvas_w - title_w - 80.0f; }

}  // namespace

TEST_CASE("a short title keeps the control on its line, 16 px to its left",
          "[picture_control]") {
    const float title_x = title_x_for(960.0f, 70.0f);  // 810
    const auto pos = ui::place_picture_control(960.0f, title_x, kBaseline, kControlW, kLineH);
    CHECK_FALSE(pos.own_line);
    CHECK(pos.x == title_x - 16.0f - kControlW);
    CHECK(pos.y == kBaseline);
    // Never reaches the title.
    CHECK(pos.x + kControlW < title_x);
}

TEST_CASE("a long title at 960 moves the control to its own line above it",
          "[picture_control]") {
    const float title_x = title_x_for(960.0f, 400.0f);  // 480: control would start at 406
    const auto pos = ui::place_picture_control(960.0f, title_x, kBaseline, kControlW, kLineH);
    CHECK(pos.own_line);
    CHECK(pos.x == 960.0f - 80.0f - kControlW);
    CHECK(pos.y == kBaseline - kLineH - 6.0f);
    // Stays in the right half, clear of the playlist list.
    CHECK(pos.x >= 960.0f / 2.0f);
}

TEST_CASE("a long title at 640 moves the control to its own line above it",
          "[picture_control]") {
    const float title_x = title_x_for(640.0f, 250.0f);  // 310: control would start at 236
    const auto pos = ui::place_picture_control(640.0f, title_x, 431.0f, kControlW, kLineH);
    CHECK(pos.own_line);
    CHECK(pos.x == 640.0f - 80.0f - kControlW);
    CHECK(pos.y == 431.0f - kLineH - 6.0f);
    CHECK(pos.x >= 640.0f / 2.0f);
}

TEST_CASE("a short title at 640 keeps the control on its line", "[picture_control]") {
    const float title_x = title_x_for(640.0f, 70.0f);  // 490: control starts at 416
    const auto pos = ui::place_picture_control(640.0f, title_x, 431.0f, kControlW, kLineH);
    CHECK_FALSE(pos.own_line);
    CHECK(pos.x == title_x - 16.0f - kControlW);
    CHECK(pos.y == 431.0f);
}

TEST_CASE("the control may start exactly at the canvas centre and stay on the line",
          "[picture_control]") {
    // "Left of canvas_w / 2" is strict: starting ON the centre still fits.
    const float on_centre_title_x = 480.0f + 16.0f + kControlW;
    const auto on_centre =
        ui::place_picture_control(960.0f, on_centre_title_x, kBaseline, kControlW, kLineH);
    CHECK_FALSE(on_centre.own_line);
    CHECK(on_centre.x == 480.0f);

    const auto one_px_left = ui::place_picture_control(960.0f, on_centre_title_x - 1.0f,
                                                       kBaseline, kControlW, kLineH);
    CHECK(one_px_left.own_line);
}
