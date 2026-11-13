// The Channel Guide's cursor (app/guide_nav.h): the channel rows, the grid's
// 5-row window over them, and — while a non-4:3 playlist video plays — the
// Picture (crop) control one stop ABOVE channel 00, in the now-playing panel.
// One row per turn, clamped at both ends; the window scrolled just enough to
// keep the cursor row on screen and never past the last channel. Then the
// input rules main.cpp applies (guide_input / guide_tune_taken /
// guide_frame): who owns the guide, what a press does, when the cursor is
// reset.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <optional>

#include "app/app_state.h"
#include "app/guide_nav.h"
#include "app/playback_reset.h"

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

// ── Input rules ──────────────────────────────────────────────────────────

namespace {

// A channel playing (the playing indexes set, video up), the guide hidden.
void put_playing(app::AppState& s, int tuned = 1) {
    s.video_active = true;
    s.current_playlist_index = tuned;
    s.current_item_index = 0;
    s.tuned_channel = tuned;
    s.ui_visible_when_playing = false;
}

// Nothing playing or loading (stop_to_menu's state).
void put_nothing_playing(app::AppState& s, int tuned = 1) {
    s.video_active = false;
    s.current_playlist_index = -1;
    s.current_item_index = -1;
    s.tuned_channel = tuned;
    s.ui_visible_when_playing = true;
}

// The playing indexes set with no video yet: a tune's settle, an item
// boundary's preroll.
void put_loading(app::AppState& s, int channel = 1) {
    s.video_active = false;
    s.current_playlist_index = channel;
    s.current_item_index = 0;
}

app::GuideFacts facts(const app::AppState& s, int rows = 8,
                      std::optional<int> pending = std::nullopt, bool tunable = true) {
    app::GuideFacts f;
    f.on_screen = app::guide_visible(s);
    f.rows = rows;
    f.pending_tune = pending;
    f.cursor_row_tunable = tunable;
    f.owns_pipeline = true;
    f.now = std::chrono::steady_clock::time_point(std::chrono::seconds(1000));
    return f;
}

app::GuideCommand input(app::AppState& s, app::GuideEvent e, int delta = 0,
                        std::optional<int> pending = std::nullopt, bool tunable = true) {
    return app::guide_input(s, e, delta, facts(s, 8, pending, tunable));
}

bool no_calls(const app::GuideCommand& c) { return c.tune == -1 && !c.toggle_picture; }

app::GuideFrameFacts frame_facts(std::optional<int> pending = std::nullopt,
                                 bool may_stay = false, int rows = 8) {
    app::GuideFrameFacts f;
    f.guide_style = true;
    f.rows = rows;
    f.pending_tune = pending;
    f.picture_may_stay = may_stay;
    return f;
}

}  // namespace

TEST_CASE("the guide is on screen with the overlay up or with nothing playing or loading",
          "[guide_nav]") {
    app::AppState s;
    SECTION("over a playing video: only with the overlay up") {
        put_playing(s);
        CHECK_FALSE(app::guide_visible(s));
        s.ui_visible_when_playing = true;
        CHECK(app::guide_visible(s));
    }
    SECTION("nothing playing or loading: always (the menu is drawn then)") {
        put_nothing_playing(s);
        CHECK(app::guide_visible(s));
        s.ui_visible_when_playing = false;
        CHECK(app::guide_visible(s));
    }
    SECTION("a load gap (a tune's settle, an item boundary): only with the overlay up") {
        put_loading(s);
        s.ui_visible_when_playing = true;
        CHECK(app::guide_visible(s));
        s.ui_visible_when_playing = false;
        CHECK_FALSE(app::guide_visible(s));
    }
#ifdef MEDIA_BROWSER_ENABLED
    SECTION("never while the Media Browser owns the screen") {
        app::reset_main_ui_for_media_browser(s);  // nothing playing, overlay off
        s.current_screen = app::AppScreen::MediaBrowser;
        CHECK_FALSE(app::guide_visible(s));
        s.ui_visible_when_playing = true;
        CHECK_FALSE(app::guide_visible(s));
        s.current_screen = app::AppScreen::MainMenu;
        CHECK(app::guide_visible(s));
    }
#endif
}

TEST_CASE("SELECT on the hidden guide opens it on the tuned channel, the viewer's",
          "[guide_nav]") {
    app::AppState s;
    put_playing(s, 3);
    s.guide_cursor = {7, 3, false};
    s.is_fading = true;  // a Close's fade-out still running
    REQUIRE_FALSE(app::guide_visible(s));

    SECTION("on the tuned channel") {
        CHECK(no_calls(input(s, app::GuideEvent::Select)));
        CHECK(same(s.guide_cursor, app::guide_open(3, 8)));
        CHECK(s.ui_visible_when_playing);
        CHECK(s.guide_opened_by_viewer);
        CHECK_FALSE(s.is_fading);
    }
    SECTION("on the channel a pending tune is loading") {
        input(s, app::GuideEvent::Select, 0, 5);
        CHECK(s.guide_cursor.row == 5);
    }
    SECTION("not while the intro plays") {
        s.showing_intro_video = true;
        CHECK(no_calls(input(s, app::GuideEvent::Select)));
        CHECK_FALSE(s.ui_visible_when_playing);
        CHECK_FALSE(s.guide_opened_by_viewer);
        CHECK(same(s.guide_cursor, {7, 3, false}));
    }
}

TEST_CASE("a turn moves the shown guide's cursor and makes it the viewer's", "[guide_nav]") {
    app::AppState s;
    SECTION("over a playing video") {
        put_playing(s);
        s.ui_visible_when_playing = true;
        s.guide_cursor = {1, 0, false};
        CHECK(no_calls(input(s, app::GuideEvent::Rotate, +1)));
        CHECK(same(s.guide_cursor, {2, 0, false}));
        CHECK(s.guide_opened_by_viewer);
    }
    SECTION("with nothing playing, the overlay flag goes up too") {
        // Drawn without it (the menu shows whenever there is no video); it
        // must stay up when the video comes back.
        put_nothing_playing(s);
        s.ui_visible_when_playing = false;
        s.guide_cursor = {1, 0, false};
        input(s, app::GuideEvent::Rotate, -1);
        CHECK(same(s.guide_cursor, {0, 0, false}));
        CHECK(s.guide_opened_by_viewer);
        CHECK(s.ui_visible_when_playing);
    }
    SECTION("up from 00 reaches the Picture control while it is drawn") {
        put_playing(s);
        s.ui_visible_when_playing = true;
        s.picture_control_available = true;
        input(s, app::GuideEvent::Rotate, -1);
        CHECK(same(s.guide_cursor, {0, 0, true}));
    }
    SECTION("hidden: nothing (main.cpp seeks)") {
        put_playing(s);
        s.guide_cursor = {1, 0, false};
        CHECK(no_calls(input(s, app::GuideEvent::Rotate, +1)));
        CHECK(same(s.guide_cursor, {1, 0, false}));
        CHECK_FALSE(s.guide_opened_by_viewer);
        CHECK_FALSE(s.ui_visible_when_playing);
    }
}

TEST_CASE("a press on another channel tunes it, ownership dropped first", "[guide_nav]") {
    app::AppState s;
    put_playing(s, 1);
    s.ui_visible_when_playing = true;
    s.guide_opened_by_viewer = true;
    s.guide_cursor = {2, 0, false};

    const auto cmd = input(s, app::GuideEvent::Select);
    CHECK(cmd.tune == 2);
    CHECK_FALSE(cmd.toggle_picture);
    CHECK_FALSE(s.guide_opened_by_viewer);  // the tune's load closes the guide
    CHECK(s.ui_visible_when_playing);       // not before the load

    SECTION("taken: its wait begins") {
        app::guide_tune_taken(s, 2, true);
        CHECK(s.guide_viewer_tune == 2);
        CHECK_FALSE(s.guide_opened_by_viewer);
    }
    SECTION("dropped (a switch in flight): the guide stays the viewer's") {
        app::guide_tune_taken(s, 2, false);
        CHECK(s.guide_viewer_tune == -1);
        CHECK(s.guide_opened_by_viewer);
        CHECK(s.ui_visible_when_playing);
    }
}

TEST_CASE("a press on the tuned channel closes the guide, fading over a video",
          "[guide_nav]") {
    app::AppState s;
    SECTION("over a playing video: today's fade-out") {
        put_playing(s, 1);
        s.ui_visible_when_playing = true;
        s.guide_opened_by_viewer = true;
        s.guide_cursor = {1, 0, false};
        CHECK(no_calls(input(s, app::GuideEvent::Select)));
        CHECK_FALSE(s.ui_visible_when_playing);
        CHECK_FALSE(s.guide_opened_by_viewer);
        CHECK(s.is_fading);
        CHECK_FALSE(s.fade_target_ui_visible);
        CHECK(s.fade_start_time ==
              std::chrono::steady_clock::time_point(std::chrono::seconds(1000)));
    }
    SECTION("in a load gap (the channel being tuned): hidden, no fade") {
        put_playing(s, 1);
        put_loading(s, 2);
        s.ui_visible_when_playing = true;
        s.guide_cursor = {2, 0, false};
        CHECK(no_calls(input(s, app::GuideEvent::Select, 0, /*pending=*/2)));
        CHECK_FALSE(s.ui_visible_when_playing);
        CHECK_FALSE(s.is_fading);
    }
}

TEST_CASE("with nothing playing, a press on the tuned channel retries it", "[guide_nav]") {
    // A failed item's dark window (stop_to_menu keeps the channel tuned
    // while its slot is waited out): the press tunes it again.
    app::AppState s;
    put_nothing_playing(s, 1);
    s.guide_cursor = {1, 0, false};
    SECTION("a channel with a schedule: tuned") {
        const auto cmd = input(s, app::GuideEvent::Select);
        CHECK(cmd.tune == 1);
        CHECK_FALSE(s.guide_opened_by_viewer);
        CHECK(s.ui_visible_when_playing);  // the load decides
    }
    SECTION("no schedule any more: nothing, and the guide is the viewer's") {
        CHECK(no_calls(input(s, app::GuideEvent::Select, 0, std::nullopt, false)));
        CHECK(s.guide_opened_by_viewer);
    }
    SECTION("over a playing video the same press still closes") {
        put_playing(s, 1);
        s.ui_visible_when_playing = true;
        CHECK(no_calls(input(s, app::GuideEvent::Select)));
        CHECK_FALSE(s.ui_visible_when_playing);
    }
}

TEST_CASE("the Picture control toggles crop; a row with no schedule does nothing",
          "[guide_nav]") {
    app::AppState s;
    put_playing(s, 1);
    s.ui_visible_when_playing = true;
    SECTION("on the control: toggle, the guide stays and is the viewer's") {
        s.guide_cursor = {0, 0, true};
        const auto cmd = input(s, app::GuideEvent::Select);
        CHECK(cmd.toggle_picture);
        CHECK(cmd.tune == -1);
        CHECK(s.ui_visible_when_playing);
        CHECK(s.guide_opened_by_viewer);
    }
    SECTION("the pipeline changed hands before the clamp ran: no toggle") {
        s.guide_cursor = {0, 0, true};
        auto f = facts(s);
        f.owns_pipeline = false;
        CHECK(no_calls(app::guide_input(s, app::GuideEvent::Select, 0, f)));
        CHECK(s.guide_opened_by_viewer);
    }
    SECTION("a row with no schedule: nothing, the viewer's") {
        s.guide_cursor = {4, 0, false};
        CHECK(no_calls(input(s, app::GuideEvent::Select, 0, std::nullopt, false)));
        CHECK(s.ui_visible_when_playing);
        CHECK(s.guide_opened_by_viewer);
    }
}

TEST_CASE("channel up/down on the shown guide: the viewer's, the cursor follows",
          "[guide_nav]") {
    app::AppState s;
    put_playing(s, 1);
    s.ui_visible_when_playing = true;
    s.guide_cursor = {4, 0, false};
    SECTION("the step was taken: onto the channel being tuned") {
        CHECK(no_calls(input(s, app::GuideEvent::ChannelStep, 0, 7)));
        CHECK(same(s.guide_cursor, {7, 3, false}));
        CHECK(s.guide_opened_by_viewer);
    }
    SECTION("nothing pending: the cursor stays") {
        input(s, app::GuideEvent::ChannelStep);
        CHECK(same(s.guide_cursor, {4, 0, false}));
        CHECK(s.guide_opened_by_viewer);
    }
    SECTION("hidden: nothing") {
        s.ui_visible_when_playing = false;
        input(s, app::GuideEvent::ChannelStep, 0, 7);
        CHECK(same(s.guide_cursor, {4, 0, false}));
        CHECK_FALSE(s.guide_opened_by_viewer);
    }
}

TEST_CASE("a viewer's Tune closes the guide even if the knob ticks during the wait",
          "[guide_nav]") {
    app::AppState s;
    put_playing(s, 1);
    s.ui_visible_when_playing = true;
    app::guide_frame(s, frame_facts());  // on screen already: no edge below
    s.guide_cursor = {3, 0, false};
    const auto cmd = input(s, app::GuideEvent::Select);
    REQUIRE(cmd.tune == 3);
    app::guide_tune_taken(s, 3, true);
    put_loading(s, 3);  // tune() stopped the old stream and points at 03
    const std::optional<int> pending = 3;

    SECTION("a turn neither moves the cursor off the row nor owns the guide") {
        CHECK(no_calls(input(s, app::GuideEvent::Rotate, +1, pending)));
        CHECK(no_calls(input(s, app::GuideEvent::Rotate, -2, pending)));
        CHECK(s.guide_cursor.row == 3);
        CHECK_FALSE(s.guide_opened_by_viewer);
    }
    SECTION("channel up/down does not own it either") {
        input(s, app::GuideEvent::ChannelStep, 0, pending);
        CHECK(s.guide_cursor.row == 3);
        CHECK_FALSE(s.guide_opened_by_viewer);
    }
    SECTION("SELECT again closes it at once") {
        CHECK(no_calls(input(s, app::GuideEvent::Select, 0, pending)));
        CHECK_FALSE(s.ui_visible_when_playing);
    }
    SECTION("once the load has run the wait is over: turns own the guide again") {
        app::guide_frame(s, frame_facts(std::nullopt));
        CHECK(s.guide_viewer_tune == -1);
        input(s, app::GuideEvent::Rotate, +1);
        CHECK(s.guide_cursor.row == 4);
        CHECK(s.guide_opened_by_viewer);
    }
    SECTION("another tune's wait (the nothing-playing rule) does not hold the knob") {
        s.guide_viewer_tune = -1;
        input(s, app::GuideEvent::Rotate, +1, pending);
        CHECK(s.guide_cursor.row == 4);
        CHECK(s.guide_opened_by_viewer);
    }
}

TEST_CASE("each frame, ownership needs the overlay up and guide style", "[guide_nav]") {
    app::AppState s;
    put_playing(s, 1);
    s.ui_visible_when_playing = true;
    s.guide_opened_by_viewer = true;
    app::guide_frame(s, frame_facts());
    CHECK(s.guide_opened_by_viewer);

    SECTION("the overlay hidden (Close, a tune landing, the Media Browser, the intro)") {
        s.ui_visible_when_playing = false;
        app::guide_frame(s, frame_facts());
        CHECK_FALSE(s.guide_opened_by_viewer);
    }
    SECTION("List style") {
        auto f = frame_facts();
        f.guide_style = false;
        app::guide_frame(s, f);
        CHECK_FALSE(s.guide_opened_by_viewer);
    }
}

TEST_CASE("each frame, a viewer's Tune wait ends when its tune is no longer pending",
          "[guide_nav]") {
    app::AppState s;
    put_loading(s, 3);
    s.ui_visible_when_playing = true;
    s.guide_viewer_tune = 3;
    app::guide_frame(s, frame_facts(3));
    CHECK(s.guide_viewer_tune == 3);
    app::guide_frame(s, frame_facts(5));  // something else is pending
    CHECK(s.guide_viewer_tune == -1);
    s.guide_viewer_tune = 3;
    app::guide_frame(s, frame_facts(std::nullopt));  // loaded
    CHECK(s.guide_viewer_tune == -1);
}

TEST_CASE("a guide that comes on screen without SELECT starts on the tuned row",
          "[guide_nav]") {
    app::AppState s;
    put_playing(s, 2);
    s.guide_cursor = {6, 3, false};
    app::guide_frame(s, frame_facts());  // hidden: no edge yet
    CHECK(same(s.guide_cursor, {6, 3, false}));

    SECTION("a failed item's stop_to_menu") {
        put_nothing_playing(s, 2);
        app::guide_frame(s, frame_facts());
        CHECK(same(s.guide_cursor, app::guide_open(2, 8)));
        // Once only: the viewer's moves after it are kept.
        s.guide_cursor = {5, 1, false};
        app::guide_frame(s, frame_facts());
        CHECK(same(s.guide_cursor, {5, 1, false}));
    }
    SECTION("the channel a pending tune is loading comes first") {
        put_nothing_playing(s, 2);
        app::guide_frame(s, frame_facts(4));
        CHECK(s.guide_cursor.row == 4);
    }
    SECTION("SELECT opened it this frame: its cursor stays") {
        input(s, app::GuideEvent::Select);  // opens on 02, the viewer's
        s.guide_cursor = {3, 0, false};     // and a turn in the same batch
        app::guide_frame(s, frame_facts());
        CHECK(same(s.guide_cursor, {3, 0, false}));
    }
#ifdef MEDIA_BROWSER_ENABLED
    SECTION("leaving the Media Browser with nothing playing, not entering it") {
        app::reset_main_ui_for_media_browser(s);
        s.current_screen = app::AppScreen::MediaBrowser;
        s.guide_cursor = {6, 3, false};
        app::guide_frame(s, frame_facts());
        CHECK(same(s.guide_cursor, {6, 3, false}));
        s.current_screen = app::AppScreen::MainMenu;
        app::guide_frame(s, frame_facts());
        CHECK(same(s.guide_cursor, app::guide_open(2, 8)));
    }
#endif
    SECTION("List style: never") {
        auto f = frame_facts();
        f.guide_style = false;
        put_nothing_playing(s, 2);
        app::guide_frame(s, f);
        CHECK(same(s.guide_cursor, {6, 3, false}));
    }
}

TEST_CASE("each frame the cursor is clamped; the List's Picture focus is never set",
          "[guide_nav]") {
    app::AppState s;
    put_playing(s, 1);
    s.ui_visible_when_playing = true;
    app::guide_frame(s, frame_facts());  // on screen already: no edge below
    s.guide_cursor = {0, 0, true};
    s.picture_focus = true;  // left over from the List style

    SECTION("the control drawn: kept") {
        s.picture_control_available = true;
        app::guide_frame(s, frame_facts());
        CHECK(same(s.guide_cursor, {0, 0, true}));
        CHECK_FALSE(s.picture_focus);
    }
    SECTION("a load gap between items: rides through while it may stay") {
        s.picture_control_available = false;
        app::guide_frame(s, frame_facts(std::nullopt, /*may_stay=*/true));
        CHECK(same(s.guide_cursor, {0, 0, true}));
    }
    SECTION("the new video has no control: back to 00") {
        s.picture_control_available = false;
        app::guide_frame(s, frame_facts(std::nullopt, /*may_stay=*/false));
        CHECK(same(s.guide_cursor, {0, 0, false}));
    }
    SECTION("the list shrank under the cursor") {
        s.guide_cursor = {7, 3, false};
        app::guide_frame(s, frame_facts(std::nullopt, false, /*rows=*/3));
        CHECK(same(s.guide_cursor, {2, 0, false}));
    }
    SECTION("List style: neither the cursor nor the List's focus is touched") {
        auto f = frame_facts();
        f.guide_style = false;
        s.guide_cursor = {7, 3, false};
        app::guide_frame(s, f);
        CHECK(same(s.guide_cursor, {7, 3, false}));
        CHECK(s.picture_focus);
    }
}

TEST_CASE("the guide's appearance is a rising edge, once per appearance", "[guide_nav]") {
    app::GuideShownEdge edge;
    CHECK_FALSE(edge.rose(false));
    CHECK(edge.rose(true));        // appears
    CHECK_FALSE(edge.rose(true));  // stays
    CHECK_FALSE(edge.rose(true));
    CHECK_FALSE(edge.rose(false));  // goes
    CHECK_FALSE(edge.rose(false));
    CHECK(edge.rose(true));  // appears again

    SECTION("on screen from the very first frame counts as appearing") {
        app::GuideShownEdge first;
        CHECK(first.rose(true));
    }
}

// ── Painting (main.cpp / Renderer::render) ───────────────────────────────

TEST_CASE("the video goes in the guide's window while the guide is on screen",
          "[guide_nav]") {
    app::AppState s;
    SECTION("over a playing channel: only with the overlay up") {
        put_playing(s);
        CHECK_FALSE(app::guide_window_video(s));
        s.ui_visible_when_playing = true;
        CHECK(app::guide_window_video(s));
    }
    SECTION("a tune's load gap with the guide up: the window (dark until the stream)") {
        put_loading(s);
        s.ui_visible_when_playing = true;
        CHECK(app::guide_window_video(s));
        s.ui_visible_when_playing = false;
        CHECK_FALSE(app::guide_window_video(s));
    }
    SECTION("never the intro, even before its first frame (nothing playing yet)") {
        put_nothing_playing(s);
        s.ui_visible_when_playing = false;
        s.showing_intro_video = true;
        CHECK_FALSE(app::guide_window_video(s));
        s.showing_intro_video = false;
        CHECK(app::guide_window_video(s));
    }
}

TEST_CASE("the guide is painted while on screen and through a Close's fade-out",
          "[guide_nav]") {
    app::AppState s;
    SECTION("on screen: painted, the load gap included") {
        put_loading(s);
        s.ui_visible_when_playing = true;
        CHECK(app::guide_painted(s));
        put_nothing_playing(s);
        s.ui_visible_when_playing = false;
        CHECK(app::guide_painted(s));
    }
    SECTION("a Close over a video: hidden at once, painted until the fade ends") {
        put_playing(s, 3);
        s.ui_visible_when_playing = true;
        s.guide_cursor = {3, 1, false};
        input(s, app::GuideEvent::Select, 0);  // the tuned row: Close
        REQUIRE_FALSE(s.ui_visible_when_playing);
        REQUIRE(s.is_fading);
        CHECK_FALSE(app::guide_visible(s));
        CHECK_FALSE(app::guide_window_video(s));  // the video is back full-screen
        CHECK(app::guide_painted(s));             // the guide fades over it
        s.is_fading = false;                      // the fade ended
        CHECK_FALSE(app::guide_painted(s));
    }
    SECTION("a fade IN with the overlay hidden is not the guide's") {
        put_playing(s);
        s.is_fading = true;
        s.fade_target_ui_visible = true;
        CHECK_FALSE(app::guide_painted(s));
    }
    SECTION("never over the intro") {
        put_nothing_playing(s);
        s.showing_intro_video = true;
        CHECK_FALSE(app::guide_painted(s));
    }
#ifdef MEDIA_BROWSER_ENABLED
    SECTION("never over the Media Browser, a fade included") {
        app::reset_main_ui_for_media_browser(s);
        s.current_screen = app::AppScreen::MediaBrowser;
        s.is_fading = true;
        s.fade_target_ui_visible = false;
        CHECK_FALSE(app::guide_painted(s));
    }
#endif
}

TEST_CASE("the channel last tuned is saved when it changes, checked on a tuned move",
          "[guide_nav]") {
    app::AppState s;
    s.last_channel_id = "chan-a";  // loaded from settings.json
    app::LastChannelWatch watch{s.tuned_channel, s.last_channel_id};
    CHECK_FALSE(watch.changed(s));  // nothing moved

    SECTION("a tune to another channel: saved once") {
        s.tuned_channel = 2;
        s.last_channel_id = "chan-b";
        CHECK(watch.changed(s));
        CHECK_FALSE(watch.changed(s));  // the next frames: nothing new
        CHECK_FALSE(watch.changed(s));
    }
    SECTION("the boot tune to the channel already saved: nothing to save") {
        s.tuned_channel = 1;  // last_channel_id still "chan-a"
        CHECK_FALSE(watch.changed(s));
    }
    SECTION("a rebuild re-anchoring the tuned row: same channel, no save") {
        s.tuned_channel = 1;
        CHECK_FALSE(watch.changed(s));
        s.tuned_channel = 4;  // a reload moved "chan-a" to row 4
        CHECK_FALSE(watch.changed(s));
    }
    SECTION("the id is not compared while tuned_channel stays put") {
        s.last_channel_id = "chan-c";
        CHECK_FALSE(watch.changed(s));
        s.tuned_channel = 3;
        CHECK(watch.changed(s));  // seen at the move
    }
    SECTION("tuned, lost to a reload, tuned again elsewhere: saved") {
        s.tuned_channel = 1;
        CHECK_FALSE(watch.changed(s));
        s.tuned_channel = -1;  // its playlist is gone
        CHECK_FALSE(watch.changed(s));
        s.tuned_channel = 1;  // another playlist now at row 1
        s.last_channel_id = "chan-d";
        CHECK(watch.changed(s));
    }
}
