// The list overlay's cursor (app/overlay_nav.h): the playlist rows, the
// scroll window over them, and — while a non-4:3 playlist video plays — the
// Picture (crop) control one stop below the last row. Without the control
// these pin main.cpp's old inline ROTATE handler: one row per turn, clamped
// at both ends, the window scrolled just enough to keep the cursor on screen
// and never past the end of the list.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "app/app_state.h"
#include "app/overlay_nav.h"

namespace {

using app::OverlayCursor;

// The cursor is on screen and the window inside the list.
void check_window(const OverlayCursor& c, int rows, int max_visible) {
    INFO("selected " << c.selected << " scroll " << c.scroll << " rows " << rows
                     << " max_visible " << max_visible);
    CHECK(c.scroll >= 0);
    CHECK(c.scroll <= std::max(0, rows - max_visible));
    CHECK(c.selected >= c.scroll);
    CHECK(c.selected < c.scroll + max_visible);
}

bool same(const OverlayCursor& a, const OverlayCursor& b) {
    return a.selected == b.selected && a.scroll == b.scroll &&
           a.on_picture == b.on_picture;
}

}  // namespace

TEST_CASE("turning walks the rows, clamped at both ends, without the Picture control",
          "[overlay_nav]") {
    const int rows = 5;
    const int mv = 3;
    OverlayCursor c;

    // Up at the top: nothing moves.
    c = app::rotate_overlay(c, -1, rows, mv, false);
    CHECK(same(c, {0, 0, false}));

    // Down to the bottom; the window follows only once the cursor leaves it.
    const int expected_scroll[] = {0, 0, 1, 2};
    for (int i = 1; i < rows; ++i) {
        c = app::rotate_overlay(c, +1, rows, mv, false);
        CHECK(c.selected == i);
        CHECK(c.scroll == expected_scroll[i - 1]);
        CHECK_FALSE(c.on_picture);
        check_window(c, rows, mv);
    }

    // Down at the bottom: clamped, and no Picture stop is offered.
    c = app::rotate_overlay(c, +1, rows, mv, false);
    CHECK(same(c, {4, 2, false}));

    // Back up to the top; the window follows only once the cursor leaves it.
    const int expected_scroll_up[] = {2, 2, 1, 0};
    for (int i = rows - 2; i >= 0; --i) {
        c = app::rotate_overlay(c, -1, rows, mv, false);
        CHECK(c.selected == i);
        CHECK(c.scroll == expected_scroll_up[rows - 2 - i]);
        check_window(c, rows, mv);
    }
    c = app::rotate_overlay(c, -1, rows, mv, false);
    CHECK(same(c, {0, 0, false}));
}

TEST_CASE("one row per turn, whatever the turn's size", "[overlay_nav]") {
    OverlayCursor c = app::rotate_overlay({}, +3, 10, 4, false);
    CHECK(same(c, {1, 0, false}));
    c = app::rotate_overlay(c, -5, 10, 4, false);
    CHECK(same(c, {0, 0, false}));
    // No direction: nothing moves.
    c = app::rotate_overlay({2, 0, false}, 0, 10, 4, false);
    CHECK(same(c, {2, 0, false}));
}

TEST_CASE("a list shorter than the window never scrolls", "[overlay_nav]") {
    OverlayCursor c;
    for (int i = 0; i < 4; ++i) c = app::rotate_overlay(c, +1, 2, 8, false);
    CHECK(same(c, {1, 0, false}));
    // A window of zero rows (before the first draw) counts as one.
    c = app::rotate_overlay({0, 0, false}, +1, 3, 0, false);
    CHECK(same(c, {1, 1, false}));
}

TEST_CASE("turning down past the last row reaches the Picture control", "[overlay_nav]") {
    const int rows = 5;
    const int mv = 3;
    OverlayCursor c{4, 2, false};

    c = app::rotate_overlay(c, +1, rows, mv, true);
    CHECK(same(c, {4, 2, true}));

    // Down again: clamped on the control.
    c = app::rotate_overlay(c, +1, rows, mv, true);
    CHECK(same(c, {4, 2, true}));

    // Up: back to the last playlist row, then on up the list.
    c = app::rotate_overlay(c, -1, rows, mv, true);
    CHECK(same(c, {4, 2, false}));
    c = app::rotate_overlay(c, -1, rows, mv, true);
    CHECK(same(c, {3, 2, false}));

    // Only from the last row: down from row 3 is row 4, not the control.
    c = app::rotate_overlay(c, +1, rows, mv, true);
    CHECK(same(c, {4, 2, false}));
}

TEST_CASE("picture focus falls back when the control disappears", "[overlay_nav]") {
    const int rows = 5;
    const int mv = 3;

    // The next video is 4:3 (or playback stopped): the control is gone, so
    // the cursor returns to the last playlist row — never left on a control
    // that is not drawn.
    OverlayCursor c = app::clamp_overlay({rows - 1, 2, true}, rows, mv, false);
    CHECK_FALSE(c.on_picture);
    CHECK(c.selected == rows - 1);
    CHECK(c.scroll == 2);

    // Still offered: nothing changes.
    c = app::clamp_overlay({rows - 1, 2, true}, rows, mv, true);
    CHECK(same(c, {4, 2, true}));

    // A turn while it is gone cannot reach it either.
    c = app::rotate_overlay({rows - 1, 2, true}, +1, rows, mv, false);
    CHECK(same(c, {4, 2, false}));
}

TEST_CASE("on the Picture control the last row stays the one above it", "[overlay_nav]") {
    // A playlist import added rows while the cursor sat on the control: it
    // stays on the control, now below the NEW last row, which is on screen.
    OverlayCursor c = app::clamp_overlay({2, 0, true}, 6, 3, true);
    CHECK(same(c, {5, 3, true}));
    // Going up lands on that row.
    c = app::rotate_overlay(c, -1, 6, 3, true);
    CHECK(same(c, {5, 3, false}));

    // No rows at all: there is nothing for the control to sit below.
    c = app::clamp_overlay({0, 0, true}, 0, 3, true);
    CHECK_FALSE(c.on_picture);
    CHECK(c.scroll == 0);
}

TEST_CASE("clamp_overlay without the control only repairs the window", "[overlay_nav]") {
    // In range: untouched.
    CHECK(same(app::clamp_overlay({3, 1, false}, 5, 3, false), {3, 1, false}));
    CHECK(same(app::clamp_overlay({3, 1, false}, 5, 3, true), {3, 1, false}));
    // Cursor above the window: the window moves up to it.
    CHECK(same(app::clamp_overlay({1, 2, false}, 5, 3, false), {1, 1, false}));
    // Cursor below the window: the window moves down to it.
    CHECK(same(app::clamp_overlay({4, 0, false}, 5, 3, false), {4, 2, false}));
    // A window past the end of a shrunken list comes back.
    CHECK(same(app::clamp_overlay({0, 7, false}, 5, 3, false), {0, 0, false}));
    CHECK(same(app::clamp_overlay({1, 0, false}, 2, 8, false), {1, 0, false}));
}

TEST_CASE("AppState holds the overlay cursor", "[overlay_nav]") {
    app::AppState s;
    s.selected_index = 4;
    s.playlist_scroll_offset = 2;
    s.picture_focus = true;
    const OverlayCursor c = app::overlay_cursor(s);
    CHECK(same(c, {4, 2, true}));

    app::set_overlay_cursor(s, {1, 0, false});
    CHECK(s.selected_index == 1);
    CHECK(s.playlist_scroll_offset == 0);
    CHECK_FALSE(s.picture_focus);

    // Defaults: no control offered, focus off, cropped.
    const app::AppState fresh;
    CHECK_FALSE(fresh.picture_focus);
    CHECK_FALSE(fresh.picture_control_available);
    CHECK(fresh.picture_crop);
}
