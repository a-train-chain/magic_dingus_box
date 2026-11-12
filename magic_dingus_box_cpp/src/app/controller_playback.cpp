// controller_playback.cpp
// The Controller's playback state and playlist stepping: the player
// pass-throughs, update_state() (the video_active derivation and its
// debounce), and NEXT / PREV / Master Shuffle stepping with the load-next
// retry loop. Lives in a separate TU (like controller_text_input.cpp) so the
// app unit-test target can link it against a fake video::VideoPlayer: it
// talks to the player only through the abstract interface and has no
// GStreamer / GL / DRM / RetroArch-launcher dependency. The media half of
// load_playlist_item (stop, settle, load, play, and the game hand-off) is
// Controller::load_item_media in controller.cpp, which is kiosk-only.
// Tests: tests/app/test_controller_playback.cpp.

#include "controller.h"
#include "playback_reset.h"
#include "auto_advance.h"
#include "playlist_cursor.h"
#include "shuffle_queue.h"
#include "../video/video_player.h"
#include "../utils/path_resolver.h"
#include "../utils/subprocess.h"
#include "app_state.h"

#include <sstream>
#include <vector>
#include <iomanip>
#include <iostream>
#include <filesystem>
#include <chrono>
#include <algorithm>

namespace fs = std::filesystem;

namespace app {

Controller::Controller(video::VideoPlayer* player)
    : player_(player), display_(nullptr), input_manager_(nullptr)
{
}

void Controller::load_file(const std::string& path, double start, double end, bool loop) {
    if (player_) {
        player_->load_file(path, start, end, loop);
        // Re-apply current volume to player
        player_->set_volume(current_system_volume_);
    }
}

void Controller::set_system_volume(int percent) {
    // Clamp percentage
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    current_system_volume_ = percent;

    // The software volume applies immediately (cheap — no subprocess), so
    // the audible level and the slider track every detent. The amixer
    // fork+exec pair is DEFERRED to the next update_state() tick: it costs
    // ~10-30ms per invocation on a loaded Pi 4 and used to run twice per
    // rotary event on the render thread — a fast spin queued several
    // events in one frame and stalled rendering >100ms exactly while the
    // user watched the volume slider animate.
    if (player_) {
        player_->set_volume(percent);
    }
    pending_system_volume_ = percent;
}

void Controller::apply_system_volume_now(int percent) {
    std::string pct = std::to_string(percent) + "%";

    // Bounded: this runs from update_state() on the render thread, and an
    // amixer stuck on a wedged ALSA control must cost a frame hiccup, not
    // the systemd watchdog.
    auto run_amixer = [](const std::string& control, const std::string& pct_str) -> int {
        return utils::subprocess::run({"amixer", "sset", control, pct_str},
                                      std::chrono::milliseconds(2000)).exit_code;
    };

    int ret_master = run_amixer("Master", pct);
    int ret_pcm = run_amixer("PCM", pct);

    if (ret_master != 0 && ret_pcm != 0) {
        std::cerr << "Warning: Failed to set system volume (amixer Master/PCM both failed)" << std::endl;
    }
}

utils::Result<> Controller::load_file_with_resolution(const std::string& path, const std::string& playlist_dir, double start, double end, bool loop) {
    if (!player_) {
        return utils::Result<>::fail("Player not initialized");
    }

    std::string resolved_path = utils::resolve_video_path(path, playlist_dir);

    // Check if file exists before trying to load
    fs::path file_path(resolved_path);
    if (!fs::exists(file_path)) {
        std::string error = "Video file does not exist: " + resolved_path;
        std::cerr << "ERROR: " << error << std::endl;
        return utils::Result<>::fail(error);
    }

    bool success = player_->load_file(resolved_path, start, end, loop);
    if (!success) {
        std::string error = "Failed to load video file: " + resolved_path;
        std::cerr << "ERROR: " << error << std::endl;
        return utils::Result<>::fail(error);
    }

    // Re-apply current volume to player
    player_->set_volume(current_system_volume_);

    return utils::Result<>::ok();
}

void Controller::play() {
    if (player_) {
        player_->play();
    }
}

void Controller::pause() {
    if (player_) {
        player_->pause();
    }
}

void Controller::toggle_pause() {
    if (player_) {
        player_->toggle_pause();
    }
}

void Controller::seek(double seconds) {
    if (player_) {
        player_->seek(seconds);
    }
}

void Controller::seek_absolute(double timestamp) {
    if (player_) {
        player_->seek_absolute(timestamp);
    }
}


void Controller::stop() {
    if (player_) {
        player_->stop();
    }
}

bool Controller::is_playing() const {
    return player_ ? player_->is_playing() : false;
}

bool Controller::is_paused() const {
    return player_ ? player_->is_paused() : false;
}

double Controller::get_position() const {
    return player_ ? player_->get_position() : 0.0;
}

double Controller::get_duration() const {
    return player_ ? player_->get_duration() : 0.0;
}

std::string Controller::status_text() const {
    if (!player_) {
        return "No player";
    }
    
    double pos = get_position();
    double dur = get_duration();
    
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    
    int pos_min = static_cast<int>(pos / 60);
    int pos_sec = static_cast<int>(pos) % 60;
    oss << pos_min << ":" << std::setfill('0') << std::setw(2) << pos_sec;
    
    if (dur > 0) {
        int dur_min = static_cast<int>(dur / 60);
        int dur_sec = static_cast<int>(dur) % 60;
        oss << " / " << dur_min << ":" << std::setfill('0') << std::setw(2) << dur_sec;
    }
    
    return oss.str();
}

void Controller::set_volume(double volume) {
    if (player_) {
        player_->set_volume(volume);
    }
}

double Controller::get_volume() const {
    if (player_) {
        return player_->get_volume();
    }
    return 100.0;
}

namespace {

// The PlaylistItem currently playing, or nullptr if the indices do not point at
// one. Both indices are free-running ints that are legitimately -1 between
// items and during a playlist switch, so every access needs the same bounds
// check used further down in this file rather than trusting them.
const PlaylistItem* current_item(const AppState& state) {
    if (state.current_playlist_index < 0 ||
        state.current_playlist_index >= static_cast<int>(state.playlists.size())) {
        return nullptr;
    }
    const auto& pl = state.playlists[state.current_playlist_index];
    if (state.current_item_index < 0 ||
        state.current_item_index >= static_cast<int>(pl.items.size())) {
        return nullptr;
    }
    return &pl.items[state.current_item_index];
}

}  // namespace

void Controller::update_state(AppState& state) {
    if (!player_) {
        return;
    }

    // Flush any deferred system-volume change: one amixer pair per frame
    // maximum, identical values skipped. Sits after the player_ guard so
    // unit tests with a null player never fork subprocesses.
    if (pending_system_volume_ >= 0) {
        if (pending_system_volume_ != last_applied_system_volume_) {
            apply_system_volume_now(pending_system_volume_);
            last_applied_system_volume_ = pending_system_volume_;
        }
        pending_system_volume_ = -1;
    }

    state.update_playback_state(get_position(), get_duration());
    state.paused = is_paused();

    // Snapshot for the rest of this frame so we don't repeatedly take the lock
    // and so we operate on a consistent (position, duration) pair.
    const double cur_position = state.get_position();
    const double cur_duration = state.get_duration();

    // Check if video is actually playing (mpv might report playing even before duration is available)
    bool mpv_playing = is_playing();

    // Only set video_active to true if a file is actually loaded (duration > 0)
    // AND either it's playing or has a position > 0
    // During playlist switches, allow video_active to become true when new video loads
    // but prevent it from being set to false prematurely
    //
    // at_eos() gate: after natural end-of-stream the pipeline still
    // reports a position > 0, which reads exactly like "paused at end" —
    // so the (position > 0) arm kept video_active latched true forever
    // and the Media Browser's natural-end detector (edge on video_active
    // true→false) could never fire. A seek out of EOS or a new load
    // clears the latch, so scrubbing back from the end still works.
    bool was_active = state.video_active;
    bool should_be_active = (cur_duration > 0.0) && !player_->at_eos()
                            && (mpv_playing || (cur_position > 0.0));

    // Debounce single-frame negatives. Once video_active is true, a
    // transient should_be_active=false (GStreamer reporting PAUSED for
    // one tick during a seek, or a position/duration query failing) is
    // not enough to flip video_active off. We require the negative read
    // to persist across multiple ticks (kVideoActiveNegativeFrames).
    // This was added because seek bar visibility and video render were
    // both gated on video_active, and the flicker made them disappear
    // during scrubbing. EOS still works correctly: the video_ended
    // detection below fires on position >= duration regardless of
    // video_active state, and a real stop()/load() resets duration to 0
    // which is sticky-false here.
    constexpr int kVideoActiveNegativeFrames = 4;
    if (should_be_active) {
        video_active_negative_count_ = 0;
    } else if (was_active) {
        ++video_active_negative_count_;
    }

    if (state.is_switching_playlist) {
        // During switch: only allow video_active to become true (new video loaded)
        // Don't allow it to become false (would interfere with switch detection)
        if (should_be_active) {
            state.video_active = true;
        }
        // If should_be_active is false, keep current state (don't change it)
    } else if (was_active && !should_be_active
               && video_active_negative_count_ < kVideoActiveNegativeFrames) {
        // Hold previous true state through transient negatives. Don't
        // touch state.video_active.
    } else {
        // Normal operation: set video_active based on actual state
        state.video_active = should_be_active;
    }
    
    // Check if video has ended (for auto-advancing to next item in playlist)
    bool video_ended = false;
    // An item's `end` trims playback short. Same effective end main.cpp's
    // auto-advance uses (see auto_advance.h), so "ended" means one thing.
    double effective_duration = cur_duration;
    {
        const PlaylistItem* cur = current_item(state);
        if (cur) effective_duration = effective_end(cur_duration, cur->end);
    }
    if (state.video_active && effective_duration > 0.0) {
        // Check if we're at or past the end (with small tolerance for rounding)
        // Also check if mpv reports end-of-file
        if (cur_position >= effective_duration - 0.5) {
            video_ended = true;
        } else if (cur_position < effective_duration - 1.0) {
            // Video is playing normally and not near the end
            // Mark playback as started - this confirms we are playing the NEW video
            // and not seeing stale state from the previous video
            if (!state.playback_started_) {
                state.playback_started_ = true;
                std::cout << "Playback confirmed: item " << state.current_item_index
                          << ", position " << cur_position << "/" << cur_duration << std::endl;
            }
        }
    }
    
    // Reset current playlist/item indices when video stops (but not if we're auto-advancing)
    // Also don't reset if we're switching playlists (current_playlist_index is still valid)
    // Only reset if video truly stopped and we're not in the middle of a playlist switch
    if (was_active && !state.video_active && !video_ended) {
        // Only reset if we don't have a valid playlist index set
        // This prevents resetting during playlist switches
        if (state.current_playlist_index < 0) {
            state.current_item_index = -1;
            // Clear the published now-playing info too, so the phone
            // remote doesn't keep showing a track that has stopped.
            // Only on a real stop — the else-branch below is a playlist
            // switch, where the next item overwrites these immediately.
            //
            // NOT during Media Browser sessions: there the PlaybackScreen
            // owns these fields (publishes on enter()/episode-advance,
            // clears in leave()). MB playback runs with
            // current_playlist_index == -1 by design, and its pipeline
            // stops mid-session on every TV episode end — clearing here
            // would blank the remote's now-playing during the 8 s
            // next-episode countdown.
#ifdef MEDIA_BROWSER_ENABLED
            const bool mb_owns_now_playing =
                state.current_screen == AppScreen::MediaBrowser;
#else
            const bool mb_owns_now_playing = false;
#endif
            if (!mb_owns_now_playing) {
                state.now_playing_title.clear();
                state.now_playing_subtitle.clear();
                state.now_playing_kind.clear();
            }
        } else {
            // If we have a valid playlist index but video stopped, keep the index
            // This handles the case where we're switching playlists
            // The index will be updated when the new video starts
        }
    }
    
           // Record the stream's level when video becomes active (the intro
           // fade-out ramps down from it). Read only — load_file already
           // applied the user's volume. This used to set_volume(100.0) first,
           // a leftover from UI-dimming that no longer exists: original_volume
           // never left 100, so every new video jumped the stream to 100%
           // over the user's master volume.
           if (!was_active && state.video_active) {
               state.original_volume = get_volume();
               std::cout << "Video playback started: duration=" << cur_duration << "s, volume=" << state.original_volume << "%" << std::endl;

               // Mark intro as ready when video actually starts playing
               if (state.showing_intro_video && !state.intro_ready) {
                   state.intro_ready = true;
                   std::cout << "Intro video is now ready (first frame playing)" << std::endl;
               }

        // Reset advance flags when a new video starts (detected by duration change)
        // This allows auto-advance to work for the new video
        if (cur_duration > 0.0 && cur_duration != state.last_advanced_duration) {
            // New video has loaded - reset advance tracking
            state.last_advanced_item_index = -1;
            state.last_advanced_duration = cur_duration;
        }

        // Clear playlist switching flag when new video is active
        if (state.is_switching_playlist) {
            if (state.video_active) {
                std::cout << "Playlist switch completed - video is now active, clearing flag" << std::endl;
            }
            state.is_switching_playlist = false;
        }

        // BARE BONES: Removed periodic audio checks - let MPV handle audio
    }
    
    state.status_text = status_text();
}

utils::Result<> Controller::load_playlist_item(AppState& state, const app::Playlist& playlist, int item_index, const std::string& playlist_directory, std::function<void()> progress_callback) {
    if (item_index < 0 || item_index >= static_cast<int>(playlist.items.size())) {
        std::string error = "Invalid item index " + std::to_string(item_index) + " for playlist " + playlist.title;
        std::cerr << "Error: " << error << std::endl;
        return utils::Result<>::fail(error);
    }

    const auto& item = playlist.items[item_index];

    // Publish "what is playing" for StatusWriter -> kiosk_status.json ->
    // the phone remote. These AppState fields were declared and
    // serialized but never assigned anywhere, so remote.js's
    // `np.title || '—'` always rendered the em-dash and the header fell
    // back to the literal "Playlist". Set here because every playlist
    // path (select, auto-advance, next/prev, retry-after-failed-load)
    // funnels through this function.
    state.current_playlist_name = playlist.title;
    state.current_item_count    = static_cast<int>(playlist.items.size());
    state.now_playing_title     = item.title;
    if (item.source_type == "emulated_game") {
        state.now_playing_kind     = "game";
        state.now_playing_subtitle = item.emulator_system;
    } else {
        state.now_playing_kind     = "video";
        state.now_playing_subtitle = item.artist;
    }

    // Check for Master Shuffle (index 0 in playlist 0)
    if (playlist.title == "Master Shuffle") {
        std::cout << "Master Shuffle selected! Starting global shuffle..." << std::endl;
        return utils::Result<>::ok();
    }

    return load_item_media(state, item, playlist_directory, progress_callback);
}

void Controller::load_next_item(AppState& state, const std::string& playlist_directory) {
    // Check if we have a valid playlist and item index
    if (state.current_playlist_index < 0 || 
        state.current_playlist_index >= static_cast<int>(state.playlists.size())) {
        return;
    }
    
    const auto& playlist = state.playlists[state.current_playlist_index];
    
    // Check for Master Shuffle active state
    if (state.master_shuffle_active) {
        play_random_global_video(state, playlist_directory);
        return;
    }
    
    if (playlist.items.empty()) {
        return;
    }
    
    // Preserve UI visibility state - don't show UI when advancing songs
    bool was_ui_visible = state.ui_visible_when_playing;
    
    // Move to next item
    int next_index;
    int playlist_size = static_cast<int>(playlist.items.size());
    
    if (state.shuffle) {
        // Shuffle mode: every item plays once before any repeats, never the
        // same item twice in a row (app/shuffle_queue.h).
        next_index = shuffle::next_shuffled_index(
            state.shuffle_queue, state.shuffle_queue_position,
            state.shuffle_queue_playlist_id, state.current_playlist_index,
            playlist_size, state.current_item_index, rng_);
    } else {
        // Sequential mode. Per-playlist loop wins for VIDEO playlists; game
        // playlists keep the global setting (app/playlist_cursor.h).
        const auto step = cursor::sequential_next(
            state.current_item_index, playlist_size,
            cursor::effective_loop(playlist, state.playlist_loop));
        if (step.stop_to_menu) {
            // Stop playback if looping is disabled. Back to the menu,
            // NOT "cursor to item 0": indexes left >= 0 with no video
            // read as a between-items transition and the renderer drew
            // nothing until reboot. Nothing needs the cursor — starting
            // a playlist from the menu always loads item 0 itself.
            std::cout << "Playlist finished and looping disabled. Stopping playback." << std::endl;
            stop();
            stop_to_menu(state);
            return;
        }
        next_index = step.index;
    }
    
    int old_index = state.current_item_index;
    state.current_item_index = next_index;
    
    // Set advance flags BEFORE loading to prevent multiple advances
    // Keep the current duration temporarily - it will be updated when new video loads
    // Don't reset to 0.0 here as it causes race conditions with update_state
    state.last_advanced_item_index = old_index;
    
    // Load the next item
    auto load_result = load_playlist_item(state, playlist, state.current_item_index, playlist_directory, nullptr);

    if (!load_result) {
        // If load failed, try skipping to next valid item (up to playlist size attempts)
        std::cerr << "Warning: Failed to load item " << (state.current_item_index + 1)
                  << ": " << load_result.error() << ", skipping..." << std::endl;
        state.last_advanced_item_index = -1;  // Reset advance flag to allow retry

        bool found = false;
        for (int candidate : cursor::retry_order(state.current_item_index, old_index,
                                                 static_cast<int>(playlist.items.size()))) {
            state.current_item_index = candidate;

            // Defensive null guard. player_ is set at construction and not
            // reassigned anywhere, but an emulated-game retry path can have
            // already called player_->cleanup() (which flips initialized_=false
            // inside GstPlayer). stop() handles !initialized_ as a no-op
            // safely, but adding the explicit null check here prevents future
            // refactors from introducing a null deref under the same name.
            if (player_) {
                player_->stop();
            }
            auto retry = load_playlist_item(state, playlist, state.current_item_index, playlist_directory, nullptr);
            if (retry) {
                found = true;
                break;
            }
            std::cerr << "Warning: Also failed item " << (state.current_item_index + 1)
                      << ": " << retry.error() << std::endl;
        }

        if (!found) {
            // All items failed - stop and show UI. Return so the UI
            // visibility restore below can't re-hide the menu.
            std::cerr << "All playlist items failed to load, stopping." << std::endl;
            stop();
            stop_to_menu(state, "No playable content in playlist");
            return;
        }
    }

    // Restore UI visibility state - keep it hidden when advancing
    state.ui_visible_when_playing = was_ui_visible;

    if (load_result) {
        // Reset playback started flag - we need to wait for update_state to confirm
        // that the new video has actually started playing (position < duration)
        std::cout << "Resetting playback_started flag for item " << state.current_item_index << std::endl;
        state.playback_started_ = false;
        
        std::cout << "Advanced to next item in playlist: " << playlist.title 
                  << " (item " << (old_index + 1) << " -> " << (state.current_item_index + 1) 
                  << "/" << playlist.items.size() << ")" << std::endl;
    }
}

void Controller::load_previous_item(AppState& state, const std::string& playlist_directory) {
    // Check if we have a valid playlist and item index
    if (state.current_playlist_index < 0 || 
        state.current_playlist_index >= static_cast<int>(state.playlists.size())) {
        return;
    }
    
    const auto& playlist = state.playlists[state.current_playlist_index];
    if (playlist.items.empty()) {
        return;
    }
    
    // Preserve UI visibility state - don't show UI when advancing songs
    bool was_ui_visible = state.ui_visible_when_playing;
    
    // Move to previous item (loop back to end if at start)
    int old_index = state.current_item_index;
    state.current_item_index = cursor::previous_index(
        state.current_item_index, static_cast<int>(playlist.items.size()));
    
    // Set advance flags BEFORE loading to prevent multiple advances
    // Mark that we've advanced from the old index
    // Keep the current duration temporarily - it will be updated when new video loads
    // Don't reset to 0.0 here as it causes race conditions with update_state
    state.last_advanced_item_index = old_index;
    
    // Load the previous item
    auto load_result = load_playlist_item(state, playlist, state.current_item_index, playlist_directory, nullptr);

    if (!load_result) {
        // If load failed, revert to previous index and try previous item (skip broken file)
        std::cerr << "Warning: Failed to load item " << (state.current_item_index + 1)
                  << ": " << load_result.error() << ", skipping..." << std::endl;
        state.current_item_index = old_index;  // Revert index
        state.last_advanced_item_index = -1;  // Reset advance flag to allow retry
        // Try previous item if there are more
        if (playlist.items.size() > 1) {
            state.current_item_index = cursor::previous_index(
                state.current_item_index, static_cast<int>(playlist.items.size()));
            if (state.current_item_index != old_index) {  // Only if we have another item
                load_playlist_item(state, playlist, state.current_item_index, playlist_directory, nullptr);
            }
        }
    }

    // Restore UI visibility state - keep it hidden when advancing
    state.ui_visible_when_playing = was_ui_visible;

    if (load_result) {
        // Reset playback started flag
        state.playback_started_ = false;
        
        std::cout << "Advanced to previous item in playlist: " << playlist.title 
                  << " (item " << (old_index + 1) << " -> " << (state.current_item_index + 1) 
                  << "/" << playlist.items.size() << ")" << std::endl;
    }
}

namespace {
std::vector<int> playlist_sizes_of(const AppState& state) {
    std::vector<int> sizes;
    sizes.reserve(state.playlists.size());
    for (const auto& pl : state.playlists) {
        sizes.push_back(static_cast<int>(pl.items.size()));
    }
    return sizes;
}
}  // namespace

void Controller::master_shuffle_advance(AppState& state, const std::string& playlist_directory) {
    // Remember what is being left so PREV can come back to it (the virtual
    // row and unset indices are filtered inside record_history).
    shuffle::record_history(state.shuffle_history, state.current_playlist_index,
                            state.current_item_index);
    play_random_global_video(state, playlist_directory);
}

void Controller::master_shuffle_back(AppState& state, const std::string& playlist_directory) {
    // Walk back past entries a playlist reload invalidated, rather than
    // letting a stale one swallow the press.
    const auto prev = shuffle::pop_valid_history(state.shuffle_history,
                                                 playlist_sizes_of(state));
    if (!prev) {
        // No history - pick another random video
        play_random_global_video(state, playlist_directory);
        return;
    }
    state.current_playlist_index = prev->first;
    state.current_item_index = prev->second;
    state.last_advanced_item_index = -1;
    state.last_advanced_duration = 0.0;
    state.playback_started_ = false;
    load_playlist_item(state, state.playlists[prev->first], prev->second, playlist_directory);
}

void Controller::play_random_global_video(AppState& state, const std::string& playlist_directory, int depth) {
    if (depth > 5) {
        std::cerr << "Error: play_random_global_video exceeded max retry depth" << std::endl;
        return;
    }

    if (state.playlists.size() <= 1) {
        std::cerr << "Warning: No playlists available for Master Shuffle" << std::endl;
        return;
    }

    // Get next item from master shuffle queue (will generate if needed).
    // nullopt = no source playlist has a single item; the old code indexed
    // element 0 of the empty queue here.
    const std::vector<int> playlist_sizes = playlist_sizes_of(state);
    const auto pick = shuffle::next_master_item(
        state.master_shuffle_queue, state.master_shuffle_queue_position,
        playlist_sizes, rng_);
    if (!pick) {
        std::cerr << "Warning: Master Shuffle has no videos to play" << std::endl;
        return;
    }
    const auto [playlist_index, item_index] = *pick;

    // A queue built before a playlist reload can name items that no longer
    // exist: regenerate and retry.
    if (!shuffle::is_valid_source_entry(*pick, playlist_sizes)) {
        std::cerr << "Error: Stale entry in master shuffle queue; regenerating" << std::endl;
        state.master_shuffle_queue.clear();
        play_random_global_video(state, playlist_directory, depth + 1);
        return;
    }
    const auto& playlist = state.playlists[playlist_index];

    std::cout << "Master Shuffle: playlist " << playlist_index 
              << " (" << playlist.title << "), item " << item_index 
              << "/" << playlist.items.size() 
              << " [queue pos " << state.master_shuffle_queue_position 
              << "/" << state.master_shuffle_queue.size() << "]" << std::endl;
              
    // Activate Master Shuffle mode and update state for UI tracking
    state.master_shuffle_active = true;
    state.current_playlist_index = playlist_index;
    state.current_item_index = item_index;
    // Reset advance tracking flags so next auto-advance triggers next in queue
    state.last_advanced_item_index = -1;
    state.last_advanced_duration = 0.0;
    state.playback_started_ = false; // will be set when playback actually starts

    auto result = load_playlist_item(state, playlist, item_index, playlist_directory, nullptr);

    if (!result) {
        // Skip this item and try next (error already logged by load_playlist_item)
        play_random_global_video(state, playlist_directory, depth + 1);
    }
}

} // namespace app
