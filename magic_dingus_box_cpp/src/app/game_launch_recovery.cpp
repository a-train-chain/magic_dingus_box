#include "game_launch_recovery.h"

namespace app {

void prepare_kiosk_state_after_game(AppState& state) {
    state.reset_display = true;
    state.current_item_index = -1;
    state.current_playlist_index = -1;
    state.video_active = false;
    state.is_loading_game = false;
    // Request the menu fade-up. -1 is the "requested" sentinel; main.cpp's
    // render loop stamps the real start time on the first frame it draws.
    // Requested HERE and not in the game-session exit hook: this helper runs
    // only after a genuine display handover, while the hook also fires on
    // validation early-returns where the menu never left the screen.
    state.post_game_fade_start_ms.store(-1);
}

void prepare_loading_state_for_launch(AppState& state) {
    state.is_loading_game = true;
    state.loading_alpha.store(1.0f);
    state.post_game_fade_start_ms.store(0);   // cancel any in-flight fade
}

float return_dissolve_alpha(float elapsed_ms, float duration_ms) {
    if (duration_ms <= 0.0f) return 0.0f;
    const float a = 1.0f - elapsed_ms / duration_ms;
    if (a < 0.0f) return 0.0f;
    if (a > 1.0f) return 1.0f;
    return a;
}

}  // namespace app
