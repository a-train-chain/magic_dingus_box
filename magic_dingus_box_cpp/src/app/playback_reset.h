#pragma once

#include <string>

#include "app_state.h"

namespace app {

// The single "playback is over, show the main menu" state reset. Call it
// AFTER stopping the pipeline (controller.stop()); it only touches AppState.
//
// Every path that ends playlist playback must land here. Renderer::render
// treats "playing indexes set + no video" as a between-items transition and
// draws nothing, so a stop that leaves the indexes >= 0 blanks the menu, the
// error banner and the settings menu until reboot. A non-empty
// `error_message` is raised AFTER the reset so the banner shows on the menu
// this reset makes visible; an empty one leaves any current banner alone.
void stop_to_menu(AppState& state, const std::string& error_message = {});

// The main-UI half of the hand-off to and from the Media Browser: used at
// MB entry (after controller.stop()) and by every MB exit. Clears the same
// playing-item indexes / UI fade / switching flag as stop_to_menu — the
// anti-blank-menu part — but deliberately differs from it in what it does
// NOT do:
//   - leaves ui_visible_when_playing FALSE (stop_to_menu: true): the MB
//     owns the screen on entry, and nothing is playing on exit, where the
//     flag has no visible effect until the next playlist start resets it;
//   - leaves position/duration, the auto-advance guard and the phone
//     remote's now-playing fields alone: on exit those belong to the MB
//     PlaybackScreen, which drives the shared pipeline (and whose watch
//     flush must read the position first). Entry clears now-playing itself
//     with clear_now_playing().
void reset_main_ui_for_media_browser(AppState& state);

// Clears the now-playing / playlist fields kiosk_status.json publishes to
// the phone remote. Controller::update_state's stop-clear only fires on a
// video_active true->false edge it observes; paths that force video_active
// false themselves never produce that edge, so they clear here.
void clear_now_playing(AppState& state);

}  // namespace app
