#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include <atomic>
#include <mutex>
#include <shared_mutex>

namespace app {

// Display mode enumeration
enum class DisplayMode {
    CRT_NATIVE,   // Fullscreen with CRT effects
    MODERN_TV     // 4:3 centered with bezel overlay
};

// Bezel info structure
struct BezelInfo {
    std::string id;
    std::string name;
    std::string file;  // filename (empty for procedural)
    std::string description;
};

} // namespace app

// Forward declaration
namespace ui {
class SettingsMenuManager;
class VirtualKeyboard;
}

#include "../utils/wifi_manager.h"

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

    // Thread-safe flags (accessed from GStreamer callbacks and main thread)
    std::atomic<bool> reset_display{false};       // Signal to reset display state (e.g. after VT switch)
    std::atomic<bool> video_active{false};        // True when video is actively playing
    std::atomic<bool> paused{false};              // True when playback is paused
    std::atomic<bool> is_switching_playlist{false}; // Flag to prevent overlapping playlist switches
    std::atomic<bool> playback_started_{false};   // True when current video has actually started playing

    double original_volume;  // Store original volume when video starts (for dimming when UI is visible)
    std::string current_file;
    int current_playlist_index;  // Index of playlist currently playing (-1 if none)
    int current_item_index;  // Index of item currently playing (-1 if none)
    int last_advanced_item_index;  // Last item index we advanced from (to prevent multiple advances)
    double last_advanced_duration;  // Duration of the video we last advanced from (to detect when new video loads)

    // Thread-safe position/duration (written by GStreamer, read by UI)
private:
    mutable std::shared_mutex playback_mutex_;
    double position_ = 0.0;
    double duration_ = 0.0;

public:
    // Thread-safe accessors for position/duration
    void set_position(double pos) {
        std::unique_lock lock(playback_mutex_);
        position_ = pos;
    }

    double get_position() const {
        std::shared_lock lock(playback_mutex_);
        return position_;
    }

    void set_duration(double dur) {
        std::unique_lock lock(playback_mutex_);
        duration_ = dur;
    }

    double get_duration() const {
        std::shared_lock lock(playback_mutex_);
        return duration_;
    }

    // Bulk update for efficiency (single lock)
    void update_playback_state(double pos, double dur) {
        std::unique_lock lock(playback_mutex_);
        position_ = pos;
        duration_ = dur;
    }

    // Legacy accessors for backwards compatibility (direct access still available for single-threaded use)
    double position = 0.0;  // Deprecated: use get_position()/set_position()
    double duration = 0.0;  // Deprecated: use get_duration()/set_duration()

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
    std::atomic<bool> is_loading_game{false}; // True when a game is being launched
    // Note: playback_started is now playback_started_ (atomic) - use playback_started_.load()/store()
    
    // Display settings for CRT effects and display mode
    struct DisplaySettings {
        DisplayMode mode = DisplayMode::CRT_NATIVE;
        int bezel_index = 0;  // Index into bezels array (0 = procedural/none)
        
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
        
        // Cycle display mode: CRT_NATIVE <-> MODERN_TV
        void cycle_mode() {
            mode = (mode == DisplayMode::CRT_NATIVE) 
                   ? DisplayMode::MODERN_TV 
                   : DisplayMode::CRT_NATIVE;
        }
        
        // Get mode name for display
        std::string get_mode_name() const {
            return (mode == DisplayMode::CRT_NATIVE) ? "CRT Native" : "Modern TV";
        }
    } display_settings;
    
    // Available bezels (loaded from bezels.json)
    std::vector<BezelInfo> available_bezels;

    AppState()
        : selected_index(0),
          original_volume(100.0),
          current_playlist_index(-1),
          current_item_index(-1),
          last_advanced_item_index(-1),
          last_advanced_duration(0.0),
          position(0.0),
          duration(0.0),
          loop(false),
          playlist_loop(true),
          shuffle(false),
          master_shuffle_active(false),
          sample_mode_active(false),
          settings_menu(nullptr),
          fade_duration(300),
          fade_target_ui_visible(false),
          is_fading(false),
          showing_intro_video(false),
          intro_ready(false),
          intro_complete(false),
          intro_fading_out(false)
    {
        // Atomic members are already initialized inline
        // Initialize default display settings
        display_settings.scanline_intensity = 0.1f;
    }
    
    // Wi-Fi State
    struct WifiState {
        bool enabled = true;
        bool scanning = false;
        std::vector<utils::WifiNetwork> scan_results;
        std::string status_message;
    } wifi_state;
    
    // Content Manager URL for QR code display
    std::string content_manager_url;
    
    // Virtual Keyboard
    ui::VirtualKeyboard* keyboard = nullptr; // pointer to keyboard instance (owned by main/renderer or here?) 
    // Better to let AppState own it or main. Let's make it a pointer managed by main, 
    // or include the header and make it a member. Header inclusion is cleaner for usage.
    // For now, pointer to avoid header dependency hell in this header 
    // (forward declared above).
};

} // namespace app
