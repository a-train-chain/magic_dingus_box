#pragma once

#include "app_state.h"
#include "../video/video_player.h"
#include "../retroarch/retroarch_launcher.h"
#include "../utils/result.h"
#include <string>
#include <functional>
#include <random>

namespace platform {
    class DrmDisplay;  // Forward declaration
    class InputManager;  // Forward declaration
}

namespace app {

class Controller {
public:
    Controller(video::VideoPlayer* player);
    
    // Set display reference for DRM cleanup before RetroArch launch
    void set_display(platform::DrmDisplay* display) { display_ = display; }
    
    // Set input manager reference for controller release before RetroArch launch
    void set_input_manager(platform::InputManager* input_manager) { input_manager_ = input_manager; }
    
    // Playback control
    void load_file(const std::string& path, double start = 0.0, double end = 0.0, bool loop = false);
    utils::Result<> load_file_with_resolution(const std::string& path, const std::string& playlist_dir, double start = 0.0, double end = 0.0, bool loop = false);
    void play();
    void pause();
    void toggle_pause();
    void seek(double seconds);
    void seek_absolute(double timestamp);
    void stop();
    
    // State queries
    bool is_playing() const;
    bool is_paused() const;
    double get_position() const;
    double get_duration() const;
    
    // Status text
    std::string status_text() const;
    
    // Volume control
    void set_volume(double volume);
    double get_volume() const;
    
    // Update state from player
    void update_state(AppState& state);
    
    // Load a playlist item (video or game)
    // progress_callback: Optional callback to run while waiting (e.g. for loading animation)
    // Returns Result with error message on failure
    utils::Result<> load_playlist_item(AppState& state, const app::Playlist& playlist, int item_index, const std::string& playlist_directory, std::function<void()> progress_callback = nullptr);
    
    // Navigation
    void load_next_item(AppState& state, const std::string& playlist_directory);
    void load_previous_item(AppState& state, const std::string& playlist_directory);
    
    // Play a random video from any playlist (Master Shuffle)
    void play_random_global_video(AppState& state, const std::string& playlist_directory, int depth = 0);
    
    // Audio recovery
    void check_audio_recovery();

    // Polled each main-loop tick. Reads data/seek_request.json if present,
    // seeks to (pos * duration), then deletes the file. No-op if the file
    // doesn't exist or contains an invalid pos. Cheap when no file exists
    // (single fs::exists check).
    void poll_seek_request();
    
    // System Volume
    void set_system_volume(int percent);
    
    // Initialize RetroArch launcher
    utils::Result<> initialize_retroarch_launcher();
    
    // Get RetroArch launcher (for core downloader)
    retroarch::RetroArchLauncher& get_retroarch_launcher() { return retroarch_launcher_; }

private:
    video::VideoPlayer* player_;
    retroarch::RetroArchLauncher retroarch_launcher_;
    platform::DrmDisplay* display_;  // For DRM cleanup before RetroArch launch
    platform::InputManager* input_manager_;  // For controller release before RetroArch launch
    int current_system_volume_ = 100;
    std::mt19937 rng_{std::random_device{}()};

    // Debounce counter for state.video_active. The GStreamer state poll
    // and position/duration queries can return transient negatives during
    // seek transitions (PLAYING→PAUSED→PLAYING). Without this debounce,
    // state.video_active flickers true→false→true at update_state cadence,
    // which makes the seek bar invisible and causes video render frames
    // to be skipped. See controller.cpp::update_state.
    int video_active_negative_count_ = 0;

    // Shuffle queue helpers
    void generate_shuffle_queue(AppState& state, int playlist_size);
    void generate_master_shuffle_queue(AppState& state);
    int get_next_shuffled_index(AppState& state, int playlist_size);
    std::pair<int, int> get_next_master_shuffled_item(AppState& state);
};

} // namespace app
