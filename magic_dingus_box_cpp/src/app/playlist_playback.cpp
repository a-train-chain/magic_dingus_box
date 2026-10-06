#include "playlist_playback.h"

#include "auto_advance.h"
#include "playback_reset.h"

#include <iostream>

namespace app {

PlaylistPlayback::PlaylistPlayback(AppState& state, PlaylistTransport& transport,
                                   const std::string& playlist_directory)
    : state_(state), transport_(transport), playlist_directory_(playlist_directory) {}

// ── Failed-item handling (main playlist / Master Shuffle) ────────────────

bool PlaylistPlayback::owns_pipeline() const {
    bool owns = state_.current_playlist_index >= 0 &&
                state_.current_item_index >= 0 &&
                state_.intro_complete && !state_.showing_intro_video &&
                !state_.is_switching_playlist && !state_.is_loading_game;
#ifdef MEDIA_BROWSER_ENABLED
    // MB PlaybackScreen owns its own error path (toast + exit).
    owns = owns && state_.current_screen != app::AppScreen::MediaBrowser;
#endif
    return owns;
}

int PlaylistPlayback::failure_budget() const {
    int size = 1;
    if (state_.current_playlist_index >= 0 &&
        state_.current_playlist_index <
            static_cast<int>(state_.playlists.size())) {
        size = static_cast<int>(
            state_.playlists[state_.current_playlist_index].items.size());
    }
    return video::PlaybackErrorPolicy::failure_budget(
        size, state_.master_shuffle_active);
}

void PlaylistPlayback::act_on_failed_item(video::PlaybackErrorPolicy::Decision d,
                                          const char* why) {
    using Decision = video::PlaybackErrorPolicy::Decision;
    if (d == Decision::Advance) {
        std::cerr << "Playlist item " << state_.current_item_index
                  << " failed (" << why << ") — skipping to next item"
                  << std::endl;
        if (state_.master_shuffle_active) {
            // Records the failed item in the PREV history (the virtual
            // row is filtered there), then picks the next random video.
            transport_.master_shuffle_advance(state_, playlist_directory_);
        } else {
            transport_.load_next_item(state_, playlist_directory_);
        }
    } else if (d == Decision::GiveUp) {
        std::cerr << "Playlist item failed (" << why << ") and too many "
                  << "consecutive items failed — stopping playback"
                  << std::endl;
        // Same end state as load_next_item's all-items-failed branch.
        transport_.stop();
        app::stop_to_menu(state_, "Couldn't play these videos");
    }
}

// ── Main-menu input ──────────────────────────────────────────────────────

void PlaylistPlayback::on_select() {
    // Don't allow playlist selection during intro video
    if (state_.showing_intro_video) {
        return;  // Ignore input during intro
    }

    // If video is playing and UI is hidden, just show UI
    if (state_.video_active && !state_.ui_visible_when_playing) {
        state_.ui_visible_when_playing = true;
        state_.ui_visibility_timer = 3.0; // Show for 3 seconds
        return; // Don't trigger selection yet
    }

    // If video is already playing
    if (state_.video_active) {
        // Check if the selected playlist is the same as the one currently playing
        // Special case for Master Shuffle (index 0): current_playlist_index points to the source playlist,
        // so we check master_shuffle_active flag instead.
        bool is_same_playlist = (state_.current_playlist_index == state_.selected_index) ||
                                (state_.master_shuffle_active && state_.selected_index == 0);

        if (is_same_playlist) {
            // Same playlist: just toggle UI visibility with fade
            state_.ui_visible_when_playing = !state_.ui_visible_when_playing;

            // Start fade animation (synchronized UI and audio)
            state_.fade_start_time = std::chrono::steady_clock::now();
            state_.fade_target_ui_visible = state_.ui_visible_when_playing;
            state_.is_fading = true;
        } else {
            // Different playlist: stop current and start new playlist
            // Prevent overlapping playlist switches
            if (state_.is_switching_playlist) {
                return;  // Skip if already switching
            }

            state_.is_switching_playlist = true;  // Set flag to prevent overlapping operations
            state_.playlist_switch_start_time = std::chrono::steady_clock::now();  // Track when switch started

            // First, update the playlist index BEFORE stopping to prevent reset
            state_.current_playlist_index = state_.selected_index;
            state_.current_item_index = 0;

            // Reset advance flags when switching playlists to prevent issues
            state_.last_advanced_item_index = -1;
            state_.last_advanced_duration = 0.0;
            // No volume reset here: nothing dims the stream any more,
            // and forcing 100% blasted the outgoing video at full
            // level until stop() landed. load_file re-applies the
            // user's volume to the new stream.

            transport_.stop();
            // Wait longer to ensure stop completes and buffers are released
            // Increased delay to prevent race conditions and buffer export errors
            // The DRM driver needs time to release GEM buffers from previous video
            transport_.sleep_for(std::chrono::milliseconds(200));

            // Verify that mpv actually stopped before proceeding
            // This prevents race conditions when loading new videos
            int retry_count = 0;
            const int max_retries = 10;
            while (transport_.is_playing() && retry_count < max_retries) {
                transport_.sleep_for(std::chrono::milliseconds(50));
                retry_count++;
            }

            if (transport_.is_playing()) {
                std::cerr << "Warning: Video did not stop cleanly after " << (max_retries * 50) << "ms, proceeding anyway" << std::endl;
            }

            // Then start the new playlist
            bool load_success = false;

            // Check for Master Shuffle (index 0)
            if (state_.selected_index == 0) {
                std::cout << "Master Shuffle selected!" << std::endl;
                state_.master_shuffle_active = true;
                transport_.play_random_global_video(state_, playlist_directory_);
                load_success = true; // Assume success for now (play_random_global_video handles retries)

                // When starting video, hide UI completely so video shows through fully
                state_.ui_visible_when_playing = false;
            } else if (!state_.playlists.empty() && state_.selected_index < static_cast<int>(state_.playlists.size())) {
                state_.master_shuffle_active = false; // Disable master shuffle for normal playlists
                const auto& pl = state_.playlists[state_.selected_index];
                if (!pl.items.empty() && pl.is_video_playlist()) {
                    // Load first item of new playlist
                    auto load_result = transport_.load_playlist_item(state_, pl, 0, playlist_directory_);
                    load_success = static_cast<bool>(load_result);
                    if (!load_result) {
                        std::cerr << "Failed to load playlist item: " << load_result.error() << std::endl;
                        state_.set_error("Could not load: " + pl.title);
                    }
                    if (load_success) {
                        // Only hide UI when video actually loaded
                        state_.ui_visible_when_playing = false;
                    }
                } else if (!pl.items.empty() && pl.is_game_playlist()) {
                    // Game playlists are launched from Settings > Video Games
                    state_.set_error("Use Settings to launch games");
                    load_success = false;
                } else {
                    state_.set_error("No content in playlist");
                    load_success = false;
                }
            }

            // If load failed, the old video is already stopped:
            // land on the menu. stop_to_menu, not the three flags
            // this used to clear — current_playlist_index /
            // current_item_index were set to the NEW playlist above,
            // and indexes set with no video is the Renderer's
            // "between items" early-out, so the menu AND the error
            // banner just raised stayed blank until another press.
            // (An empty message leaves that banner in place.)
            if (!load_success) {
                app::stop_to_menu(state_);
                std::cerr << "Playlist switch failed - flag cleared, ready for retry" << std::endl;
            }
            // Otherwise, the flag will be cleared when the new video becomes active
            // If video doesn't become active within timeout, flag will be cleared by timeout mechanism
        }
    } else {
        // No video playing: start the selected playlist
        // Prevent overlapping playlist switches
        if (state_.is_switching_playlist) {
            return;  // Skip if already switching
        }

        // Check for Master Shuffle (index 0)
        if (state_.selected_index == 0) {
            std::cout << "Master Shuffle selected (from stopped)!" << std::endl;
            state_.is_switching_playlist = true;
            state_.playlist_switch_start_time = std::chrono::steady_clock::now();
            state_.master_shuffle_active = true;

            transport_.play_random_global_video(state_, playlist_directory_);

            // When starting video, hide UI completely so video shows through fully
            state_.ui_visible_when_playing = false;

            // Note: play_random_global_video handles loading, but doesn't return success/fail
            // We assume it works or retries.
        } else if (!state_.playlists.empty() && state_.selected_index < static_cast<int>(state_.playlists.size())) {
            state_.master_shuffle_active = false; // Disable master shuffle for normal playlists
            const auto& pl = state_.playlists[state_.selected_index];
            if (!pl.items.empty() && pl.is_video_playlist()) {
                state_.is_switching_playlist = true;  // Set flag
                state_.playlist_switch_start_time = std::chrono::steady_clock::now();

                // Load first item of playlist
                auto load_result = transport_.load_playlist_item(state_, pl, 0, playlist_directory_);
                if (load_result) {
                    // Track which playlist and item is playing
                    state_.current_playlist_index = state_.selected_index;
                    state_.current_item_index = 0;
                    // When starting video, hide UI completely so video shows through fully
                    state_.ui_visible_when_playing = false;
                } else {
                    // Load failed - clear flag and show error
                    std::cerr << "Failed to load playlist item: " << load_result.error() << std::endl;
                    state_.set_error("Could not load: " + pl.title);
                    state_.is_switching_playlist = false;
                }
            } else if (!pl.items.empty() && pl.is_game_playlist()) {
                state_.set_error("Use Settings to launch games");
            } else {
                state_.set_error("No content in playlist");
            }
        }
    }
}

void PlaylistPlayback::on_play_pause() {
    // Only allow play/pause if intro is complete and we have an active video
    if (state_.intro_complete && state_.video_active) {
        transport_.toggle_pause();
    }
}

void PlaylistPlayback::on_next() {
    // If video is playing, advance to next playlist item
    // In Master Shuffle mode, pick another random video
    // Otherwise, seek forward in current video
    // Don't allow if we're switching playlists
    if (!state_.is_switching_playlist && state_.video_active && state_.current_playlist_index >= 0) {
        if (state_.master_shuffle_active) {
            // In Master Shuffle, NEXT triggers another random video,
            // saving the current one to the shuffle history for
            // "Previous". Index 0 is the VIRTUAL Master Shuffle row
            // (one dummy item, no file), never a real source
            // playlist — the reload path parks
            // current_playlist_index there when a source playlist is
            // deleted mid-playback; record_history refuses it (see
            // app/shuffle_queue.h for why recording it stalled
            // auto-advance).
            transport_.master_shuffle_advance(state_, playlist_directory_);
        } else {
            transport_.load_next_item(state_, playlist_directory_);
        }
    } else if (!state_.is_switching_playlist && state_.video_active) {
        // Only seek if we have an active video (not intro)
        transport_.seek(10.0);
    }
}

void PlaylistPlayback::on_prev() {
    // If video is playing, go to previous playlist item
    // In Master Shuffle mode, pick another random video
    // Otherwise, seek backward in current video
    // Don't allow if we're switching playlists
    if (!state_.is_switching_playlist && state_.video_active && state_.current_playlist_index >= 0) {
        if (state_.master_shuffle_active) {
            // In Master Shuffle, PREV goes back through shuffle
            // history (random pick when it is empty).
            transport_.master_shuffle_back(state_, playlist_directory_);
        } else {
            transport_.load_previous_item(state_, playlist_directory_);
        }
    } else if (!state_.is_switching_playlist && state_.video_active) {
        // Only seek if we have an active video (not intro)
        transport_.seek(-10.0);
    }
}

// ── Per-frame ticks ──────────────────────────────────────────────────────

void PlaylistPlayback::tick_switch_timeout() {
    // Clear playlist switching flag if it's been stuck for too long (timeout safety)
    // This prevents the flag from getting stuck if video fails to load or gets into bad state
    if (state_.is_switching_playlist) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state_.playlist_switch_start_time);
        if (elapsed.count() > 2000) {  // 2 second timeout
            std::cerr << "CRITICAL: Playlist switch timeout after " << elapsed.count() << "ms - clearing flag and resetting state" << std::endl;
            std::cerr << "  Debug info: video_active=" << state_.video_active
                      << ", is_playing=" << transport_.is_playing()
                      << ", current_playlist=" << state_.current_playlist_index
                      << ", current_item=" << state_.current_item_index << std::endl;
            state_.is_switching_playlist = false;

            // Also reset video state to ensure clean recovery
            if (!transport_.is_playing() && !state_.video_active) {
                std::cerr << "MPV appears stuck - attempting recovery by stopping and clearing state" << std::endl;
                transport_.stop();
                transport_.sleep_for(std::chrono::milliseconds(200));
            }
        }
    }
}

void PlaylistPlayback::tick_pipeline_error() {
    // Pipeline error on a playlist item -> skip it (or give up after a
    // run of failures). Not gated on video_active: an error before
    // preroll leaves duration 0, so video_active never turned on.
    if (owns_pipeline()) {
        const auto decision = error_policy_.on_frame(
            transport_.stream_generation(), transport_.has_error(),
            transport_.player_position(), failure_budget());
        if (decision != video::PlaybackErrorPolicy::Decision::None) {
            act_on_failed_item(decision, "pipeline error");
        }
    }
}

void PlaylistPlayback::tick_auto_advance() {
    // Auto-advance to next item in playlist when current video ends.
    // "Ends" honors the item's `end:` trim (decide_auto_advance) — the
    // old full-file comparison played every trimmed item to EOF.
    if (state_.video_active && state_.current_playlist_index >= 0 && state_.current_item_index >= 0) {
        // Snapshot the (position, duration) pair so the whole advance
        // decision sees a consistent view rather than reading the
        // mutex-protected fields multiple times.
        app::AutoAdvanceInput adv;
        adv.position = state_.get_position();
        adv.duration = state_.get_duration();
        if (state_.current_playlist_index < static_cast<int>(state_.playlists.size())) {
            const auto& adv_pl = state_.playlists[state_.current_playlist_index];
            if (state_.current_item_index < static_cast<int>(adv_pl.items.size())) {
                adv.item_end = adv_pl.items[state_.current_item_index].end;
            }
        }
        adv.playback_started = state_.playback_started_;
        adv.master_shuffle = state_.master_shuffle_active;
        adv.current_item = state_.current_item_index;
        adv.last_advanced_item = state_.last_advanced_item_index;

        switch (app::decide_auto_advance(adv)) {
            case app::AutoAdvance::Advance:
                std::cout << "Auto-advancing from item " << state_.current_item_index
                          << " at position " << adv.position << "/"
                          << app::effective_end(adv.duration, adv.item_end) << std::endl;
                // Set flag BEFORE calling load_next_item to prevent race
                // conditions. The REAL duration: update_state compares it
                // to the new file's duration to detect the next item loaded.
                state_.last_advanced_item_index = state_.current_item_index;
                state_.last_advanced_duration = adv.duration;
                // Note: load_next_item handles errors internally (skips broken files)
                if (state_.master_shuffle_active) {
                    // Saves the current item to the shuffle history first
                    // (virtual row excluded — see on_next()).
                    transport_.master_shuffle_advance(state_, playlist_directory_);
                } else {
                    transport_.load_next_item(state_, playlist_directory_);
                }
                break;
            case app::AutoAdvance::Held:
                if (!state_.master_shuffle_active) {
                    std::cout << "NOT auto-advancing: item=" << state_.current_item_index
                              << ", last_advanced=" << state_.last_advanced_item_index
                              << ", playback_started=" << state_.playback_started_ << std::endl;
                }
                break;
            case app::AutoAdvance::ResetGuard:
                // Well away from the end: re-arm the once-per-item guard,
                // so auto-advance works even after manual navigation.
                // Master Shuffle stays active - only exits when user selects a different playlist
                state_.last_advanced_item_index = -1;
                state_.last_advanced_duration = 0.0;
                break;
            case app::AutoAdvance::None:
                break;
        }
    }
}

void PlaylistPlayback::tick_stall_watchdog(double now_sec) {
    // Catches the pipeline silently stalling while the kiosk still
    // believes it is playing. Seen live 2026-07-29: a playlist-switch
    // timeout restored the "playing" flags but left GStreamer PAUSED
    // (its PulseAudio stream read `Corked: yes` against the kiosk's
    // `is_paused = false`), position sat at 0.00 for seven hours, and
    // nothing detected it. Logic and thresholds live in
    // app::PlaybackStallWatchdog — see tests/app/test_playback_stall.cpp.
    //
    // A new stream (every load_file bumps the generation) starts at
    // 0.0, which must not read as "frozen at 0.0" from the previous
    // item's baseline — and its escalation count starts fresh.
    if (transport_.stream_generation() != watchdog_generation_) {
        watchdog_generation_ = transport_.stream_generation();
        stall_watchdog_.reset();
    }
    const bool expect_playing =
        state_.video_active && transport_.is_playing() &&
        !transport_.is_paused() && !state_.is_loading_game &&
        !state_.showing_intro_video && !state_.is_switching_playlist;
    const auto wd_action = stall_watchdog_.update(
        expect_playing, state_.get_position(), now_sec);
    if (wd_action == app::PlaybackStallWatchdog::Action::Advance &&
        owns_pipeline()) {
        // Repeated play() never got position moving: skip the item,
        // counted in the same failure run as pipeline errors.
        std::cerr << "Playback still stalled at "
                  << state_.get_position() << "s after "
                  << app::PlaybackStallWatchdog::kMaxRecoveriesBeforeAdvance
                  << " restarts — giving up on this item" << std::endl;
        act_on_failed_item(
            error_policy_.on_failure(
                transport_.stream_generation(), failure_budget()),
            "stalled");
    } else if (wd_action != app::PlaybackStallWatchdog::Action::None) {
        // Recover — or Advance where there is no playlist to
        // advance (Media Browser playback): one more restart.
        std::cerr << "Playback stalled at " << state_.get_position()
                  << "s while reported playing — restarting playback"
                  << std::endl;
        transport_.play();
    }
}

}  // namespace app
