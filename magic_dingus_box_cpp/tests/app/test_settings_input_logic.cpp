// app/settings_input_logic.h — the Settings-menu input decisions moved out
// of main.cpp's event loop (main.cpp is in no test target).

#include <catch2/catch_test_macros.hpp>

#include "app/settings_input_logic.h"

using namespace std::chrono_literals;
using app::GameBrowserSelect;
using app::GameListSelect;
using app::MenuButtonHold;

namespace {
const auto t0 = std::chrono::steady_clock::time_point{} + 1000s;
}

TEST_CASE("MenuButtonHold: short press toggles Settings", "[settings_input]") {
    MenuButtonHold h;
    h.press(t0);
    CHECK(h.button_held);
    CHECK_FALSE(h.volume_changed_while_held);
    CHECK(h.release(t0 + 120ms) == MenuButtonHold::Release::ToggleSettings);
    CHECK_FALSE(h.button_held);
}

TEST_CASE("MenuButtonHold: a long press without a volume change does nothing",
          "[settings_input]") {
    MenuButtonHold h;
    h.press(t0);
    // The threshold is exclusive on release: 300 ms is already "long".
    CHECK(h.release(t0 + 300ms) == MenuButtonHold::Release::Nothing);
    h.press(t0);
    CHECK(h.release(t0 + 299ms) == MenuButtonHold::Release::ToggleSettings);
}

TEST_CASE("MenuButtonHold: any volume change saves instead of toggling",
          "[settings_input]") {
    MenuButtonHold h;
    h.press(t0);
    h.volume_changed_while_held = true;
    // Even a quick flick of the knob during a short press must not open
    // Settings — the press was used as a volume modifier.
    CHECK(h.release(t0 + 50ms) == MenuButtonHold::Release::SaveVolume);
    h.press(t0);
    h.volume_changed_while_held = true;
    CHECK(h.release(t0 + 5s) == MenuButtonHold::Release::SaveVolume);
}

TEST_CASE("MenuButtonHold: a new press clears the previous hold's volume flag",
          "[settings_input]") {
    MenuButtonHold h;
    h.press(t0);
    h.volume_changed_while_held = true;
    (void)h.release(t0 + 400ms);
    h.press(t0 + 2s);
    CHECK_FALSE(h.volume_changed_while_held);
    CHECK(h.release(t0 + 2s + 100ms) == MenuButtonHold::Release::ToggleSettings);
}

TEST_CASE("MenuButtonHold: slider appears only after MORE than 300 ms held",
          "[settings_input]") {
    MenuButtonHold h;
    CHECK_FALSE(h.slider_due(t0 + 10s));  // not held
    h.press(t0);
    CHECK_FALSE(h.slider_due(t0 + 299ms));
    CHECK_FALSE(h.slider_due(t0 + 300ms));
    CHECK(h.slider_due(t0 + 301ms));
    (void)h.release(t0 + 400ms);
    CHECK_FALSE(h.slider_due(t0 + 500ms));
}

TEST_CASE("volume_after_hold_step: 5% per step, vertical inverted, clamped",
          "[settings_input]") {
    CHECK(app::volume_after_hold_step(50, +1, false) == 55);
    CHECK(app::volume_after_hold_step(50, -1, false) == 45);
    // Vertical axis: up (-1) is louder.
    CHECK(app::volume_after_hold_step(50, -1, true) == 55);
    CHECK(app::volume_after_hold_step(50, +1, true) == 45);
    // Multi-detent deltas scale.
    CHECK(app::volume_after_hold_step(50, +3, false) == 65);
    // Clamped at both ends.
    CHECK(app::volume_after_hold_step(98, +1, false) == 100);
    CHECK(app::volume_after_hold_step(2, -1, false) == 0);
    CHECK(app::volume_after_hold_step(100, -4, true) == 100);
}

TEST_CASE("classify_game_list_select: games, then Back, else invalid",
          "[settings_input]") {
    CHECK(app::classify_game_list_select(0, 3) == GameListSelect::Launch);
    CHECK(app::classify_game_list_select(2, 3) == GameListSelect::Launch);
    CHECK(app::classify_game_list_select(3, 3) == GameListSelect::Back);
    CHECK(app::classify_game_list_select(4, 3) == GameListSelect::Invalid);
    CHECK(app::classify_game_list_select(-1, 3) == GameListSelect::Invalid);
    // An empty list has only its Back row.
    CHECK(app::classify_game_list_select(0, 0) == GameListSelect::Back);
}

TEST_CASE("classify_game_browser_select: playlists, then Back, else nothing",
          "[settings_input]") {
    CHECK(app::classify_game_browser_select(0, 2) == GameBrowserSelect::EnterPlaylist);
    CHECK(app::classify_game_browser_select(1, 2) == GameBrowserSelect::EnterPlaylist);
    CHECK(app::classify_game_browser_select(2, 2) == GameBrowserSelect::ExitBrowser);
    CHECK(app::classify_game_browser_select(3, 2) == GameBrowserSelect::Nothing);
    CHECK(app::classify_game_browser_select(-1, 2) == GameBrowserSelect::Nothing);
    CHECK(app::classify_game_browser_select(0, 0) == GameBrowserSelect::ExitBrowser);
}
