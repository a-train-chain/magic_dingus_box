#include "game_launch_recovery.h"

namespace app {

void prepare_kiosk_state_after_game(AppState& state) {
    state.reset_display = true;
    state.current_item_index = -1;
    state.current_playlist_index = -1;
    state.video_active = false;
    state.is_loading_game = false;
}

}  // namespace app
