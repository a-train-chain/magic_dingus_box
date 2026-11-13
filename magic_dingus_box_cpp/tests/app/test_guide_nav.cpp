// The Channel Guide's cursor (app/guide_nav.h): the channel rows, the grid's
// 5-row window over them, and — while a non-4:3 playlist video plays — the
// Picture (crop) control one stop ABOVE channel 00, in the now-playing panel.
// One row per turn, clamped at both ends; the window scrolled just enough to
// keep the cursor row on screen and never past the last channel.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "app/guide_nav.h"

namespace {

using app::GuideCursor;
using app::GuidePress;
using app::kGuideVisibleRows;

// The cursor row is on screen and the window inside the list.
void check_window(const GuideCursor& c, int rows) {
    INFO("row " << c.row << " scroll " << c.scroll << " rows " << rows);
    CHECK(c.scroll >= 0);
    CHECK(c.scroll <= std::max(0, rows - kGuideVisibleRows));
    CHECK(c.row >= c.scroll);
    CHECK(c.row < c.scroll + kGuideVisibleRows);
}

bool same(const GuideCursor& a, const GuideCursor& b) {
    return a.row == b.row && a.scroll == b.scroll && a.on_picture == b.on_picture;
}

}  // namespace

TEST_CASE("5 channel rows are on screen", "[guide_nav]") {
    CHECK(kGuideVisibleRows == 5);
}

TEST_CASE("the guide opens on the tuned channel, with it on screen", "[guide_nav]") {
    SECTION("8 channels: every tuned row lands on its row, on screen") {
        for (int tuned = 0; tuned < 8; ++tuned) {
            const GuideCursor c = app::guide_open(tuned, 8);
            CHECK(c.row == tuned);
            CHECK_FALSE(c.on_picture);
            check_window(c, 8);
        }
    }
    SECTION("the tuned row is centred as far as the list allows") {
        CHECK(same(app::guide_open(0, 8), {0, 0, false}));
        CHECK(same(app::guide_open(2, 8), {2, 0, false}));
        CHECK(same(app::guide_open(4, 8), {4, 2, false}));
        CHECK(same(app::guide_open(7, 8), {7, 3, false}));  // never past the end
        CHECK(same(app::guide_open(5, 20), {5, 3, false}));
    }
    SECTION("fewer channels than rows: no scroll") {
        CHECK(same(app::guide_open(2, 3), {2, 0, false}));
    }
    SECTION("nothing tuned, or a tuned row the list no longer has: row 0") {
        CHECK(same(app::guide_open(-1, 8), {0, 0, false}));
        CHECK(same(app::guide_open(8, 8), {0, 0, false}));
        CHECK(same(app::guide_open(0, 0), {0, 0, false}));
    }
}

TEST_CASE("turning walks the channels one row at a time, clamped at both ends",
          "[guide_nav]") {
    const int rows = 4;
    GuideCursor c;

    // Down to the last row.
    for (int i = 1; i < rows; ++i) {
        c = app::guide_rotate(c, +1, rows, false);
        CHECK(c.row == i);
        CHECK_FALSE(c.on_picture);
    }
    // Down at the last row: clamped (the knob never wraps).
    c = app::guide_rotate(c, +1, rows, false);
    CHECK(same(c, {3, 0, false}));
    c = app::guide_rotate(c, +1, rows, true);
    CHECK(same(c, {3, 0, false}));

    // A fast turn is still one row.
    c = app::guide_rotate(c, -7, rows, false);
    CHECK(same(c, {2, 0, false}));

    // A zero turn moves nothing.
    CHECK(same(app::guide_rotate(c, 0, rows, true), c));

    // Up to the top; without the control, up at row 0 is clamped.
    c = app::guide_rotate(c, -1, rows, false);
    c = app::guide_rotate(c, -1, rows, false);
    CHECK(same(c, {0, 0, false}));
    c = app::guide_rotate(c, -1, rows, false);
    CHECK(same(c, {0, 0, false}));
}

TEST_CASE("up from channel 00 reaches the Picture control only when it is there",
          "[guide_nav]") {
    const int rows = 8;
    GuideCursor c = app::guide_open(1, rows);

    c = app::guide_rotate(c, -1, rows, true);
    CHECK(same(c, {0, 0, false}));

    SECTION("without the control: clamped at row 0") {
        c = app::guide_rotate(c, -1, rows, false);
        CHECK(same(c, {0, 0, false}));
    }
    SECTION("with the control: onto it, the grid at its top") {
        c = app::guide_rotate(c, -1, rows, true);
        CHECK(same(c, {0, 0, true}));
        // Up from the control: nothing further.
        c = app::guide_rotate(c, -1, rows, true);
        CHECK(same(c, {0, 0, true}));
    }
}

TEST_CASE("down from the Picture control returns to channel 00", "[guide_nav]") {
    const GuideCursor on_picture{0, 0, true};
    CHECK(same(app::guide_rotate(on_picture, +1, 8, true), {0, 0, false}));
    // A fast turn down still stops at 00 first.
    CHECK(same(app::guide_rotate(on_picture, +5, 8, true), {0, 0, false}));
}

TEST_CASE("the Picture control vanishing puts the cursor on channel 00", "[guide_nav]") {
    const GuideCursor on_picture{0, 0, true};
    SECTION("the clamp drops it") {
        CHECK(same(app::guide_clamp(on_picture, 8, false), {0, 0, false}));
    }
    SECTION("a turn up without it does not keep it") {
        CHECK(same(app::guide_rotate(on_picture, -1, 8, false), {0, 0, false}));
    }
    SECTION("while it is there, the clamp keeps it") {
        CHECK(same(app::guide_clamp(on_picture, 8, true), on_picture));
    }
    SECTION("a cursor on the control with a stale window is repaired") {
        CHECK(same(app::guide_clamp({4, 2, true}, 8, true), {0, 0, true}));
    }
    SECTION("no channels: never on the control") {
        CHECK(same(app::guide_clamp(on_picture, 0, true), {0, 0, false}));
        CHECK(same(app::guide_rotate({0, 0, false}, -1, 0, true), {0, 0, false}));
    }
}

TEST_CASE("8 channels scroll a 5-row window that always holds the cursor row",
          "[guide_nav]") {
    const int rows = 8;
    GuideCursor c;

    // Down: the window follows only once the cursor leaves it.
    const int expected_scroll_down[] = {0, 0, 0, 0, 1, 2, 3};
    for (int i = 1; i < rows; ++i) {
        c = app::guide_rotate(c, +1, rows, false);
        CHECK(c.row == i);
        CHECK(c.scroll == expected_scroll_down[i - 1]);
        check_window(c, rows);
    }
    CHECK(same(c, {7, 3, false}));

    // Up: likewise.
    const int expected_scroll_up[] = {3, 3, 3, 3, 2, 1, 0};
    for (int i = rows - 2; i >= 0; --i) {
        c = app::guide_rotate(c, -1, rows, false);
        CHECK(c.row == i);
        CHECK(c.scroll == expected_scroll_up[rows - 2 - i]);
        check_window(c, rows);
    }
    CHECK(same(c, {0, 0, false}));
}

TEST_CASE("the clamp repairs a cursor the channel list changed under", "[guide_nav]") {
    SECTION("a consistent cursor comes back unchanged") {
        CHECK(same(app::guide_clamp({6, 3, false}, 8, false), {6, 3, false}));
        CHECK(same(app::guide_clamp({2, 1, false}, 8, false), {2, 1, false}));
    }
    SECTION("the list shrank under the cursor: the last channel, on screen") {
        const GuideCursor c = app::guide_clamp({7, 3, false}, 3, false);
        CHECK(same(c, {2, 0, false}));
    }
    SECTION("a window past the list's end is pulled back") {
        const GuideCursor c = app::guide_clamp({5, 5, false}, 6, false);
        CHECK(same(c, {5, 1, false}));
        check_window(c, 6);
    }
    SECTION("a negative row or scroll is pulled back") {
        CHECK(same(app::guide_clamp({-3, -1, false}, 8, false), {0, 0, false}));
    }
    SECTION("no channels") {
        CHECK(same(app::guide_clamp({4, 2, false}, 0, false), {0, 0, false}));
    }
}

TEST_CASE("the cursor follows a channel change, scrolling only as far as needed",
          "[guide_nav]") {
    const int rows = 8;
    SECTION("channel up within the window") {
        CHECK(same(app::guide_follow({1, 0, false}, 2, rows), {2, 0, false}));
    }
    SECTION("channel up past the window's bottom") {
        CHECK(same(app::guide_follow({4, 0, false}, 5, rows), {5, 1, false}));
    }
    SECTION("channel up wrapping from the last channel to 00") {
        CHECK(same(app::guide_follow({7, 3, false}, 0, rows), {0, 0, false}));
    }
    SECTION("channel down wrapping from 00 to the last channel") {
        CHECK(same(app::guide_follow({0, 0, false}, 7, rows), {7, 3, false}));
    }
    SECTION("from the Picture control, onto the channel's row") {
        CHECK(same(app::guide_follow({0, 0, true}, 1, rows), {1, 0, false}));
    }
    SECTION("a channel the list does not have is clamped into it") {
        CHECK(same(app::guide_follow({0, 0, false}, 9, rows), {7, 3, false}));
    }
}

TEST_CASE("a press tunes another channel, closes on the tuned one, toggles the picture",
          "[guide_nav]") {
    SECTION("another channel's row with a schedule: tune it") {
        CHECK(app::guide_press({3, 0, false}, 1, true) == GuidePress::Tune);
        CHECK(app::guide_press({0, 0, false}, 2, true) == GuidePress::Tune);
    }
    SECTION("nothing tuned: any row with a schedule tunes") {
        CHECK(app::guide_press({2, 0, false}, -1, true) == GuidePress::Tune);
    }
    SECTION("the tuned channel's row: close the guide") {
        CHECK(app::guide_press({1, 0, false}, 1, true) == GuidePress::Close);
        CHECK(app::guide_press({0, 0, false}, 0, true) == GuidePress::Close);
    }
    SECTION("a row with no schedule: nothing") {
        CHECK(app::guide_press({4, 0, false}, 1, false) == GuidePress::None);
        CHECK(app::guide_press({4, 0, false}, -1, false) == GuidePress::None);
    }
    SECTION("the Picture control: toggle crop, whatever row 0 is") {
        CHECK(app::guide_press({0, 0, true}, 1, true) == GuidePress::TogglePicture);
        CHECK(app::guide_press({0, 0, true}, 0, true) == GuidePress::TogglePicture);
        CHECK(app::guide_press({0, 0, true}, 1, false) == GuidePress::TogglePicture);
    }
}
