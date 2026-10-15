#include "intro_sequence.h"

#include <algorithm>

namespace app {

bool intro_video_ended(double position, double duration, bool is_playing) {
    // Check if intro video has ended
    // Use multiple conditions to ensure reliable detection
    bool video_ended = false;

    // Primary check: position near end (tight margin so video plays fully)
    if (position >= duration - 0.05) {
        video_ended = true;
    }

    // Fallback check: if video stopped playing but we're still showing intro
    if (!is_playing && duration > 0.0 && position > 0.0) {
        video_ended = true;
    }
    return video_ended;
}

double intro_fade_out_volume(double original_volume,
                             std::chrono::milliseconds elapsed) {
    // Fade-out in progress - interpolate volume from 100% to 0%
    float fade_progress = static_cast<float>(elapsed.count()) /
                          static_cast<float>(kIntroFadeOut.count());
    fade_progress = std::min(1.0f, std::max(0.0f, fade_progress));  // Clamp to [0, 1]

    // Fade volume from original_volume to 0
    return original_volume * (1.0 - fade_progress);
}

void hand_intro_to_menu(AppState& state,
                        std::chrono::steady_clock::time_point now) {
    // Force video_active to false immediately (don't wait for update_state)
    state.video_active = false;
    state.update_playback_state(0.0, 0.0);
    state.current_playlist_index = -1;
    state.current_item_index = -1;

    // Start fade-in animation for UI (from transparent to visible)
    // Since there's no video active after intro, we fade in the UI
    state.fade_start_time = now;
    state.fade_target_ui_visible = true;  // Fade to visible
    state.is_fading = true;
}

bool should_render_video(const AppState& state, bool player_is_playing) {
    if (!state.intro_complete) {
        // During intro phase: render intro video
        return (state.video_active || state.showing_intro_video || player_is_playing) &&
               !state.intro_fading_out;
    }
    // After intro: render regular videos when active or switching playlists
    return state.video_active || (state.is_switching_playlist && player_is_playing);
}

}  // namespace app
