#include <iostream>
#include <string>
#include <filesystem>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
#include "../src/retroarch/retroarch_launcher.h"
#include "../src/utils/path_resolver.h"

namespace fs = std::filesystem;

int main() {
    std::cout << "=== Testing Super Mario Bros. 3 Launch ===" << std::endl;

    // Initialize RetroArch launcher
    retroarch::RetroArchLauncher launcher;
    if (!launcher.initialize()) {
        std::cerr << "Failed to initialize RetroArch launcher" << std::endl;
        return 1;
    }

    std::cout << "✓ RetroArch launcher initialized" << std::endl;

    // Test the same parameters as Super Mario Bros. 3
    std::string rom_path = "dev_data/roms/nes/Super Mario Bros. 3.nes";
    std::string playlist_dir = "/data/playlists";
    std::string core_name = "nestopia_libretro";

    // Resolve ROM path (same as controller does)
    std::string resolved_path = utils::resolve_video_path(rom_path, playlist_dir);
    std::cout << "✓ ROM path resolved: " << resolved_path << std::endl;

    // Verify ROM exists
    if (!fs::exists(resolved_path)) {
        std::cerr << "✗ ROM file not found: " << resolved_path << std::endl;
        return 1;
    }
    std::cout << "✓ ROM file exists" << std::endl;

    // Create game launch info
    retroarch::GameLaunchInfo game_info = {
        resolved_path,
        core_name,
        ""  // overlay_path
    };

    std::cout << "Stopping UI service first..." << std::endl;
    system("systemctl stop magic-dingus-box-cpp.service");

    std::cout << "Waiting for service to stop..." << std::endl;
    sleep(3);

    std::cout << "Launching Super Mario Bros. 3..." << std::endl;
    std::cout << "  ROM: " << game_info.rom_path << std::endl;
    std::cout << "  Core: " << game_info.core_name << std::endl;

    // Launch the game (this will test the full launch process)
    bool launched = launcher.launch_game(game_info);

    if (launched) {
        std::cout << "✓ Game launched successfully!" << std::endl;
        std::cout << "✓ RetroArch should now be running Super Mario Bros. 3" << std::endl;
        return 0;
    } else {
        std::cerr << "✗ Game launch failed!" << std::endl;
        return 1;
    }
}
