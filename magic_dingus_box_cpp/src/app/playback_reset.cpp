#include "playback_reset.h"

namespace app {

void stop_to_menu(AppState& state, const std::string& error_message) {
    state.video_active = false;
    state.is_switching_playlist = false;
    state.update_playback_state(0.0, 0.0);
    // The renderer's is_transitioning test is exactly these two being >= 0.
    state.current_playlist_index = -1;
    state.current_item_index = -1;
    // A fade toward hidden that outlives the video zeroes the menu's alpha.
    state.is_fading = false;
    state.ui_visible_when_playing = true;
    state.last_advanced_item_index = -1;
    state.last_advanced_duration = 0.0;
    clear_now_playing(state);
    if (!error_message.empty()) {
        state.set_error(error_message);
    }
}

void reset_main_ui_for_media_browser(AppState& state) {
    state.video_active = false;
    state.is_switching_playlist = false;
    state.current_playlist_index = -1;
    state.current_item_index = -1;
    state.is_fading = false;
    state.ui_visible_when_playing = false;
}

void clear_now_playing(AppState& state) {
    state.now_playing_title.clear();
    state.now_playing_subtitle.clear();
    state.now_playing_kind.clear();
    state.current_playlist_name.clear();
    state.current_item_count = 0;
}

}  // namespace app
