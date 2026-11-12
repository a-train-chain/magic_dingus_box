#include "controller.h"
#include "game_launch_recovery.h"
#include "game_launch_validation.h"
#include "playback_reset.h"
#include "auto_advance.h"
#include "playlist_cursor.h"
#include "shuffle_queue.h"
#include "../video/video_player.h"
#include "../utils/path_resolver.h"
#include "../retroarch/retroarch_launcher.h"
#include "../retroarch/launch_contract.h"
#include "../platform/drm_display.h"
#include "app_state.h"

#include <sstream>
#include <vector>
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
#include "../platform/udev_wake.h"
#include "../utils/subprocess.h"

namespace fs = std::filesystem;

namespace app {

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

void Controller::wait_for_playback_start(int max_ms, std::function<void()> progress_callback) {
    auto start = std::chrono::steady_clock::now();
    auto budget = std::chrono::milliseconds(max_ms);
    while (std::chrono::steady_clock::now() - start < budget) {
        // Tick the player state machine so is_playing_ can flip: drains the
        // GStreamer bus and does a 0-timeout state poll (which counts
        // ASYNC+pending==PLAYING as playing — see GstPlayer::update_state).
        if (player_) {
            player_->update_state();
        }
        if (is_playing()) return;  // started — no reason to keep waiting
        if (player_ && player_->has_error()) return;  // broken file: the
                                    // error policy in main skips it; waiting
                                    // out the budget only delays that
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

utils::Result<> Controller::load_item_media(AppState& state, const app::PlaylistItem& item, const std::string& playlist_directory, std::function<void()> progress_callback) {
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
        // Handle RetroArch game launch
        std::cout << "Launching RetroArch game: " << item.title << std::endl;
        std::cout << "  Core: " << item.emulator_core << std::endl;
        std::cout << "  System: " << item.emulator_system << std::endl;
        std::cout << "  Path: " << item.path << std::endl;

        // Validate BEFORE the session hooks: the begin hook pauses every
        // torrent and stops the arr containers (GameQuietMode), and the end
        // hook undoes it — a missing ROM or core used to churn the whole
        // service stack for a game that never ran. Validation touches
        // nothing (filesystem reads only), so nothing needs the hooks yet.
        auto validated = validate_game_launch(
            item, playlist_directory, [](const std::string& core) {
                return retroarch::RetroArchLauncher::find_core_dir(core).has_value();
            });
        if (!validated) {
            std::cerr << "Error: " << validated.error() << std::endl;
            return utils::Result<>::fail(validated.error());
        }
        const std::string core_name = validated.get().core_name;
        const std::string resolved_rom_path = validated.get().rom_path;
        if (core_name != item.emulator_core) {
            std::cout << "Resolved auto core for " << item.emulator_system
                      << " -> " << core_name << std::endl;
        }

        // Bracket the rest of the session with the hooks installed via
        // set_game_session_hooks. The end hook re-enables the systemd
        // watchdog and joins the GPIO poll thread, so once begin has fired
        // it MUST fire on every exit path; the guard makes that hold for
        // exceptions too.
        if (game_session_begin_) game_session_begin_(item);
        struct GameSessionEndGuard {
            const std::function<void()>& end;
            ~GameSessionEndGuard() { if (end) end(); }
        } game_session_end_guard{game_session_end_};

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
        platform::udev::wake_input_devices();
        if (progress_callback) progress_callback();
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
                // the launcher's session stop and display restoration logic
                // for a clean exit.
                const bool disable_crtc = false;
                std::cout << "Releasing DRM master for RetroArch (disable_crtc="
                          << disable_crtc << ")..." << std::endl;
                display_->release_master(disable_crtc);
                std::cout << "DRM master released" << std::endl;
                // Let DRM resources settle before RetroArch grabs the display.
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        };

        // Feed the systemd watchdog while RetroArch plays (main.cpp wires
        // this to sd_notify); the teardown above and the restore below stay
        // unwatched, bracketed by the session hooks.
        opts.watchdog = session_watchdog_;

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

        // RetroArch is gone and reaped by the time launch_game returns: the
        // launcher spawns it directly, supervises it, and every way a session
        // ends (quit gesture, kiosk SIGTERM, startup timeout) goes through
        // retroarch::stop_game_session(), which TERMs RetroArch, waits for its
        // auto-save, and only then SIGKILLs and sweeps its process group. The
        // `pgrep`/`pkill -x retroarch` safety net that used to live here
        // guessed at a process the kiosk now owns by PID.

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
                // Used to "proceed anyway": the render loop then kept the
                // re-enabled watchdog happy in front of a black screen
                // forever. Flag it so main exits non-zero and systemd
                // (Restart=on-failure) restarts the kiosk from scratch.
                std::cerr << "CRITICAL: Failed to acquire DRM master after retries! "
                             "Display lost; the kiosk must restart." << std::endl;
                display_lost_ = true;
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
                platform::udev::wake_input_devices();
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
                platform::udev::wake_input_devices();
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

        if (display_lost_) {
            std::string error = "Display lost after game (DRM master not re-acquired): " + item.title;
            std::cerr << "Error: " << error << std::endl;
            return utils::Result<>::fail(error);
        }
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


utils::Result<> Controller::initialize_retroarch_launcher() {
    if (!retroarch_launcher_.initialize()) {
        return utils::Result<>::fail("Failed to initialize RetroArch launcher");
    }
    return utils::Result<>::ok();
}

} // namespace app
