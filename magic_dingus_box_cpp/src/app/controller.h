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
    // Default constructor for unit tests (player_ = nullptr, all player ops are no-ops).
    Controller() : player_(nullptr), display_(nullptr), input_manager_(nullptr) {}
    
    // Set display reference for DRM cleanup before RetroArch launch
    void set_display(platform::DrmDisplay* display) { display_ = display; }
    
    // Set input manager reference for controller release before RetroArch launch
    void set_input_manager(platform::InputManager* input_manager) { input_manager_ = input_manager; }

    // RetroArch session bracketing hooks — installed once from main().
    // begin runs at the top of load_playlist_item's emulated_game branch;
    // end is guaranteed to run when the branch exits, on every path
    // (normal return, validation early-return, exception). They carry the
    // side effects that must wrap EVERY game session — systemd watchdog
    // disable/re-enable, phone-remote status writes, GPIO restart-button
    // polling, media-stack quiet mode — so all five launch routes get
    // them (main-UI select on a mixed playlist, NEXT/PREV, auto-advance,
    // Master Shuffle, Settings game browser), not just the Settings
    // branch that used to inline this logic.
    void set_game_session_hooks(std::function<void(const app::PlaylistItem&)> begin,
                                std::function<void()> end) {
        game_session_begin_ = std::move(begin);
        game_session_end_   = std::move(end);
    }
    
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

    // Phone Remote: drain the JSON-Lines queue at the configured path
    // and dispatch each event to state.active_text_keyboard. No-op when
    // file is missing/empty (idle fast path) or pointer is null. The
    // file is truncated after parsing under the same flock.
    void poll_text_input_queue(AppState& state);

    // Set the queue file path. Default is data/text_input_queue.jsonl
    // resolved via config::get_data_path(). Tests inject a temp path.
    void set_text_input_queue_path(std::string path);
    
    // System Volume
    void set_system_volume(int percent);
    
    // Initialize RetroArch launcher
    utils::Result<> initialize_retroarch_launcher();

    // Get RetroArch launcher (for core downloader)
    retroarch::RetroArchLauncher& get_retroarch_launcher() { return retroarch_launcher_; }

    // The display mode the kiosk runs at, for restoring after RetroArch.
    // Set at boot once the final mode is known, and again whenever the
    // CRT_NATIVE <-> MODERN_TV toggle changes it at runtime — otherwise the
    // stored value goes stale the first time an operator flips display mode.
    void set_kiosk_display_mode(uint32_t w, uint32_t h) {
        kiosk_mode_w_ = w;
        kiosk_mode_h_ = h;
    }

private:
    // Poll until playback starts, up to max_ms. Ticks the player's state
    // machine (bus drain + non-blocking state poll) and the optional
    // progress callback every ~16ms, returning as soon as is_playing()
    // flips true. Replaces fixed-length wait_with_callback(1000) calls in
    // load_playlist_item that froze the render thread for the full budget
    // even when playback had already started.
    void wait_for_playback_start(int max_ms, std::function<void()> progress_callback);

    video::VideoPlayer* player_;
    retroarch::RetroArchLauncher retroarch_launcher_;
    std::function<void(const app::PlaylistItem&)> game_session_begin_;
    std::function<void()> game_session_end_;

    // Mode to restore after RetroArch exits. 0 = never set (fall back to
    // the legacy 640x480 floor).
    uint32_t kiosk_mode_w_ = 0;
    uint32_t kiosk_mode_h_ = 0;

    // Deferred amixer apply (see set_system_volume): the fork+exec pair
    // runs from update_state(), at most once per call, so a burst of
    // rotary detents in one frame costs ONE amixer pair instead of one
    // per event — and identical re-applies (knob pinned at 0/100) cost
    // nothing. -1 = nothing pending / never applied.
    void apply_system_volume_now(int percent);
    int pending_system_volume_ = -1;
    int last_applied_system_volume_ = -1;
    std::string text_input_queue_path_;
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
