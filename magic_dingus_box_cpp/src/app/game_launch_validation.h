#pragma once

#include <functional>
#include <string>

#include "app_state.h"
#include "../utils/result.h"

namespace app {

struct ValidatedGameLaunch {
    std::string core_name;  // "auto" resolved; no _libretro suffix added
    std::string rom_path;   // resolved, known to exist
};

// Everything about an emulated_game item that can fail WITHOUT touching the
// system: core resolution ("auto" → per-system core), ROM path, ROM and core
// existence. Controller runs it BEFORE the game-session begin hook, which
// pauses torrents and stops containers (GameQuietMode) — a launch that
// cannot happen must not churn the service stack.
//
// `core_installed` is injected (RetroArchLauncher::find_core_dir in the
// kiosk) so this stays testable without the launcher's link tail.
utils::Result<ValidatedGameLaunch> validate_game_launch(
    const PlaylistItem& item, const std::string& playlist_directory,
    const std::function<bool(const std::string& core_name)>& core_installed);

}  // namespace app
