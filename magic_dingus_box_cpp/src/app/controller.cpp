#include "controller.h"
#include "../video/video_player.h"
#include "../video/gst_player.h"
#include "../utils/path_resolver.h"
#include "../retroarch/retroarch_launcher.h"
#include "../platform/drm_display.h"
#include "app_state.h"

#include <sstream>
#include <iomanip>
#include <iostream>
#include <filesystem>
#include <chrono>
#include <thread>
#include <random>
#include <algorithm>

#include "../platform/input_manager.h"

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

// ... (rest of file)

void Controller::set_system_volume(int percent) {
    // Clamp percentage
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    
    current_system_volume_ = percent;
    
    // 1. Try to set 'Master' volume
    std::string command_master = "amixer sset 'Master' " + std::to_string(percent) + "% > /dev/null 2>&1";
    int ret_master = std::system(command_master.c_str());
    
    // 2. Try to set 'PCM' volume (fallback or additional)
    std::string command_pcm = "amixer sset 'PCM' " + std::to_string(percent) + "% > /dev/null 2>&1";
    int ret_pcm = std::system(command_pcm.c_str());
    
    if (ret_master != 0 && ret_pcm != 0) {
        std::cerr << "Warning: Failed to set system volume (amixer Master/PCM both failed)" << std::endl;
    }
    
    // 3. Set player software volume as well (belt and suspenders)
    if (player_) {
        player_->set_volume(percent);
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

void Controller::check_audio_recovery() {
    // GStreamer player doesn't implement this yet
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
        } else if (state.position < state.duration - 1.0) {
            // Video is playing normally and not near the end
            // Mark playback as started - this confirms we are playing the NEW video
            // and not seeing stale state from the previous video
            if (!state.playback_started_) {
                state.playback_started_ = true;
                std::cout << "Playback confirmed: item " << state.current_item_index 
                          << ", position " << state.position << "/" << state.duration << std::endl;
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

// Helper to wait with callback
void wait_with_callback(int milliseconds, std::function<void()> callback) {
    auto start = std::chrono::steady_clock::now();
    auto duration = std::chrono::milliseconds(milliseconds);
    
    while (std::chrono::steady_clock::now() - start < duration) {
        if (callback) {
            callback();
        }
        // Small sleep to prevent 100% CPU usage but keep animation smooth
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

utils::Result<> Controller::load_playlist_item(AppState& state, const app::Playlist& playlist, int item_index, const std::string& playlist_directory, std::function<void()> progress_callback) {
    if (item_index < 0 || item_index >= static_cast<int>(playlist.items.size())) {
        std::string error = "Invalid item index " + std::to_string(item_index) + " for playlist " + playlist.title;
        std::cerr << "Error: " << error << std::endl;
        return utils::Result<>::fail(error);
    }

    const auto& item = playlist.items[item_index];

    // Check for Master Shuffle (index 0 in playlist 0)
    if (playlist.title == "[S] Master Shuffle") {
        std::cout << "Master Shuffle selected! Starting global shuffle..." << std::endl;
        return utils::Result<>::ok();
    }

    if (item.source_type == "local") {
        std::cout << "Starting playlist transition..." << std::endl;

        // Stop current playback
        stop();
        std::cout << "Stopped playback, loading new file..." << std::endl;

        // Brief delay for MPV to stop
        wait_with_callback(200, progress_callback);

        // Load the new file
        std::cout << "Loading file: " << item.path << std::endl;
        auto load_result = load_file_with_resolution(item.path, playlist_directory, 0.0, 0.0, false);
        if (load_result) {
            std::cout << "File loaded successfully, starting playback..." << std::endl;

            play();

            // Update player state immediately after play
            if (auto gst_player = dynamic_cast<video::GstPlayer*>(player_)) {
                gst_player->update_state();
            }

            // Brief delay for playback to start
            wait_with_callback(1000, progress_callback);

            // Verify playback actually started
            if (!is_playing()) {
                std::cerr << "Warning: Playback did not start after load - this may cause playlist switching issues" << std::endl;
            }

            // Debug: Check audio-related properties after video starts
            try {
                double vol = player_->get_volume();
                std::cout << "DEBUG: After playlist transition - volume=" << vol << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "DEBUG: Error checking audio properties: " << e.what() << std::endl;
            }

            return utils::Result<>::ok();
        } else {
            std::string error = "Failed to load playlist item: " + item.path + " (" + load_result.error() + ")";
            std::cerr << "Error: " << error << std::endl;
            return utils::Result<>::fail(error);
        }
    } else if (item.source_type == "emulated_game") {
        // Handle RetroArch game launch
        std::cout << "Launching RetroArch game: " << item.title << std::endl;
        std::cout << "  Core: " << item.emulator_core << std::endl;
        std::cout << "  System: " << item.emulator_system << std::endl;
        std::cout << "  Path: " << item.path << std::endl;

        // Get core name from playlist item
        std::string core_name = item.emulator_core;
        if (core_name.empty()) {
            std::string error = "No emulator_core specified for game: " + item.title;
            std::cerr << "Error: " << error << std::endl;
            return utils::Result<>::fail(error);
        }
        
        // Resolve "auto" core based on system
        if (core_name == "auto") {
            std::string system = item.emulator_system;
            if (system == "genesis") {
                core_name = "genesis_plus_gx";
            } else if (system == "snes") {
                core_name = "snes9x2010";
            } else if (system == "nes") {
                core_name = "nestopia";
            } else if (system == "ps1" || system == "psx") {
                core_name = "pcsx_rearmed";
            } else if (system == "atari7800") {
                core_name = "prosystem";
            } else if (system == "pcengine") {
                core_name = "mednafen_pce_fast";
            } else if (system == "arcade") {
                core_name = "fbneo";
            } else {
                std::string error = "Could not resolve auto core for system: " + system;
                std::cerr << "Error: " << error << std::endl;
                return utils::Result<>::fail(error);
            }
            std::cout << "Resolved auto core for " << system << " -> " << core_name << std::endl;
        }
        
        // Resolve full ROM path
        std::string resolved_rom_path = utils::resolve_video_path(item.path, playlist_directory);
        
        // Check if ROM exists
        if (!fs::exists(resolved_rom_path)) {
            std::string error = "ROM file does not exist: " + resolved_rom_path;
            std::cerr << "Error: " << error << std::endl;
            return utils::Result<>::fail(error);
        }
        
        // Look for overlay/bezel (optional)
        std::string overlay_path;
        // Could implement bezel lookup here based on emulator_system if needed
        
        // Stop GStreamer completely before launching RetroArch
        // This ensures all resources (EGL, DRM, threads) are released
        if (player_) {
            std::cout << "Cleaning up GStreamer pipeline before RetroArch launch..." << std::endl;
            player_->cleanup();
            // Wait a moment for cleanup to finish? cleanup() should be synchronous for pipeline destruction.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } // Wait for stop to complete
        wait_with_callback(300, progress_callback);
        
        // Verify player stopped with retry loop
        int retry_count = 0;
        const int max_retries = 10;
        while (is_playing() && retry_count < max_retries) {
            wait_with_callback(100, progress_callback);
            retry_count++;
        }
        
        if (is_playing()) {
            std::cerr << "Warning: GStreamer did not stop cleanly before RetroArch launch" << std::endl;
        } else {
            std::cout << "GStreamer stopped successfully" << std::endl;
        }
        
        // Cleanup DRM display before launching RetroArch to release resources
        // Don't restore mode - leave it at 640x480 for RetroArch
        // Cleanup DRM display before launching RetroArch to release resources
        // Don't restore mode - leave it at 640x480 for RetroArch
        if (display_) {
            // CRITICAL: Keep CRTC enabled (disable_crtc = false) for Vulkan compatibility.
            // Disabling it causes "QueuePresent failed" on startup for most cores (Genesis, SNES, NES, PS1).
            // We rely on pkill and display restoration logic for clean exit.
            bool disable_crtc = false;

            std::cout << "Releasing DRM master for RetroArch (disable_crtc=" << disable_crtc << ")..." << std::endl;
            display_->release_master(disable_crtc);
            std::cout << "DRM master released" << std::endl;
            // Wait for DRM resources to be fully released and display to settle
            wait_with_callback(200, progress_callback);
        }
        
        // CRITICAL: Release controller input grab before launching RetroArch
        // This ensures the main app doesn't block RetroArch from accessing the controller
        if (input_manager_) {
            std::cout << "Releasing input devices for RetroArch..." << std::endl;
            input_manager_->cleanup();
            std::cout << "Input devices released" << std::endl;
        }
        
        // CRITICAL: Wake up controller before launching RetroArch
        // Controller may be in sleep mode after GStreamer/DRM cleanup
        std::cout << "Waking up controller before RetroArch launch..." << std::endl;
        std::system("sudo udevadm trigger --action=change --sysname-match=js* 2>/dev/null || true");
        std::system("sudo udevadm trigger --action=change --sysname-match=event* 2>/dev/null || true");
        wait_with_callback(200, progress_callback);
        std::cout << "Controller wake-up signal sent" << std::endl;
        
        // Launch the game (BLOCKING)
        retroarch::GameLaunchInfo game_info = {
            resolved_rom_path,
            core_name,
            overlay_path
        };
        
        
        bool launched = retroarch_launcher_.launch_game(game_info, current_system_volume_, state.audio_settings.retroarch_volume_offset_db);
        
        // Game has exited. Restore system.
        
        // CRITICAL: Ensure RetroArch is truly dead before we try to take back control
        // This prevents "zombie" processes from holding onto DRM/Input resources
        std::cout << "RetroArch exited. Ensuring process termination..." << std::endl;
        std::system("pkill -9 retroarch 2>/dev/null || true");
        
        // Add delay here to ensure RetroArch has fully released DRM master and kernel resources
        std::cout << "Waiting for system to settle..." << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        // Re-acquire DRM master with retry logic
        if (display_) {
            std::cout << "Re-acquiring DRM master..." << std::endl;
            bool acquired = false;
            for (int i = 0; i < 5; ++i) {
                if (display_->acquire_master()) {
                    acquired = true;
                    std::cout << "DRM master acquired successfully." << std::endl;
                    break;
                }
                std::cerr << "Failed to acquire DRM master, retrying (" << (i+1) << "/5)..." << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            
            if (!acquired) {
                std::cerr << "CRITICAL: Failed to acquire DRM master after retries! Attempting to proceed anyway..." << std::endl;
            }
            
            // Force restore video mode to ensure UI is visible
            std::cout << "Restoring display mode to 640x480..." << std::endl;
            display_->set_mode(640, 480);
        }

        // Re-initialize input devices after RetroArch exits with retry logic
        if (input_manager_) {
            std::cout << "Re-initializing input devices after RetroArch..." << std::endl;
            
            bool input_initialized = false;
            for (int i = 0; i < 3; ++i) {
                // Re-wake controller before initializing
                std::system("sudo udevadm trigger --action=change --sysname-match=js* 2>/dev/null || true");
                std::system("sudo udevadm trigger --action=change --sysname-match=event* 2>/dev/null || true");
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                
                if (input_manager_->initialize()) {
                    input_initialized = true;
                    std::cout << "Input devices initialized successfully." << std::endl;
                    break;
                }
                std::cerr << "Failed to initialize input devices, retrying (" << (i+1) << "/3)..." << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            
            if (!input_initialized) {
                std::cerr << "CRITICAL: Failed to re-initialize input devices!" << std::endl;
            }
        }
        
        if (launched) {
            std::cout << "Successfully launched game: " << item.title << std::endl;
            
            // CRITICAL: Ensure UI is visible when returning from game
            std::cout << "Game exited - ensuring UI is visible and video state is correct" << std::endl;
            
            // Signal main loop to reset display state (fix page flip failure)
            state.reset_display = true;
            
            // CRITICAL: Do NOT try to resume video after game exit
            // The GStreamer pipeline was killed via pkill during RetroArch launch
            // The GstPlayer object still has stale state (duration > 0, position, etc.)
            // Trying to play() on a dead pipeline corrupts EGL rendering state
            // Instead, properly stop the player to clear its state
            if (player_) {
                // Determine if we need to cleanup (if not already done)
                // We called cleanup() before launch, but player_ object still exists
                // Calling stop() here ensures pure-virtual state is clean
                player_->stop();
                std::cout << "Stopped video player after game exit" << std::endl;
            }

            // CRITICAL: Reset playback state to prevent Renderer from thinking we are "transitioning"
            // The Renderer's is_transitioning logic (current_item_index >= 0 && !video_active)
            // causes it to skip rendering the UI (thinking it's a video transition gap).
            // Since we just finished a game, we are NOT playing video, so we must clear this state
            // to allow the UI to render.
            state.current_item_index = -1;
            state.current_playlist_index = -1;
            state.video_active = false; // Ensure this is false too

            return utils::Result<>::ok();
        } else {
            std::string error = "Failed to launch game: " + item.title;
            std::cerr << "Error: " << error << std::endl;
            return utils::Result<>::fail(error);
        }
    } else {
        std::string error = "Unsupported source type: " + item.source_type;
        std::cerr << "Error: " << error << std::endl;
        return utils::Result<>::fail(error);
    }
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
        // Shuffle mode: Use queue-based selection (Fisher-Yates)
        // Ensures all videos play once before reshuffling
        if (playlist_size <= 1) {
            next_index = 0;
        } else {
            // Check if playlist changed (need to regenerate queue)
            if (state.shuffle_queue_playlist_id != state.current_playlist_index) {
                state.shuffle_queue.clear();  // Force regeneration
                state.shuffle_queue_playlist_id = state.current_playlist_index;
            }
            
            // Get next shuffled index (will generate queue if needed)
            next_index = get_next_shuffled_index(state, playlist_size);
            
            // If we got the same video we're currently on (shouldn't happen often),
            // get another one
            if (next_index == state.current_item_index && state.shuffle_queue_position < playlist_size) {
                next_index = get_next_shuffled_index(state, playlist_size);
            }
        }
    } else {
        // Sequential mode
        next_index = state.current_item_index + 1;
        
        // Check if we've reached the end of the playlist
        if (next_index >= playlist_size) {
            if (state.playlist_loop) {
                // Loop back to start
                next_index = 0;
            } else {
                // Stop playback if looping is disabled
                std::cout << "Playlist finished and looping disabled. Stopping playback." << std::endl;
                stop();
                // Reset to start for next play, but don't load it
                state.current_item_index = 0;
                state.video_active = false;
                state.ui_visible_when_playing = true; // Show UI when stopped
                return;
            }
        }
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
        // If load failed, revert to previous index and try next item (skip broken file)
        std::cerr << "Warning: Failed to load item " << (state.current_item_index + 1)
                  << ": " << load_result.error() << ", skipping..." << std::endl;
        state.current_item_index = old_index;  // Revert index
        state.last_advanced_item_index = -1;  // Reset advance flag to allow retry
        // Try next item if there are more
        if (playlist.items.size() > 1) {
            state.current_item_index = (state.current_item_index + 1) % playlist.items.size();
            if (state.current_item_index != old_index) {  // Only if we have another item
                // Stop current playback before trying next item
                player_->stop();
                load_playlist_item(state, playlist, state.current_item_index, playlist_directory, nullptr);
            }
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
    state.current_item_index = (state.current_item_index - 1 + playlist.items.size()) % playlist.items.size();
    
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
            state.current_item_index = (state.current_item_index - 1 + playlist.items.size()) % playlist.items.size();
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



utils::Result<> Controller::initialize_retroarch_launcher() {
    if (!retroarch_launcher_.initialize()) {
        return utils::Result<>::fail("Failed to initialize RetroArch launcher");
    }
    return utils::Result<>::ok();
}

void Controller::play_random_global_video(AppState& state, const std::string& playlist_directory) {
    if (state.playlists.size() <= 1) {
        std::cerr << "Warning: No playlists available for Master Shuffle" << std::endl;
        return;
    }

    // Get next item from master shuffle queue (will generate if needed)
    auto [playlist_index, item_index] = get_next_master_shuffled_item(state);
    
    // Validate the selection
    if (playlist_index < 0 || playlist_index >= static_cast<int>(state.playlists.size())) {
        std::cerr << "Error: Invalid playlist index from master shuffle queue" << std::endl;
        // Regenerate and retry
        state.master_shuffle_queue.clear();
        play_random_global_video(state, playlist_directory);
        return;
    }
    
    const auto& playlist = state.playlists[playlist_index];
    
    if (item_index < 0 || item_index >= static_cast<int>(playlist.items.size())) {
        std::cerr << "Error: Invalid item index from master shuffle queue" << std::endl;
        // Regenerate and retry
        state.master_shuffle_queue.clear();
        play_random_global_video(state, playlist_directory);
        return;
    }

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
        play_random_global_video(state, playlist_directory);
    }
}

// Generate a shuffled queue of indices for the current playlist
// Uses Fisher-Yates shuffle to randomize order
void Controller::generate_shuffle_queue(AppState& state, int playlist_size) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    
    // Create sequential indices
    state.shuffle_queue.clear();
    state.shuffle_queue.reserve(playlist_size);
    for (int i = 0; i < playlist_size; ++i) {
        state.shuffle_queue.push_back(i);
    }
    
    // Fisher-Yates shuffle
    std::shuffle(state.shuffle_queue.begin(), state.shuffle_queue.end(), gen);
    
    // Reset position to start
    state.shuffle_queue_position = 0;
    
    std::cout << "Generated new shuffle queue with " << playlist_size << " items" << std::endl;
}

// Get the next index from the shuffle queue, regenerating if exhausted
int Controller::get_next_shuffled_index(AppState& state, int playlist_size) {
    // Check if queue is empty or exhausted
    if (state.shuffle_queue.empty() || 
        state.shuffle_queue_position >= static_cast<int>(state.shuffle_queue.size()) ||
        static_cast<int>(state.shuffle_queue.size()) != playlist_size) {
        // Generate new queue
        generate_shuffle_queue(state, playlist_size);
    }
    
    // Get next index and advance position
    int index = state.shuffle_queue[state.shuffle_queue_position];
    state.shuffle_queue_position++;
    
    return index;
}

// Generate a shuffled queue for Master Shuffle (all items from all playlists)
void Controller::generate_master_shuffle_queue(AppState& state) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    
    state.master_shuffle_queue.clear();
    
    // Collect all items from all playlists (skip playlist 0 which is Master Shuffle itself)
    for (size_t playlist_idx = 1; playlist_idx < state.playlists.size(); ++playlist_idx) {
        const auto& playlist = state.playlists[playlist_idx];
        for (size_t item_idx = 0; item_idx < playlist.items.size(); ++item_idx) {
            state.master_shuffle_queue.emplace_back(static_cast<int>(playlist_idx), static_cast<int>(item_idx));
        }
    }
    
    // Fisher-Yates shuffle
    std::shuffle(state.master_shuffle_queue.begin(), state.master_shuffle_queue.end(), gen);
    
    // Reset position to start
    state.master_shuffle_queue_position = 0;
    
    std::cout << "Generated new master shuffle queue with " << state.master_shuffle_queue.size() 
              << " items from " << (state.playlists.size() - 1) << " playlists" << std::endl;
}

// Get the next item from the master shuffle queue, regenerating if exhausted
std::pair<int, int> Controller::get_next_master_shuffled_item(AppState& state) {
    // Check if queue is empty or exhausted
    if (state.master_shuffle_queue.empty() || 
        state.master_shuffle_queue_position >= static_cast<int>(state.master_shuffle_queue.size())) {
        // Generate new queue
        generate_master_shuffle_queue(state);
    }
    
    // Get next item and advance position
    auto item = state.master_shuffle_queue[state.master_shuffle_queue_position];
    state.master_shuffle_queue_position++;
    
    return item;
}

} // namespace app
