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
    // Controller::update_state's stop-clear can't cover these: callers force
    // video_active false themselves, so it never sees the true->false edge.
    state.now_playing_title.clear();
    state.now_playing_subtitle.clear();
    state.now_playing_kind.clear();
    state.current_playlist_name.clear();
    state.current_item_count = 0;
    if (!error_message.empty()) {
        state.set_error(error_message);
    }
}

}  // namespace app
