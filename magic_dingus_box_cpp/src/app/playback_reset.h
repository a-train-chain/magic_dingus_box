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

}  // namespace app
