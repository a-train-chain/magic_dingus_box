#include "game_handoff_kiosk.h"

#include "controller.h"
#include "../platform/drm_display.h"
#include "../platform/egl_context.h"
#include "../platform/frame_presenter.h"
#include "../platform/gpio_manager.h"
#include "../platform/input_manager.h"
#include "../ui/renderer.h"
#include "../ui/settings_menu.h"
#include "../video/gst_player.h"
#include "../video/gst_renderer.h"

#include <GLES3/gl3.h>

#include <iostream>

namespace app {

void launch_game_from_browser(Controller& controller, AppState& state,
                              ui::SettingsMenuManager& settings_menu,
                              KioskGraphics& gfx, const platform::DisplayMode& mode,
                              const Playlist& playlist, int game_idx,
                              const std::string& playlist_directory) {
    std::cout << "Launching game: " << playlist.items[game_idx].title << std::endl;

    // Create progress callback to keep UI alive during launch
    auto progress_callback = [&]() {
        // Clear screen
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Render loading screen FIRST so the bezel can layer on top
        gfx.ui_renderer.render_loading_overlay(state);

        // Render bezel overlay LAST so it frames the loading screen
        // (same z-order as the main render path at end of frame)
        if (state.display_settings.mode == app::DisplayMode::MODERN_TV &&
            !state.available_bezels.empty() &&
            state.display_settings.bezel_index >= 0 &&
            state.display_settings.bezel_index < static_cast<int>(state.available_bezels.size())) {
            const auto& bezel = state.available_bezels[state.display_settings.bezel_index];
            if (!bezel.file.empty()) {
                gfx.ui_renderer.load_bezel(bezel.file);
                glViewport(0, 0, mode.width, mode.height);
                gfx.ui_renderer.render_bezel();
            }
        }

        // Swap buffers (renders to GBM surface)
        gfx.egl.swap_buffers();

        // Present frame (flips DRM page)
        gfx.frame_presenter.present(mode.width, mode.height);
    };

    // Launch the game. All session bracketing — watchdog
    // disable/re-enable, GPIO poll thread, phone-remote
    // status writes, quiet mode, artwork pause, loading
    // state — happens inside load_playlist_item via the
    // game-session hooks installed at startup, shared with
    // every other launch route.
    auto launch_result = controller.load_playlist_item(state, playlist, game_idx, playlist_directory, progress_callback);

    if (launch_result) {
        std::cout << "Game launched successfully" << std::endl;
    } else {
        std::cout << "Game launch failed: " << launch_result.error() << std::endl;
        state.set_error("Unable to start game");
    }
    // Return to the playlist UI after every launch outcome;
    // a failed takeover must not leave the game browser open.
    //
    // force_close (teleport), NOT close (animate). The call
    // above blocks for the entire game session, so by the time
    // we reach this line RetroArch has already exited and the
    // display handover back to the kiosk is done. close() would
    // start a FRESH 300ms slide-shut right then — outlasting the
    // 250ms post-game fade-up — so the fade revealed the settings
    // menu animating closed instead of the main menu, which reads
    // as a glitch on the way out of every game.
    //
    // Nothing is lost by skipping the animation: the menu was on
    // screen before RetroArch took over, minutes or hours ago, and
    // the user's mental model is "I was in a game, now I'm back",
    // not "I am still in the menu I launched from".
    settings_menu.force_close();
}

void restore_display_after_game(KioskGraphics& gfx, AppState& state,
                                const platform::DisplayMode& mode) {
    std::cout << "Resetting display state after external application..." << std::endl;

    // Re-acquire DRM master (in case it was dropped or stolen)
    if (!gfx.display.acquire_master()) {
        std::cerr << "Warning: Failed to re-acquire DRM master" << std::endl;
    }

    // Force mode restoration (RetroArch might have changed resolution)
    // — unless the game-exit path already did it, in which case a
    // second set_mode here would make the TV resync twice.
    if (state.display_mode_restored.exchange(false)) {
        std::cout << "Display mode already restored by game-exit path; skipping set_mode" << std::endl;
    } else if (!gfx.display.set_mode(mode.width, mode.height)) {
        std::cerr << "Warning: Failed to restore display mode: " << mode.width << "x" << mode.height << std::endl;
    } else {
        std::cout << "Restored display mode: " << mode.width << "x" << mode.height << std::endl;
    }

    // Reset all frame presentation state (framebuffers, GBM buffers, counters)
    gfx.frame_presenter.reset();
    state.reset_display = false;

    // CRITICAL: Re-make EGL context current after RetroArch released it
    // RetroArch uses its own EGL/DRM context, so we need to restore ours
    if (!gfx.egl.make_current()) {
        std::cerr << "Warning: Failed to re-make EGL context current after RetroArch exit" << std::endl;
    } else {
        std::cout << "EGL context restored after RetroArch exit" << std::endl;
    }

    // CRITICAL: Reset GstRenderer GL resources after context restore
    // RetroArch invalidates our textures, shaders, VAOs etc. when it takes over EGL
    // This triggers lazy re-initialization on the next video frame render
    gfx.gst_renderer.reset_gl();

    // CRITICAL CHECK: Has the player been cleaned up?
    if (!gfx.player.is_initialized()) {
        std::cout << "Re-initializing GStreamer player and linking renderer..." << std::endl;
        // Use default initialization as done in main()
        if (!gfx.player.initialize()) {
             std::cerr << "Failed to re-initialize GStreamer player!" << std::endl;
        }
        // Re-link renderer to the new pipeline/appsink
        if (!gfx.gst_renderer.initialize(&gfx.player)) {
            std::cerr << "Failed to re-initialize GStreamer renderer!" << std::endl;
        }
    }

    // Restore audio output after RetroArch
    // 1. Set PulseAudio default sink (for any non-GStreamer streams)
    std::cout << "Restoring audio output after display reset..." << std::endl;
    // apply_output() returns the sink it resolved — reused below
    // instead of listing the sinks a second time on this thread.
    const std::string pulse_device = state.audio_settings.apply_output();

    // 2. Set pulsesink device directly on GStreamer pipeline
    // This bypasses PulseAudio default sink which can be overridden by
    // module-switch-on-port-available or module-default-device-restore
    {
        if (!pulse_device.empty()) {
            gfx.player.set_audio_device(pulse_device);
        }
    }

    // CRITICAL: Also reset UI Renderer GL resources
    // The UI shaders, VAO, VBO, and logo texture also become invalid
    gfx.ui_renderer.reset_gl();

    // Force an immediate clear to black to ensure screen is in known state
    glViewport(0, 0, mode.width, mode.height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!gfx.egl.swap_buffers()) {
         std::cerr << "Error: Initial swap buffers after reset failed!" << std::endl;
    } else {
         std::cout << "Initial swap buffers after reset success." << std::endl;
    }
}

void finish_post_game_if_ready(PostGameGate& gate, AppState& state,
                               platform::InputManager& input,
                               platform::GpioManager& gpio) {
    // Return-from-game ready edge: the reset above has run (or none was
    // needed — a launch that failed before the handover), so the kiosk
    // can take input again. Anything queued since the input devices
    // reopened was pressed at a dissolving plate or a black screen, not
    // at the menu that is about to fade in — drain it unseen. Only
    // after that does the status stop saying "retroarch".
    if (gate.take_ready(state.reset_display.load())) {
        (void)input.poll();
        if (gpio.is_available()) (void)gpio.poll();
        state.retroarch_rom_name.clear();
        state.retroarch_core.clear();
        std::cout << "Post-game reset complete; accepting input" << std::endl;
    }
}

}  // namespace app
