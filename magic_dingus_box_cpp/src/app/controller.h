#pragma once

#include "app_state.h"
#include "text_queue_drainer.h"
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
    // begin runs in load_playlist_item's emulated_game branch right AFTER
    // validate_game_launch passes (a launch that can't happen must not
    // quiet the media stack); end is guaranteed to run once begin has, on
    // every path (normal return, launch failure, exception). They carry the
    // side effects that must wrap EVERY game session — systemd watchdog
    // disable/re-enable around the teardown and restore phases, phone-remote status writes, GPIO restart-button
    // polling, media-stack quiet mode — so all five launch routes get
    // them (main-UI select on a mixed playlist, NEXT/PREV, auto-advance,
    // Master Shuffle, Settings game browser), not just the Settings
    // branch that used to inline this logic.
    void set_game_session_hooks(std::function<void(const app::PlaylistItem&)> begin,
                                std::function<void()> end) {
        game_session_begin_ = std::move(begin);
        game_session_end_   = std::move(end);
    }

    // systemd watchdog control for the supervised play phase of a game
    // (see retroarch::LaunchOptions::watchdog). Installed once from main();
    // empty on dev machines / without libsystemd.
    void set_session_watchdog(std::function<void(retroarch::SessionWatchdog)> fn) {
        session_watchdog_ = std::move(fn);
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
    //
    // load_next_item retries past items that fail to load. A missing file
    // is skipped for the cost of a stat (no stop, no settle). An item that
    // exists but fails costs a stop + the 200 ms settle, so at most
    // kMaxSyncLoadRetries of those are retried in one call (after the
    // first attempt): then it sets state.playlist_item_load_failed and
    // returns, and PlaylistPlayback continues on the next frame through
    // the playback error policy. Keeps one call well inside the 10 s
    // systemd watchdog however long the run of bad items is.
    static constexpr int kMaxSyncLoadRetries = 3;
    void load_next_item(AppState& state, const std::string& playlist_directory);
    void load_previous_item(AppState& state, const std::string& playlist_directory);
    
    // Play a random video from any playlist (Master Shuffle)
    void play_random_global_video(AppState& state, const std::string& playlist_directory, int depth = 0);
    // Master Shuffle NEXT / auto-advance / failed-item skip: record the item
    // being left in the PREV history, then play_random_global_video.
    void master_shuffle_advance(AppState& state, const std::string& playlist_directory);
    // Master Shuffle PREV: back to the most recent still-valid history
    // entry, or a random pick when there is none (app/shuffle_queue.h).
    void master_shuffle_back(AppState& state, const std::string& playlist_directory);
    
    // Audio recovery
    // Polled each main-loop tick. Reads data/seek_request.json if present,
    // seeks to (pos * duration), then deletes the file. No-op if the file
    // doesn't exist or contains an invalid pos. Cheap when no file exists
    // (single fs::exists check). Returns true only when a seek was
    // actually issued — main.cpp uses that to give the Media Browser's
    // PlaybackScreen the same post-seek EOS-flicker suppression its own
    // seek handlers get (a phone tap-to-seek is invisible to it otherwise).
    bool poll_seek_request();

    // Phone Remote: dispatch queued phone keystrokes to
    // state.active_text_keyboard (dropped when it is null). Called every
    // frame, and does NO blocking file work itself: it dispatches what the
    // drain worker has read so far, then stats the JSON-Lines queue and —
    // when it is not empty — asks the worker to lock, read and truncate it
    // (app/text_queue_drainer.h). Keystrokes therefore reach the keyboard
    // on the frame AFTER the one that noticed them, in order.
    void poll_text_input_queue(AppState& state);

    // Set the queue file path. Default is data/text_input_queue.jsonl
    // resolved via config::get_data_path(). Tests inject a temp path.
    void set_text_input_queue_path(std::string path);

    // Test seams. wait_for_text_input_drain() blocks until the drain
    // worker is idle (a test then polls once more to dispatch);
    // set_text_input_drain_for_tests() replaces the lock/read/truncate.
    void wait_for_text_input_drain() { text_drainer_.wait_idle(); }
    void set_text_input_drain_for_tests(TextQueueDrainer::DrainFn fn) {
        text_drain_fn_ = std::move(fn);
    }
    
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

    // True once the kiosk has lost the display for good: DRM master could
    // not be re-acquired after a game (5 retries). Nothing can be drawn and
    // no in-process retry is known to recover, but the render loop would
    // keep pinging the systemd watchdog in front of a black screen forever.
    // main.cpp must check this after every load_playlist_item() and exit
    // NON-ZERO so Restart=on-failure brings up a fresh kiosk that opens the
    // display from scratch. Sticky: never cleared.
    bool display_lost() const { return display_lost_; }

    // Deterministic shuffle order for the unit tests (the kiosk seeds from
    // std::random_device at construction).
    void seed_shuffle_rng(uint32_t seed) { rng_.seed(seed); }

private:
    bool display_lost_ = false;

    // Poll until playback starts, up to max_ms. Ticks the player's state
    // machine (bus drain + non-blocking state poll) and the optional
    // progress callback every ~16ms, returning as soon as is_playing()
    // flips true. Replaces fixed-length wait_with_callback(1000) calls in
    // load_playlist_item that froze the render thread for the full budget
    // even when playback had already started.
    void wait_for_playback_start(int max_ms, std::function<void()> progress_callback);

    // The media half of load_playlist_item, after the index check, the
    // now-playing publish and the Master Shuffle check: stop/settle/load/
    // play for a video item, the RetroArch hand-off for a game. Kiosk-only
    // (controller.cpp); load_playlist_item itself is in
    // controller_playback.cpp so the stepping code can be linked into the
    // Mac tests, which supply their own definition of this function.
    utils::Result<> load_item_media(AppState& state, const app::PlaylistItem& item,
                                    const std::string& playlist_directory,
                                    std::function<void()> progress_callback);

    video::VideoPlayer* player_;
    retroarch::RetroArchLauncher retroarch_launcher_;
    std::function<void(const app::PlaylistItem&)> game_session_begin_;
    std::function<void()> game_session_end_;
    std::function<void(retroarch::SessionWatchdog)> session_watchdog_;

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
    std::string seek_request_path_;  // cached in poll_seek_request (per-tick)
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

    // Shuffle ordering lives in app/shuffle_queue.h (pure, tested).

    // Phone keyboard queue: the lock/read/truncate runs on text_drainer_'s
    // worker thread, never on the render loop. text_drain_fn_ is empty in
    // the kiosk (the real file drain is used). Keep text_drainer_ the LAST
    // member so its thread is joined before anything else is destroyed.
    TextQueueDrainer::DrainFn text_drain_fn_;
    TextQueueDrainer text_drainer_;
};

} // namespace app
