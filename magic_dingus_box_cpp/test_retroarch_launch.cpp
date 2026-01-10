#include <iostream>
#include <string>
#include <filesystem>
#include "../src/retroarch/retroarch_launcher.h"
#include "../src/utils/path_resolver.h"

namespace fs = std::filesystem;

int main() {
    std::cout << "=== Testing RetroArch Launch ===" << std::endl;

    // Initialize RetroArch launcher
    retroarch::RetroArchLauncher launcher;
    if (!launcher.initialize()) {
        std::cerr << "Failed to initialize RetroArch launcher" << std::endl;
        return 1;
    }

    std::cout << "RetroArch launcher initialized successfully" << std::endl;

    // Test ROM path resolution (simulate what controller does)
    std::string rom_path = "dev_data/roms/nes/Super Mario Bros. 3.nes";
    std::string playlist_dir = "/data/playlists";  // This is the symlink location

    std::string resolved_path = utils::resolve_video_path(rom_path, playlist_dir);
    std::cout << "Original path: " << rom_path << std::endl;
    std::cout << "Resolved path: " << resolved_path << std::endl;

    // Check if ROM exists
    if (!fs::exists(resolved_path)) {
        std::cerr << "ERROR: ROM file does not exist: " << resolved_path << std::endl;
        return 1;
    }

    std::cout << "ROM file exists ✓" << std::endl;

    // Create game launch info for Super Mario Bros. 3
    retroarch::GameLaunchInfo game_info = {
        resolved_path,           // rom_path
        "nestopia_libretro",     // core_name
        ""                       // overlay_path (empty)
    };

    std::cout << "Launching Super Mario Bros. 3..." << std::endl;
    std::cout << "  ROM: " << game_info.rom_path << std::endl;
    std::cout << "  Core: " << game_info.core_name << std::endl;

    // Launch the game
    bool result = launcher.launch_game(game_info);

    if (result) {
        std::cout << "✓ Game launched successfully!" << std::endl;
        return 0;
    } else {
        std::cerr << "✗ Game launch failed!" << std::endl;
        return 1;
    }
}
