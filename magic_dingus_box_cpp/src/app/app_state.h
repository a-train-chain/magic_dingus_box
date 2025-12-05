#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <chrono>

// Forward declaration
namespace ui {
class SettingsMenuManager;
}

namespace app {

struct PlaylistItem {
    std::string path;
    std::string source_type;  // "local", "youtube", "emulated_game"
    std::string title;  // Video/song title
    std::string artist;  // Artist name
    std::string emulator_core;  // RetroArch core name (for games)
    std::string emulator_system;  // System name (NES, SNES, etc.)
};

struct Playlist {
    std::string title;
    std::string curator;
    std::string path;
    std::vector<PlaylistItem> items;
    
    // Check if this playlist contains only games (no videos)
    bool is_game_playlist() const {
        if (items.empty()) return false;
        for (const auto& item : items) {
            if (item.source_type != "emulated_game") {
                return false;
            }
        }
        return true;
    }
    
    // Check if this playlist contains any video content
    bool is_video_playlist() const {
        if (items.empty()) return false;
        for (const auto& item : items) {
            if (item.source_type == "local" || item.source_type == "youtube") {
                return true;
            }
        }
        return false;
    }
};

struct AppState {
    std::vector<Playlist> playlists;  // Video playlists (shown on main screen)
    std::vector<Playlist> game_playlists;  // Game playlists (for menu)
    int selected_index;
    // Display state
    bool ui_visible_when_playing = true; // Tracks whether UI should be visible when video is active
    bool reset_display = false; // Signal to reset display state (e.g. after VT switch)
    bool video_active; // This was moved from above, but the instruction snippet removed it. I'll keep it for now as the instruction was ambiguous.
    double original_volume;  // Store original volume when video starts (for dimming when UI is visible)
    std::string current_file;
    int current_playlist_index;  // Index of playlist currently playing (-1 if none)
    int current_item_index;  // Index of item currently playing (-1 if none)
    int last_advanced_item_index;  // Last item index we advanced from (to prevent multiple advances)
    double last_advanced_duration;  // Duration of the video we last advanced from (to detect when new video loads)
    bool is_switching_playlist;  // Flag to prevent overlapping playlist switches
    double position;
    double duration;
    bool paused;
    bool loop;
    bool playlist_loop;  // Whether to loop back to start of playlist when finished
    bool shuffle;        // Whether to play videos in random order
    bool master_shuffle_active; // True if Master Shuffle is playing (picks random video from ANY playlist)
    
    // Master Volume Control
    int master_volume = 100; // 0-100%
    bool show_volume_slider = false;
    
    // UI Visibility Timer
    double ui_visibility_timer = 0.0; // Seconds to keep UI visible

    std::string status_text;
    
    // Sample mode
    bool sample_mode_active;
    std::vector<double> markers;
    
    // Settings menu (pointer - managed externally)
    ui::SettingsMenuManager* settings_menu;
    
    // Fade animation state (synchronized UI and audio fade)
    std::chrono::steady_clock::time_point fade_start_time;
    std::chrono::milliseconds fade_duration;  // Duration of fade in/out
    bool fade_target_ui_visible;  // Target UI visibility state during fade
    bool is_fading;  // Whether a fade is currently in progress
    
    // Playlist switching state
    std::chrono::steady_clock::time_point playlist_switch_start_time;  // When playlist switch started
    
    // Intro video state
    bool showing_intro_video;  // True when intro video is loaded/playing
    bool intro_ready;  // True when intro video is actually playing (first frame rendered)
    bool intro_complete;  // True after intro has finished (prevents replaying)
    bool intro_fading_out;  // True when intro video is fading out
    std::chrono::steady_clock::time_point intro_fade_out_start_time;  // When intro fade-out started
    
    // Loading state
    bool is_loading_game; // True when a game is being launched
    bool playback_started; // True when current video has actually started playing (position < duration)
    
    // Display settings for CRT effects
    struct DisplaySettings {
        float scanline_intensity = 0.0f;      // 0.0 - 1.0
        float warmth_intensity = 0.0f;        // 0.0 - 1.0
        float glow_intensity = 0.0f;          // 0.0 - 1.0
        float rgb_mask_intensity = 0.0f;      // 0.0 - 1.0
        float bloom_intensity = 0.0f;         // 0.0 - 1.0
        float interlacing_intensity = 0.0f;   // 0.0 - 1.0
        float flicker_intensity = 0.0f;       // 0.0 - 1.0
        
        // Helper to cycle intensity: OFF -> Low (0.15) -> Medium (0.3) -> High (0.5) -> OFF
        // Note: Different effects might need different scales, but this is a good baseline
        void cycle_setting(float& setting) {
            if (setting <= 0.0f) setting = 0.15f;
            else if (setting <= 0.2f) setting = 0.30f;
            else if (setting <= 0.4f) setting = 0.50f;
            else setting = 0.0f;
        }
    } display_settings;

    AppState()  
        : selected_index(0)
        , video_active(false)
        , ui_visible_when_playing(false)
        , original_volume(100.0)  // Default to 100%
        , position(0.0)
        , duration(0.0)
        , paused(false)
        , loop(false)
        , playlist_loop(true) // Default to looping playlists
        , shuffle(false) // Default to sequential playback
        , master_shuffle_active(false)
        , sample_mode_active(false)
        , settings_menu(nullptr)
        , fade_duration(300)  // 300ms fade duration
        , fade_target_ui_visible(false)
        , is_fading(false)
        , current_playlist_index(-1)
        , current_item_index(-1)
        , last_advanced_item_index(-1)
        , last_advanced_duration(0.0)
        , is_switching_playlist(false)
        , showing_intro_video(false)
        , intro_ready(false)
        , intro_complete(false)
        , intro_fading_out(false)
        , is_loading_game(false)
        , playback_started(false)
    {
        // Initialize default display settings
        // Match "Medium" scanlines from previous hardcoded value (24/255 ~= 0.1)
        display_settings.scanline_intensity = 0.1f; 
    }
};

} // namespace app
