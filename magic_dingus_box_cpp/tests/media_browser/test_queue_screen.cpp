// Unit tests for QueueScreen's pure cursor-navigation helper.
//
// Same deal as test_release_picker.cpp: the screen's .cpp translation
// unit needs the GL Renderer and is kiosk-only, so we exercise only the
// static header-inline helper. step_cursor() is the single decision
// point handle_input() uses for row navigation, so these assertions
// cover the real dispatch semantics — in particular that the rotary
// encoder's ROTATE walks the queue list exactly like D-pad
// ROTATE_VERTICAL (controller-free enclosures have no other way to
// reach a row).

#include <catch2/catch_test_macros.hpp>
#include "media_browser/ui/queue_screen.h"

namespace mbu = media_browser::ui;
using platform::InputAction;

TEST_CASE("step_cursor walks rows on rotary ROTATE and D-pad ROTATE_VERTICAL",
          "[queue][nav]") {
    for (auto action : {InputAction::ROTATE, InputAction::ROTATE_VERTICAL}) {
        auto down = mbu::QueueScreen::step_cursor(action, +1, /*cursor=*/1,
                                                  /*count=*/5);
        REQUIRE(down.is_nav);
        REQUIRE(down.cursor == 2);

        auto up = mbu::QueueScreen::step_cursor(action, -1, /*cursor=*/1,
                                                /*count=*/5);
        REQUIRE(up.is_nav);
        REQUIRE(up.cursor == 0);
    }
}

TEST_CASE("step_cursor clamps at both list ends", "[queue][nav]") {
    auto top = mbu::QueueScreen::step_cursor(InputAction::ROTATE, -1,
                                             /*cursor=*/0, /*count=*/5);
    REQUIRE(top.is_nav);
    REQUIRE(top.cursor == 0);

    auto bottom = mbu::QueueScreen::step_cursor(InputAction::ROTATE_VERTICAL,
                                                +1, /*cursor=*/4, /*count=*/5);
    REQUIRE(bottom.is_nav);
    REQUIRE(bottom.cursor == 4);
}

TEST_CASE("step_cursor ignores non-navigation actions and zero deltas",
          "[queue][nav]") {
    for (auto action : {InputAction::SELECT, InputAction::PREV,
                        InputAction::NEXT, InputAction::SETTINGS_MENU}) {
        auto r = mbu::QueueScreen::step_cursor(action, +1, /*cursor=*/2,
                                               /*count=*/5);
        REQUIRE(!r.is_nav);
        REQUIRE(r.cursor == 2);
    }

    auto still = mbu::QueueScreen::step_cursor(InputAction::ROTATE, 0,
                                               /*cursor=*/2, /*count=*/5);
    REQUIRE(!still.is_nav);
    REQUIRE(still.cursor == 2);
}

TEST_CASE("step_cursor consumes navigation on an empty list without moving",
          "[queue][nav]") {
    // Empty queue: the event is still navigation (the screen swallows it
    // rather than letting it fall through), but the cursor stays put.
    auto r = mbu::QueueScreen::step_cursor(InputAction::ROTATE, +1,
                                           /*cursor=*/0, /*count=*/0);
    REQUIRE(r.is_nav);
    REQUIRE(r.cursor == 0);
}
