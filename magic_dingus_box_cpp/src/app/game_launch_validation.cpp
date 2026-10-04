#include "game_launch_validation.h"

#include <filesystem>

#include "../retroarch/launch_contract.h"
#include "../utils/path_resolver.h"

namespace fs = std::filesystem;

namespace app {

using Validated = utils::Result<ValidatedGameLaunch>;

utils::Result<ValidatedGameLaunch> validate_game_launch(
    const PlaylistItem& item, const std::string& playlist_directory,
    const std::function<bool(const std::string& core_name)>& core_installed) {
    std::string core_name = item.emulator_core;
    if (core_name.empty()) {
        return Validated::fail("No emulator_core specified for game: " + item.title);
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
        const std::string& system = item.emulator_system;
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
            return Validated::fail("Could not resolve auto core for system: " + system);
        }
    }

    if (item.path.empty()) {
        return Validated::fail("No ROM path specified for game: " + item.title);
    }

    const std::string rom_path = utils::resolve_video_path(item.path, playlist_directory);
    if (!fs::exists(rom_path)) {
        return Validated::fail("ROM file does not exist: " + rom_path);
    }

    // The core must be installed BEFORE anything is torn down. Found
    // late, it cost a stopped video, a released DRM master and input
    // grab, and a dark screen while RetroArch failed to load it.
    if (!core_installed(core_name)) {
        return Validated::fail("Emulator core not installed: " +
                               retroarch::libretro_core_name(core_name));
    }

    return Validated::ok(ValidatedGameLaunch{core_name, rom_path});
}

}  // namespace app
