#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "app/game_launch_recovery.h"

TEST_CASE("all RetroArch exits request display reset and clear playback state",
          "[retroarch][recovery]") {
    app::AppState state{};
    state.reset_display = false;
    state.current_item_index = 4;
    state.current_playlist_index = 2;
    state.video_active = true;
    state.is_loading_game = true;

    app::prepare_kiosk_state_after_game(state);

    REQUIRE(state.reset_display.load());
    REQUIRE(state.current_item_index == -1);
    REQUIRE(state.current_playlist_index == -1);
    REQUIRE_FALSE(state.video_active.load());
    REQUIRE_FALSE(state.is_loading_game.load());
}

TEST_CASE("prepare_loading_state_for_launch resets the launch plate to opaque",
          "[retroarch][recovery]") {
    app::AppState state{};
    state.is_loading_game = false;
    state.loading_alpha.store(0.0f);            // as left by a completed dissolve
    state.post_game_fade_start_ms.store(12345); // in-flight fade from a prior exit

    app::prepare_loading_state_for_launch(state);

    REQUIRE(state.is_loading_game.load());
    REQUIRE(state.loading_alpha.load() == 1.0f);
    REQUIRE(state.post_game_fade_start_ms.load() == 0);
}

TEST_CASE("return_dissolve_alpha ramps 1 to 0, clamped and monotone",
          "[retroarch][recovery]") {
    REQUIRE(app::return_dissolve_alpha(0.0f, 250.0f) == 1.0f);
    REQUIRE(app::return_dissolve_alpha(125.0f, 250.0f) == Catch::Approx(0.5f));
    REQUIRE(app::return_dissolve_alpha(250.0f, 250.0f) == 0.0f);
    REQUIRE(app::return_dissolve_alpha(500.0f, 250.0f) == 0.0f);  // past the end
    REQUIRE(app::return_dissolve_alpha(-50.0f, 250.0f) == 1.0f);  // hold period: pre-ramp elapsed is negative
    REQUIRE(app::return_dissolve_alpha(10.0f, 0.0f) == 0.0f);     // degenerate duration

    float prev = 1.0f;
    for (float t = -120.0f; t <= 500.0f; t += 10.0f) {  // start inside the hold
        const float a = app::return_dissolve_alpha(t, 250.0f);
        REQUIRE(a <= prev);
        REQUIRE(a >= 0.0f);
        REQUIRE(a <= 1.0f);
        prev = a;
    }
}

TEST_CASE("prepare_kiosk_state_after_game requests the fade and leaves "
          "display_mode_restored alone",
          "[retroarch][recovery]") {
    app::AppState state{};
    state.display_mode_restored.store(true);
    state.post_game_fade_start_ms.store(0);

    app::prepare_kiosk_state_after_game(state);

    // -1 = "fade requested"; main.cpp's render loop stamps the real start
    // time on the first frame it actually draws, so the fade covers 250ms of
    // RENDERED frames no matter how long the reset_display work takes.
    REQUIRE(state.post_game_fade_start_ms.load() == -1);

    // The exit path sets display_mode_restored BEFORE this helper runs; the
    // main loop's reset_display handler consumes it AFTER. If this helper
    // ever cleared it, the main loop would re-set_mode and the TV would
    // resync twice again.
    REQUIRE(state.display_mode_restored.load());
}
