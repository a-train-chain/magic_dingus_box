#include "intro_sequence_kiosk.h"

#include "controller.h"
#include "intro_sequence.h"
#include "../platform/drm_display.h"
#include "../platform/egl_context.h"
#include "../platform/gpio_manager.h"
#include "../utils/config.h"
#include "../video/gst_player.h"
#include "../video/gst_renderer.h"

#include <GLES3/gl3.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace app {

std::string find_intro_video() {
    // Look for intro video in common locations (prefer .30fps version)
    std::vector<std::string> intro_paths = config::get_intro_search_paths();

    std::string intro_video_path;
    std::cout << "Checking for intro video in " << intro_paths.size() << " locations..." << std::endl;
    for (const auto& path : intro_paths) {
        std::cout << "  Checking: " << path << std::endl;
        // Try direct path check first (avoids path resolver warnings)
        if (fs::exists(path)) {
            try {
                fs::path canonical_path = fs::canonical(path);
                intro_video_path = canonical_path.string();
                std::cout << "Found intro video: " << intro_video_path << std::endl;
                break;
            } catch (const std::exception&) {
                // Canonical failed, try absolute path
                fs::path abs_path = fs::absolute(path);
                if (fs::exists(abs_path)) {
                    intro_video_path = abs_path.string();
                    std::cout << "Found intro video: " << intro_video_path << std::endl;
                    break;
                }
            }
        }
    }
    return intro_video_path;
}

void start_intro(const std::string& intro_video_path, AppState& state,
                 Controller& controller, video::GstPlayer& player,
                 video::GstRenderer& gst_renderer,
                 const std::string& playlist_directory) {
    // Load and play intro video if found
    // IMPORTANT: Set showing_intro_video BEFORE loading to prevent UI from appearing
    if (!intro_video_path.empty()) {
        // Set intro state immediately to prevent UI from rendering
        state.showing_intro_video = true;
        state.intro_ready = false;  // Not ready until video actually starts
        state.ui_visible_when_playing = false;  // UI transparent during intro
        state.video_active = false;  // Will be set to true when video actually starts

        auto intro_result = controller.load_file_with_resolution(intro_video_path, playlist_directory, 0.0, 0.0, false);
        if (intro_result) {
            controller.play();
            std::cout << "Intro video loaded, waiting for playback to start..." << std::endl;

            // Wait for intro video to actually start playing AND render at least one frame
            // This ensures the first thing user sees is the video, not UI or blank screen
            // Increased timeout to 10s (200 * 50ms) to allow for slower startup on Pi
            int wait_count = 0;
            const int max_wait = 200;  // Wait up to 10 seconds
            bool first_frame_rendered = false;

            while ((!state.intro_ready || !first_frame_rendered) && wait_count < max_wait) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                controller.update_state(state);

                // Check if mpv has rendered a frame
                if (state.intro_ready && !first_frame_rendered) {
                    // Check if frame is ready - let main loop handle actual rendering
                    // This prevents race condition with main loop's buffer management
                    uint64_t flags = gst_renderer.get_update_flags();
                    if (flags & video::GstRenderer::UPDATE_FRAME) {
                        // Frame is ready - mark as rendered and let main loop handle it
                        // DON'T call gst_renderer.render() or egl.swap_buffers() here!
                        // The main loop will handle all rendering and buffer swapping
                        first_frame_rendered = true;
                        std::cout << "Intro video first frame ready, entering main loop" << std::endl;
                    }
                }

                wait_count++;
            }

            if (state.intro_ready && first_frame_rendered) {
                std::cout << "Intro video ready with first frame, entering main loop" << std::endl;
            } else {
                std::cerr << "Warning: Intro video did not start within timeout, proceeding anyway" << std::endl;
                // Force skip intro if it timed out to prevent black screen
                state.showing_intro_video = false;
                state.intro_complete = true;
                state.video_active = false;
                player.stop();
            }
        } else {
            std::cerr << "Warning: Failed to load intro video, skipping intro" << std::endl;
            state.showing_intro_video = false;  // Reset if load failed
            state.intro_complete = true;  // Skip intro if file can't be loaded
        }
    } else {
        std::cout << "No intro video found, starting with UI" << std::endl;
        state.intro_complete = true;  // No intro video, show UI immediately
    }
}

void tick_intro(AppState& state, Controller& controller,
                platform::GpioManager& gpio, platform::EglContext& egl,
                const platform::DisplayMode& mode) {
    // Handle intro video completion
    // When intro video ends, fade it out first, then fade in the UI
    if (state.showing_intro_video && state.video_active && state.get_duration() > 0.0) {
        const double intro_position = state.get_position();
        const double intro_duration = state.get_duration();
        // Update LED dance animation during intro video
        // Use video position as elapsed time (in milliseconds)
        gpio.update_intro_animation(static_cast<uint64_t>(intro_position * 1000));
        // Check if intro video has ended (position near the end, or the
        // player stopped after making progress) — see intro_video_ended.
        const bool video_ended = intro_video_ended(
            intro_position, intro_duration, controller.is_playing());

        if (video_ended && !state.intro_fading_out) {
            state.intro_fading_out = true;
            state.intro_fade_out_start_time = std::chrono::steady_clock::now();
            std::cout << "Intro video completed, starting fade-out..." << std::endl;
        }
    }

    // Handle intro video fade-out
    if (state.intro_fading_out) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - state.intro_fade_out_start_time);

        if (elapsed >= kIntroFadeOut) {
            // Fade-out complete - stop video and start UI fade-in
            state.intro_fading_out = false;
            state.showing_intro_video = false;
            state.intro_complete = true;

            // Stop the intro video completely
            controller.stop();

            std::cout << "Intro video stopped, transition to UI complete" << std::endl;

            // Force immediate UI rendering by clearing and ensuring clean transition
            glViewport(0, 0, mode.width, mode.height);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // Force multiple buffer swaps to ensure the clear takes effect
            for (int i = 0; i < 3; i++) {
                if (!egl.swap_buffers()) {
                    std::cerr << "Failed to swap buffers during intro transition!" << std::endl;
                }
            }

            std::cout << "Intro transition complete - video stopped, renderer cleaned, screen cleared, UI ready" << std::endl;

            // Stop LED intro animation
            gpio.stop_animation();

            // Verify that video actually stopped before proceeding
            int retry_count = 0;
            const int max_retries = 20;  // Increased retries
            while (controller.is_playing() && retry_count < max_retries) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));  // Longer delay
                controller.update_state(state);  // Update state to get current playing status
                retry_count++;
                if (retry_count % 5 == 0) {
                    std::cout << "DEBUG: Waiting for video to stop... attempt " << retry_count << ", is_playing=" << controller.is_playing() << std::endl;
                }
            }

            if (controller.is_playing()) {
                std::cerr << "Warning: Intro video did not stop cleanly after " << (max_retries * 100) << "ms, proceeding anyway" << std::endl;
            } else {
                std::cout << "DEBUG: Intro video stopped successfully after " << retry_count << " attempts" << std::endl;
            }

            // Stopped, de-indexed playback state + the menu fade-in (not
            // stop_to_menu — see hand_intro_to_menu).
            hand_intro_to_menu(state, std::chrono::steady_clock::now());

            std::cout << "Intro video fade-out complete, fading in UI..." << std::endl;
        } else {
            // Fade-out in progress - ramp the intro's volume down to 0.
            controller.set_volume(intro_fade_out_volume(state.original_volume, elapsed));
        }
    }
}

}  // namespace app
