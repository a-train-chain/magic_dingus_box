#include <catch2/catch_test_macros.hpp>

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
