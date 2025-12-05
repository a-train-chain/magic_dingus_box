#include "controller.h"
#include "../video/mpv_player.h"
#include "../utils/path_resolver.h"
#include "app_state.h"

#include <sstream>
#include <iomanip>
#include <iostream>
#include <experimental/filesystem>
#include <chrono>
#include <thread>

namespace fs = std::experimental::filesystem;

namespace app {

Controller::Controller(video::MpvPlayer* player)
    : player_(player)
{
}

void Controller::load_file(const std::string& path, double start, double end, bool loop) {
    if (player_) {
        player_->load_file(path, start, end, loop);
    }
}

bool Controller::load_file_with_resolution(const std::string& path, const std::string& playlist_dir, double start, double end, bool loop) {
    if (!player_) {
        return false;
    }
    
    std::string resolved_path = utils::resolve_video_path(path, playlist_dir);
    
    // Check if file exists before trying to load
    fs::path file_path(resolved_path);
    if (!fs::exists(file_path)) {
        std::cerr << "ERROR: Video file does not exist: " << resolved_path << std::endl;
        return false;
    }
    
    bool success = player_->load_file(resolved_path, start, end, loop);
    if (!success) {
        std::cerr << "ERROR: Failed to load video file: " << resolved_path << std::endl;
        return false;
    }
    
    return true;
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

void Controller::update_state(AppState& state) {
    if (!player_) {
        return;
    }
    
    state.position = get_position();
    state.duration = get_duration();
    state.paused = is_paused();
    
    // Check if video is actually playing (mpv might report playing even before duration is available)
    bool mpv_playing = is_playing();
    
    // Only set video_active to true if a file is actually loaded (duration > 0)
    // AND either it's playing or has a position > 0
    // During playlist switches, allow video_active to become true when new video loads
    // but prevent it from being set to false prematurely
    bool was_active = state.video_active;
    bool should_be_active = (state.duration > 0.0) && (mpv_playing || (state.position > 0.0));
    
    if (state.is_switching_playlist) {
        // During switch: only allow video_active to become true (new video loaded)
        // Don't allow it to become false (would interfere with switch detection)
        if (should_be_active) {
            state.video_active = true;
        }
        // If should_be_active is false, keep current state (don't change it)
    } else {
        // Normal operation: set video_active based on actual state
        state.video_active = should_be_active;
    }
    
    // Check if video has ended (for auto-advancing to next item in playlist)
    bool video_ended = false;
    if (state.video_active && state.duration > 0.0) {
        // Check if we're at or past the end (with small tolerance for rounding)
        // Also check if mpv reports end-of-file
        if (state.position >= state.duration - 0.5) {
            video_ended = true;
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
        } else {
            // If we have a valid playlist index but video stopped, keep the index
            // This handles the case where we're switching playlists
            // The index will be updated when the new video starts
        }
    }
    
           // Capture original volume when video becomes active (only once)
           if (!was_active && state.video_active) {
               // Only capture if it hasn't been set yet (default is 100.0)
               // Also ensure volume is at 100% when capturing (in case it was dimmed from previous playlist)
               if (state.original_volume == 100.0) {
                   // Restore volume to 100% first to ensure we capture the correct original volume
                   set_volume(100.0);
                   state.original_volume = get_volume();
               }
               std::cout << "Video playback started: duration=" << state.duration << "s, volume=" << state.original_volume << "%" << std::endl;
               
               // Mark intro as ready when video actually starts playing
               if (state.showing_intro_video && !state.intro_ready) {
                   state.intro_ready = true;
                   std::cout << "Intro video is now ready (first frame playing)" << std::endl;
               }
        
        // Reset advance flags when a new video starts (detected by duration change)
        // This allows auto-advance to work for the new video
        if (state.duration > 0.0 && state.duration != state.last_advanced_duration) {
            // New video has loaded - reset advance tracking
            state.last_advanced_item_index = -1;
            state.last_advanced_duration = state.duration;
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

bool Controller::load_playlist_item(const app::Playlist& playlist, int item_index, const std::string& playlist_directory) {
    if (item_index < 0 || item_index >= static_cast<int>(playlist.items.size())) {
        std::cerr << "Error: Invalid item index " << item_index << " for playlist " << playlist.title << std::endl;
        return false;
    }

    const auto& item = playlist.items[item_index];
    if (item.source_type == "local") {
        std::cout << "Starting playlist transition..." << std::endl;

        // Stop current playback
        stop();
        std::cout << "Stopped playback, loading new file..." << std::endl;

        // Brief delay for MPV to stop
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Verify MPV actually stopped
        int retry_count = 0;
        const int max_retries = 5;
        while (is_playing() && retry_count < max_retries) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            retry_count++;
        }

        // Load the new file with software decoding (no hardware buffer issues)
        std::cout << "Loading file: " << item.path << std::endl;
        bool loaded = load_file_with_resolution(item.path, playlist_directory, 0.0, 0.0, false);
        if (loaded) {
            std::cout << "File loaded successfully, starting playback..." << std::endl;

            play();

            // Brief delay for playback to start
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            // Verify playback actually started within reasonable time
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!is_playing()) {
                std::cerr << "Warning: Playback did not start after load - this may cause playlist switching issues" << std::endl;
            }

            // Debug: Check audio-related properties after video starts
            try {
                double vol = player_->get_volume();
                std::string audio_device = player_->get_property("audio-device");
                std::cout << "DEBUG: After playlist transition - volume=" << vol << ", audio-device='" << audio_device << "'" << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "DEBUG: Error checking audio properties: " << e.what() << std::endl;
            }

            return true;
        } else {
            std::cerr << "Error: Failed to load playlist item: " << item.path << std::endl;
            return false;
        }
    } else {
        std::cerr << "Error: Unsupported source type: " << item.source_type << std::endl;
        return false;
    }
}

void Controller::load_next_item(AppState& state, const std::string& playlist_directory) {
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
    
    // Move to next item (loop back to 0 if at end)
    int old_index = state.current_item_index;
    state.current_item_index = (state.current_item_index + 1) % playlist.items.size();
    
    // Set advance flags BEFORE loading to prevent multiple advances
    // Mark that we've advanced from the old index
    state.last_advanced_item_index = old_index;
    // Keep the current duration temporarily - it will be updated when new video loads
    // Don't reset to 0.0 here as it causes race conditions with update_state
    
    // Load the next item
    bool load_success = load_playlist_item(playlist, state.current_item_index, playlist_directory);
    
    if (!load_success) {
        // If load failed, revert to previous index and try next item (skip broken file)
        std::cerr << "Warning: Failed to load item " << (state.current_item_index + 1)
                  << ", skipping..." << std::endl;
        state.current_item_index = old_index;  // Revert index
        state.last_advanced_item_index = -1;  // Reset advance flag to allow retry
        // Try next item if there are more
        if (playlist.items.size() > 1) {
            state.current_item_index = (state.current_item_index + 1) % playlist.items.size();
            if (state.current_item_index != old_index) {  // Only if we have another item
                // Stop current playback before trying next item
                player_->stop();
                load_playlist_item(playlist, state.current_item_index, playlist_directory);
            }
        }
    }
    
    // Restore UI visibility state - keep it hidden when advancing
    state.ui_visible_when_playing = was_ui_visible;
    
    if (load_success) {
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
    state.current_item_index = (state.current_item_index - 1 + playlist.items.size()) % playlist.items.size();
    
    // Set advance flags BEFORE loading to prevent multiple advances
    // Mark that we've advanced from the old index
    state.last_advanced_item_index = old_index;
    // Keep the current duration temporarily - it will be updated when new video loads
    // Don't reset to 0.0 here as it causes race conditions with update_state
    
    // Load the previous item
    bool load_success = load_playlist_item(playlist, state.current_item_index, playlist_directory);
    
    if (!load_success) {
        // If load failed, revert to previous index and try previous item (skip broken file)
        std::cerr << "Warning: Failed to load item " << (state.current_item_index + 1) 
                  << ", skipping..." << std::endl;
        state.current_item_index = old_index;  // Revert index
        state.last_advanced_item_index = -1;  // Reset advance flag to allow retry
        // Try previous item if there are more
        if (playlist.items.size() > 1) {
            state.current_item_index = (state.current_item_index - 1 + playlist.items.size()) % playlist.items.size();
            if (state.current_item_index != old_index) {  // Only if we have another item
                load_playlist_item(playlist, state.current_item_index, playlist_directory);
            }
        }
    }
    
    // Restore UI visibility state - keep it hidden when advancing
    state.ui_visible_when_playing = was_ui_visible;
    
    if (load_success) {
        std::cout << "Advanced to previous item in playlist: " << playlist.title 
                  << " (item " << (old_index + 1) << " -> " << (state.current_item_index + 1) 
                  << "/" << playlist.items.size() << ")" << std::endl;
    }
}

} // namespace app

