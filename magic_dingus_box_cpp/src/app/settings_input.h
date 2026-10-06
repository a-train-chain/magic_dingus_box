#pragma once

// The kiosk's Settings-menu input dispatch, moved out of main.cpp's event
// loop verbatim (same order, same log lines): BTN4's toggle / hold-for-
// volume, then — while they are shown — the on-screen keyboard and the
// Settings menu with every sub-screen it drives (game browser + ROM
// launch, Controller Setup wizard, Connect a Device pairing screen, the
// top-level/submenu rows, and the Media Browser entry rows).
//
// Kiosk-only (Settings menu, keyboard, the game hand-off's GL). The pure
// decisions (the BTN4 hold, the held-volume step, the game browser's
// SELECT rows) are app/settings_input_logic.h, unit-tested on the Mac.
//
// main()'s per-event order, unchanged:
//   if (!post_game_gate.accepts_input()) break;
//   if (handle_menu_button(...)) continue;     // BTN4 + held volume
//   if (dispatch_overlay_input(...)) continue; // keyboard / Settings
//   ... main-menu input

#include <functional>
#include <string>

#include "app_state.h"
#include "settings_input_logic.h"

namespace platform {
struct DisplayMode;
struct InputEvent;
class InputManager;
}  // namespace platform
namespace ui {
class SettingsMenuManager;
class VirtualKeyboard;
}  // namespace ui

namespace app {

class Controller;
struct KioskGraphics;

// What the dispatch borrows from main(). All references must outlive it;
// `mode` and `playlist_directory` are read live (main() reassigns both).
struct SettingsInputContext {
    AppState& state;
    ui::SettingsMenuManager& settings_menu;
    ui::VirtualKeyboard& keyboard;
    Controller& controller;
    platform::InputManager& input;
    KioskGraphics& gfx;
    const platform::DisplayMode& mode;
    const std::string& playlist_directory;
#ifdef MEDIA_BROWSER_ENABLED
    // Settings -> "Movies": AppScreen -> MediaBrowser on the Browse landing
    // screen (MediaBrowserHost::enter_from_settings). Called after the
    // dispatch has stopped playback and closed the Settings menu.
    std::function<void()> enter_media_browser;
#endif
};

// BTN4 (InputAction::SETTINGS_MENU) and, while it is held, the rotary /
// vertical axis as master volume. Returns true when `ev` was consumed —
// every BTN4 event, and every event while BTN4 is held.
bool handle_menu_button(SettingsInputContext& ctx, MenuButtonHold& hold,
                        const platform::InputEvent& ev);

// The keyboard (when active) or the Settings menu (when active, opening or
// closing) gets `ev`. Returns true when one of them was shown — the event
// is then consumed whether or not it did anything; false leaves it to the
// main menu.
bool dispatch_overlay_input(SettingsInputContext& ctx,
                            const platform::InputEvent& ev);

}  // namespace app
