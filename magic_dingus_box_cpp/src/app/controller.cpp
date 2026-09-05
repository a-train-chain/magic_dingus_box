#include "controller.h"
#include "game_launch_recovery.h"
#include "../video/video_player.h"
#include "../utils/path_resolver.h"
#include "../retroarch/retroarch_launcher.h"
#include "../platform/drm_display.h"
#include "app_state.h"

#include <sstream>
#include <iomanip>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <random>
#include <algorithm>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <json/json.h>
#include "../utils/config.h"

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

    auto run_amixer = [](const std::string& control, const std::string& pct_str) -> int {
        pid_t pid = fork();
        if (pid == -1) return -1;
        if (pid == 0) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
            execlp("amixer", "amixer", "sset", control.c_str(), pct_str.c_str(), nullptr);
            _exit(127);
        }
        int status;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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

bool Controller::poll_seek_request() {
    // Path is process-stable, so resolve it once and cache it — this runs
    // every main-loop tick and the old inline `get_data_path() + "..."`
    // did a getenv plus 2-3 std::string allocations 60×/s for a file that
    // is almost never present. Same idle-fast-path intent as
    // poll_text_input_queue's cached text_input_queue_path_.
    if (seek_request_path_.empty()) {
        seek_request_path_ = config::get_data_path() + "/seek_request.json";
    }
    const std::string& path = seek_request_path_;
    if (!fs::exists(path)) return false;

    Json::Value root;
    {
        std::ifstream f(path);
        if (!f) return false;
        try { f >> root; } catch (...) {
            fs::remove(path);
            return false;
        }
    }
    fs::remove(path);  // consume — even if invalid, don't loop on a bad file

    if (!root.isMember("pos")) return false;
    double frac = root["pos"].asDouble();
    if (!(frac >= 0.0 && frac <= 1.0)) return false;  // also rejects NaN

    double dur = get_duration();
    if (!(dur > 0.0)) return false;

    seek_absolute(frac * dur);
    return true;
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
    // An item's `end` trims playback short: treat it as the effective duration
    // so the existing auto-advance path fires there instead of at the real end
    // of the file. Clamped to the real duration, because an `end` past the end
    // of the media would otherwise never be reached and would hang the item.
    double effective_duration = cur_duration;
    {
        const PlaylistItem* cur = current_item(state);
        if (cur && cur->end > 0.0 && cur->end < cur_duration) {
            effective_duration = cur->end;
        }
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
    
           // Capture original volume when video becomes active (only once)
           if (!was_active && state.video_active) {
               // Only capture if it hasn't been set yet (default is 100.0)
               // Also ensure volume is at 100% when capturing (in case it was dimmed from previous playlist)
               if (state.original_volume == 100.0) {
                   // Restore volume to 100% first to ensure we capture the correct original volume
                   set_volume(100.0);
                   state.original_volume = get_volume();
               }
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

void Controller::wait_for_playback_start(int max_ms, std::function<void()> progress_callback) {
    auto start = std::chrono::steady_clock::now();
    auto budget = std::chrono::milliseconds(max_ms);
    while (std::chrono::steady_clock::now() - start < budget) {
        // Tick the player state machine so is_playing_ can flip: drains the
        // GStreamer bus and does a 0-timeout state poll (which counts
        // ASYNC+pending==PLAYING as playing — see the backend's update_state()).
        player_->update_state();
        if (is_playing()) return;  // started — no reason to keep waiting
        if (progress_callback) progress_callback();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
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

    if (item.source_type == "local") {
        std::cout << "Starting playlist transition..." << std::endl;

        // Stop current playback
        stop();
        std::cout << "Stopped playback, loading new file..." << std::endl;

        // Brief delay for MPV to stop
        wait_with_callback(200, progress_callback);

        // Load the new file
        std::cout << "Loading file: " << item.path << std::endl;
        // Trim points from the playlist. These were hardcoded to 0.0/0.0, so a
        // playlist authored with in/out points played every item in full.
        auto load_result = load_file_with_resolution(item.path, playlist_directory,
                                                     item.start, item.end, false);
        if (load_result) {
            std::cout << "File loaded successfully, starting playback..." << std::endl;

            play();

            // Wait for playback to start — but exit the moment it does
            // instead of always burning the full budget. GstPlayer::
            // update_state() treats ASYNC+pending==PLAYING as playing, so
            // this typically exits on the first tick (~16ms) where the old
            // fixed wait_with_callback(1000) froze the render thread a full
            // second every launch. The 1000ms cap and the warning on
            // timeout are unchanged.
            wait_for_playback_start(1000, progress_callback);

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
    } else if (item.source_type == "video") {
        // Treat "video" as an alias for "local"
        std::cout << "Starting playlist transition (source_type=video)..." << std::endl;

        stop();
        wait_with_callback(200, progress_callback);

        std::cout << "Loading file: " << item.path << std::endl;
        // Trim points from the playlist. These were hardcoded to 0.0/0.0, so a
        // playlist authored with in/out points played every item in full.
        auto load_result = load_file_with_resolution(item.path, playlist_directory,
                                                     item.start, item.end, false);
        if (load_result) {
            std::cout << "File loaded successfully, starting playback..." << std::endl;
            play();
            // Early-exit poll — see the "local" branch above for rationale.
            wait_for_playback_start(1000, progress_callback);
            if (!is_playing()) {
                std::cerr << "Warning: Playback did not start after load" << std::endl;
            }
            return utils::Result<>::ok();
        } else {
            std::string error = "Failed to load playlist item: " + item.path + " (" + load_result.error() + ")";
            std::cerr << "Error: " << error << std::endl;
            return utils::Result<>::fail(error);
        }
    } else if (item.source_type == "emulated_game") {
        // Bracket the whole session — validation early-returns included —
        // with the hooks installed via set_game_session_hooks. The end
        // hook re-enables the systemd watchdog and joins the GPIO poll
        // thread, so it MUST fire on every exit path; the guard makes
        // that hold for exceptions too.
        if (game_session_begin_) game_session_begin_(item);
        struct GameSessionEndGuard {
            const std::function<void()>& end;
            ~GameSessionEndGuard() { if (end) end(); }
        } game_session_end_guard{game_session_end_};

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
        
        // Resolve "auto" core based on system.
        //
        // Keep in sync with ROM_CORE_MAP in magic_dingus_box/web/static/manager.js.
        // These two lists drifted apart: the web admin wrote emulator_core:
        // 'auto' for any system it did not know, and this resolver knew the same
        // seven systems, so an N64 ROM added from the ROM library produced a
        // playlist entry that looked correct everywhere until the user selected
        // it and got "Could not resolve auto core for system: n64".
        // Note the names here carry no _libretro suffix; the playlist YAML does.
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
            } else if (system == "n64") {
                // Both mupen64plus_next and parallel_n64 are installed; next is
                // the one the emulator smoke test exercises and the one the
                // shipped N64 playlist uses.
                core_name = "mupen64plus_next";
            } else if (system == "dreamcast") {
                core_name = "flycast";
            } else {
                std::string error = "Could not resolve auto core for system: " + system;
                std::cerr << "Error: " << error << std::endl;
                return utils::Result<>::fail(error);
            }
            std::cout << "Resolved auto core for " << system << " -> " << core_name << std::endl;
        }
        
        // Validate ROM path is not empty
        if (item.path.empty()) {
            std::string error = "No ROM path specified for game: " + item.title;
            std::cerr << "Error: " << error << std::endl;
            return utils::Result<>::fail(error);
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
        
        // Launch-screen content. Set before the first frame so the very first
        // thing on screen already names the game — a plate with the title and
        // system on it reads as deliberate even when it stops updating, which
        // a bare spinner never does.
        state.loading_title = item.title;
        state.loading_system = item.emulator_system;
        state.loading_phase = "STOPPING VIDEO";
        state.loading_progress.store(0.15f);

        // Stop GStreamer completely before launching RetroArch
        // This ensures all resources (EGL, DRM, threads) are released
        if (player_) {
            std::cout << "Cleaning up GStreamer pipeline before RetroArch launch..." << std::endl;
            player_->cleanup();
            // Wait a moment for cleanup to finish? cleanup() should be synchronous for pipeline destruction.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } // Wait for stop to complete
        wait_with_callback(300, progress_callback);
        state.loading_progress.store(0.35f);
        
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
        
        // ---- Everything DRM-INDEPENDENT happens FIRST, while we can still
        // ---- draw. See app_state.h: from release_master() onward the panel
        // ---- holds whatever frame we presented last, for ~2.5s, and nothing
        // ---- can update it. Input teardown and the udev controller wake-up
        // ---- have nothing to do with the display, and doing them after the
        // ---- handoff spent 0.65s of that frozen window for no reason
        // ---- (measured: DRM released 01.205, launcher started 01.963).
        // ---- Relative order between these two is preserved: release the grab
        // ---- first, then re-trigger, so udev re-enumerates ungrabbed devices.

        // CRITICAL: Release controller input grab before launching RetroArch
        // This ensures the main app doesn't block RetroArch from accessing the controller
        if (input_manager_) {
            std::cout << "Releasing input devices for RetroArch..." << std::endl;
            input_manager_->cleanup();
            std::cout << "Input devices released" << std::endl;
        }
        state.loading_progress.store(0.55f);
        state.loading_phase = "RELEASING INPUT";
        if (progress_callback) progress_callback();

        // CRITICAL: Wake up controller before launching RetroArch
        // Controller may be in sleep mode after GStreamer/DRM cleanup
        std::cout << "Waking up controller before RetroArch launch..." << std::endl;
        auto run_udevadm = [](const char* match) {
            pid_t pid = fork();
            if (pid == 0) {
                int devnull = open("/dev/null", O_WRONLY);
                if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
                execlp("sudo", "sudo", "udevadm", "trigger", "--action=change",
                       match, nullptr);
                _exit(127);
            }
            if (pid > 0) { int s; waitpid(pid, &s, 0); }
        };
        run_udevadm("--sysname-match=js*");
        if (progress_callback) progress_callback();
        run_udevadm("--sysname-match=event*");
        state.loading_progress.store(0.75f);
        state.loading_phase = "WAKING CONTROLLER";
        wait_with_callback(200, progress_callback);
        std::cout << "Controller wake-up signal sent" << std::endl;

        // NOTE: the display is NOT released here. It is handed over from the
        // before_fork hook below, once the launcher has finished writing its
        // script — see LaunchOptions::before_fork. Everything between this
        // point and the fork needs no display, and releasing early spent that
        // whole stretch showing a frozen frame for no reason.

        // Launch the game (BLOCKING)
        retroarch::GameLaunchInfo game_info = {
            resolved_rom_path,
            core_name,
            overlay_path
        };

        retroarch::LaunchOptions opts;
        opts.display_mode = state.display_settings.mode;
        opts.pi_model = state.platform_profile.model;
        {
            int idx = state.display_settings.bezel_index;
            if (idx >= 0 && idx < static_cast<int>(state.available_bezels.size())) {
                opts.bezel_file = state.available_bezels[idx].file;
            }
        }

        // Hand the display over at the last possible instant: after the
        // launcher has written its script, immediately before the fork. This
        // is the final frame the kiosk draws for this launch — the panel holds
        // it until RetroArch takes over KMS — so the bar is presented FULL.
        // That is honest as well as better-looking: everything the kiosk
        // controls really has finished by the time this runs.
        opts.before_fork = [this, &state, &progress_callback]() {
            state.loading_progress.store(1.0f);
            state.loading_phase = "STARTING";
            if (progress_callback) {
                progress_callback();
            }
            if (display_) {
                // CRITICAL: Keep CRTC enabled (disable_crtc = false) for Vulkan
                // compatibility. Disabling it causes "QueuePresent failed" on
                // startup for most cores (Genesis, SNES, NES, PS1). We rely on
                // pkill and display restoration logic for clean exit.
                const bool disable_crtc = false;
                std::cout << "Releasing DRM master for RetroArch (disable_crtc="
                          << disable_crtc << ")..." << std::endl;
                display_->release_master(disable_crtc);
                std::cout << "DRM master released" << std::endl;
                // Let DRM resources settle before RetroArch grabs the display.
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        };

        bool launched = retroarch_launcher_.launch_game(game_info, current_system_volume_, state.audio_settings.retroarch_volume_offset_db, static_cast<int>(state.audio_settings.output), opts);
        
        // Game has exited. Restore system.
        //
        // Deliberately do NOT touch loading_progress / loading_phase here.
        // The kiosk's own framebuffer still holds the last frame it drew — a
        // full gold bar reading "STARTING" — and re-acquiring DRM master puts
        // that exact frame straight back on screen. The dissolve below fades
        // out THAT frame, so the state it renders from must stay identical to
        // the state it was drawn from: the first dissolve frame is then
        // pixel-identical to what is already on the panel, and the stale
        // frame reads as frame 1 of a deliberate fade instead of a hang.

        // CRITICAL: Ensure RetroArch is truly dead before we try to take back control
        // This prevents "zombie" processes from holding onto DRM/Input resources
        std::cout << "RetroArch exited. Ensuring process termination..." << std::endl;
        {
            pid_t pid = fork();
            if (pid == 0) {
                int devnull = open("/dev/null", O_WRONLY);
                if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
                execlp("pkill", "pkill", "-9", "retroarch", nullptr);
                _exit(127);
            }
            if (pid > 0) { int s; waitpid(pid, &s, 0); }
        }
        
        // Fixed settle before re-acquiring DRM master, so RetroArch has fully
        // released DRM and kernel resources. Load-bearing and hardware-tuned;
        // see the follow-on in the 2026-08-02 graceful-exit spec before
        // replacing this with a bounded poll.
        constexpr std::chrono::milliseconds kRetroArchSettleDelay{1000};
        std::cout << "Waiting for system to settle..." << std::endl;
        std::this_thread::sleep_for(kRetroArchSettleDelay);

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
            
            // Restore the mode the kiosk actually booted with — NOT 640x480.
            // No unit boots at 640x480 (target_drm_mode gives 1280x720 /
            // 1920x1080; 640x480 is only the boot cascade's last resort), so
            // hardcoding it made every game exit do TWO mode changes: down to
            // 640x480 here, then back up in main.cpp's reset_display handler
            // — two black-screen TV resyncs per exit. On success the
            // display_mode_restored flag tells that handler to skip its own
            // set_mode, making this the ONLY mode change on the way back.
            bool mode_restored = false;
            if (kiosk_mode_w_ > 0 && kiosk_mode_h_ > 0) {
                std::cout << "Restoring kiosk display mode "
                          << kiosk_mode_w_ << "x" << kiosk_mode_h_ << "..." << std::endl;
                mode_restored = display_->set_mode(kiosk_mode_w_, kiosk_mode_h_);
            }
            if (!mode_restored) {
                // Legacy floor, preserved for the never-configured case and
                // for a failed restore. Gives the dissolve SOMETHING to draw
                // on; deliberately does NOT set the flag below.
                std::cout << "Falling back to 640x480..." << std::endl;
                display_->set_mode(640, 480);
            }
            state.display_mode_restored.store(mode_restored);

            // First frames we may draw since the handover. MUST stay after
            // set_mode() — painting before it blocks on a page-flip event
            // that never arrives (measured: launches 4.3s -> 15.8s).
            //
            // Dissolve the launch plate to black. Hold at alpha 1.0 first:
            // the set_mode above makes many TVs blank for 300ms+ while HDMI
            // re-locks, and without the hold the ramp can finish entirely
            // behind that blank — the panel would re-light already on black
            // and the dissolve would read as a hard cut. Held, the panel
            // re-lights on the stable gold plate (continuity with what was
            // on screen before the resync) and THEN it fades. The held/ramp
            // frames are pixel-identical to the stale frame the scanout
            // picked up when RetroArch died, so the transition starts from
            // exactly what is on the panel. progress_callback ends in
            // present_frame(), which blocks on the page flip, so the loop is
            // naturally paced at the refresh rate.
            //
            // Guarded on progress_callback: only the Settings game-browser
            // route passes one; the other four routes render no launch plate
            // at all, and for them the return is the post-game fade alone.
            // Skipped when DRM master was never re-acquired: present_frame
            // cannot vblank-pace the loop then, and the box is already in a
            // degraded state where a transition is the least of its problems.
            if (acquired && progress_callback) {
                constexpr std::chrono::milliseconds kReturnDissolveHold{120};
                constexpr std::chrono::milliseconds kReturnDissolveRamp{250};
                const auto dissolve_t0 = std::chrono::steady_clock::now();
                for (;;) {
                    const auto elapsed =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - dissolve_t0);
                    if (elapsed >= kReturnDissolveHold + kReturnDissolveRamp) break;
                    // Negative pre-ramp elapsed clamps to 1.0 — the hold
                    // falls out of the ramp function.
                    state.loading_alpha.store(app::return_dissolve_alpha(
                        static_cast<float>((elapsed - kReturnDissolveHold).count()),
                        static_cast<float>(kReturnDissolveRamp.count())));
                    progress_callback();
                }
                state.loading_alpha.store(0.0f);
                progress_callback();  // hold solid black for the restore work
            }
        }

        // Re-initialize input devices after RetroArch exits with retry logic
        if (input_manager_) {
            std::cout << "Re-initializing input devices after RetroArch..." << std::endl;
            
            bool input_initialized = false;
            for (int i = 0; i < 3; ++i) {
                // Re-wake controller before initializing
                run_udevadm("--sysname-match=js*");
                run_udevadm("--sysname-match=event*");
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
                std::cerr << "CRITICAL: Failed to re-initialize input devices after 3 retries!" << std::endl;
                // Last-resort attempt: sleep longer and try once more
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                run_udevadm("--sysname-match=js*");
                run_udevadm("--sysname-match=event*");
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (input_manager_->initialize()) {
                    std::cout << "Input devices initialized on final retry." << std::endl;
                } else {
                    std::cerr << "CRITICAL: Input devices permanently failed. Controller may not work." << std::endl;
                }
            }
        }

        // Restore PulseAudio default sink to match user's audio output preference
        // RetroArch uses ALSA directly, so PulseAudio state may have drifted
        std::cout << "Restoring audio output after RetroArch..." << std::endl;
        state.audio_settings.apply_output();

        // The video pipeline and playback indices are invalid after either a
        // completed game or a failed/timed-out startup. Normalize both paths
        // before returning so the main loop can always rebuild the kiosk UI.
        if (player_) {
            player_->stop();
            std::cout << "Stopped video player after RetroArch launch" << std::endl;
        }
        prepare_kiosk_state_after_game(state);

        if (launched) {
            std::cout << "Successfully launched game: " << item.title << std::endl;
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
            // Per-playlist loop wins for VIDEO playlists; game playlists keep
            // the global setting, since the `loop:` key is meaningless for them
            // (selecting a game hands off to RetroArch rather than running
            // through the video pipeline).
            const bool effective_loop = playlist.is_game_playlist()
                                            ? state.playlist_loop
                                            : playlist.loop;
            if (effective_loop) {
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
        // If load failed, try skipping to next valid item (up to playlist size attempts)
        std::cerr << "Warning: Failed to load item " << (state.current_item_index + 1)
                  << ": " << load_result.error() << ", skipping..." << std::endl;
        state.last_advanced_item_index = -1;  // Reset advance flag to allow retry

        int attempts = 0;
        int max_attempts = static_cast<int>(playlist.items.size());
        bool found = false;

        while (attempts < max_attempts && !found) {
            state.current_item_index = (state.current_item_index + 1) % playlist.items.size();
            if (state.current_item_index == old_index) break; // Wrapped around, give up

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
            } else {
                std::cerr << "Warning: Also failed item " << (state.current_item_index + 1)
                          << ": " << retry.error() << std::endl;
            }
            attempts++;
        }

        if (!found) {
            // All items failed - stop and show UI
            std::cerr << "All playlist items failed to load, stopping." << std::endl;
            stop();
            state.video_active = false;
            state.ui_visible_when_playing = true;
            state.set_error("No playable content in playlist");
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

void Controller::play_random_global_video(AppState& state, const std::string& playlist_directory, int depth) {
    if (depth > 5) {
        std::cerr << "Error: play_random_global_video exceeded max retry depth" << std::endl;
        return;
    }

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
        play_random_global_video(state, playlist_directory, depth + 1);
        return;
    }
    
    const auto& playlist = state.playlists[playlist_index];
    
    if (item_index < 0 || item_index >= static_cast<int>(playlist.items.size())) {
        std::cerr << "Error: Invalid item index from master shuffle queue" << std::endl;
        // Regenerate and retry
        state.master_shuffle_queue.clear();
        play_random_global_video(state, playlist_directory, depth + 1);
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
        play_random_global_video(state, playlist_directory, depth + 1);
    }
}

// Generate a shuffled queue of indices for the current playlist
// Uses Fisher-Yates shuffle to randomize order
void Controller::generate_shuffle_queue(AppState& state, int playlist_size) {
    // Create sequential indices
    state.shuffle_queue.clear();
    state.shuffle_queue.reserve(playlist_size);
    for (int i = 0; i < playlist_size; ++i) {
        state.shuffle_queue.push_back(i);
    }

    // Fisher-Yates shuffle
    std::shuffle(state.shuffle_queue.begin(), state.shuffle_queue.end(), rng_);
    
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
    state.master_shuffle_queue.clear();

    // Collect all items from all playlists (skip playlist 0 which is Master Shuffle itself)
    for (size_t playlist_idx = 1; playlist_idx < state.playlists.size(); ++playlist_idx) {
        const auto& playlist = state.playlists[playlist_idx];
        for (size_t item_idx = 0; item_idx < playlist.items.size(); ++item_idx) {
            state.master_shuffle_queue.emplace_back(static_cast<int>(playlist_idx), static_cast<int>(item_idx));
        }
    }

    // Fisher-Yates shuffle
    std::shuffle(state.master_shuffle_queue.begin(), state.master_shuffle_queue.end(), rng_);
    
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
