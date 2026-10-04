// SettingsMenuManager's half of the redraw gate (app/redraw_gate.h): when an
// open Settings menu may skip unchanged frames, and the signature that makes
// it redraw when what it shows changes.
//
// The contract that matters most: the open/close slide only advances inside
// drawing (get_animation_progress() is what clears is_opening_/is_closing_),
// so the menu must NEVER report static while either is set — a skipped
// frame there would freeze the slide (and a close would never finish).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

#include "app/app_state.h"
#include "ui/controller_wizard.h"
#include "ui/settings_menu.h"
#include "ui_test_doubles.h"

using ui::SettingsMenuManager;

namespace {

// Lets the open slide (~250 ms) or close slide (300 ms) finish, then runs
// the one call that advances it — what the renderer does each frame.
void finish_slide(SettingsMenuManager& sm) {
    std::this_thread::sleep_for(std::chrono::milliseconds(320));
    (void)sm.get_animation_progress();
}

}  // namespace

TEST_CASE("Settings redraw: a closed menu is not a static menu",
          "[settings_menu][redraw]") {
    app::AppState state{};
    SettingsMenuManager sm{&state};
    CHECK_FALSE(sm.is_static_for_redraw());
}

TEST_CASE("Settings redraw: the open slide is continuous, the open menu static",
          "[settings_menu][redraw]") {
    app::AppState state{};
    SettingsMenuManager sm{&state};
    sm.open();
    REQUIRE(sm.is_opening());
    CHECK_FALSE(sm.is_static_for_redraw());
    finish_slide(sm);
    REQUIRE_FALSE(sm.is_opening());
    CHECK(sm.is_static_for_redraw());
}

TEST_CASE("Settings redraw: the close slide is continuous until it lands",
          "[settings_menu][redraw]") {
    app::AppState state{};
    SettingsMenuManager sm{&state};
    sm.open();
    finish_slide(sm);
    sm.close();
    REQUIRE(sm.is_closing());
    CHECK_FALSE(sm.is_static_for_redraw());
    finish_slide(sm);
    CHECK_FALSE(sm.is_active());
    CHECK_FALSE(sm.is_static_for_redraw());
}

TEST_CASE("Settings redraw: the controller wizard keeps drawing",
          "[settings_menu][redraw]") {
    ui_test::fake_reset();
    app::AppState state{};
    platform::InputManager im;
    SettingsMenuManager sm{&state};
    sm.open();
    finish_slide(sm);
    sm.open_controller_wizard(&im);
    REQUIRE(sm.is_controller_wizard_active());
    CHECK_FALSE(sm.is_static_for_redraw());
    sm.close_controller_wizard();
    CHECK(sm.is_static_for_redraw());
}

TEST_CASE("Settings redraw: signature tracks cursor and page, nothing else",
          "[settings_menu][redraw]") {
    app::AppState state{};
    SettingsMenuManager sm{&state};
    sm.open();
    finish_slide(sm);

    const uint64_t idle = sm.redraw_signature();
    CHECK(sm.redraw_signature() == idle);  // stable while nothing changes

    sm.navigate(1);
    const uint64_t moved = sm.redraw_signature();
    CHECK(moved != idle);
    sm.navigate(-1);
    CHECK(sm.redraw_signature() == idle);

    sm.enter_submenu(ui::MenuSection::DISPLAY);
    CHECK(sm.redraw_signature() != idle);
    // A row relabel (what update() does on Wi-Fi edges) changes it too.
    const uint64_t display = sm.redraw_signature();
    sm.rebuild_current_submenu();
    CHECK(sm.redraw_signature() == display);  // same labels: no redraw
}
