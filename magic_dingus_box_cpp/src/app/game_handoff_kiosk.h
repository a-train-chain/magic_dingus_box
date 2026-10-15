#pragma once

// The drawing / DRM half of the game hand-off, kiosk-only (GL, EGL, DRM,
// GStreamer): launching a ROM from the Settings game browser behind the
// loading screen, and getting the display back after RetroArch exits.
// Moved out of main.cpp verbatim. The non-drawing half (session
// bracketing, quiet mode, watchdog) is app/game_handoff.h, unit-tested.

#include <string>

#include "app_state.h"
#include "post_game_gate.h"

namespace platform {
struct DisplayMode;
class DrmDisplay;
class EglContext;
class FramePresenter;
class GpioManager;
class InputManager;
}  // namespace platform
namespace video {
class GstPlayer;
class GstRenderer;
}  // namespace video
namespace ui {
class Renderer;
class SettingsMenuManager;
}  // namespace ui

namespace app {

class Controller;

// The display stack the hand-off tears down and restores. All owned by
// main(); borrowed here.
struct KioskGraphics {
    platform::DrmDisplay& display;
    platform::EglContext& egl;
    platform::FramePresenter& frame_presenter;
    video::GstPlayer& player;
    video::GstRenderer& gst_renderer;
    ui::Renderer& ui_renderer;
};

// Settings game browser SELECT on a game row: launch it with the loading
// plate drawn while load_playlist_item works, report the outcome, then
// force-close the Settings menu. Blocks for the whole game session. All
// session bracketing — watchdog, GPIO poll thread, phone-remote status,
// quiet mode, artwork pause, loading state — happens inside
// load_playlist_item via the game-session hooks (app/game_handoff.h).
// `mode` is read live, as the inline code did.
void launch_game_from_browser(Controller& controller, AppState& state,
                              ui::SettingsMenuManager& settings_menu,
                              KioskGraphics& gfx, const platform::DisplayMode& mode,
                              const Playlist& playlist, int game_idx,
                              const std::string& playlist_directory);

// The state.reset_display block (set after returning from RetroArch):
// DRM master, display mode, frame presenter, EGL context, GL resources of
// both renderers, the GStreamer player, audio output, and a clear to black.
// Clears state.reset_display.
void restore_display_after_game(KioskGraphics& gfx, AppState& state,
                                const platform::DisplayMode& mode);

// Return-from-game ready edge (app/post_game_gate.h): once the reset above
// has run (or none was needed), drain the input queued at a black screen
// unseen and clear the RetroArch status fields. Call once per main-loop
// iteration, right after the reset block.
void finish_post_game_if_ready(PostGameGate& gate, AppState& state,
                               platform::InputManager& input,
                               platform::GpioManager& gpio);

}  // namespace app
