#include "retroarch_launcher.h"
#include "../utils/config.h"
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <filesystem>
#include <thread>
#include <chrono>
#include <fstream>
#include <vector>
#include <errno.h>
#include <ctime>
#include <sstream>
#include <regex>
#include <cmath>

namespace fs = std::filesystem;

namespace retroarch {

namespace {
    struct ControllerMapping {
        // Metadata
        std::string name = "Default";
        
        // Settings
        std::string analog_dpad_mode = "1"; // 0=Digital, 1=Left Analog
        std::string input_driver = "udev";
        
        // Core-specific options (e.g., for config file)
        std::string core_option_pad_type = ""; // e.g. "analog" for PS1
        std::string extra_config = ""; // For any other core-specific settings (audio, etc.)
        
        // Standard Buttons (Map Physical ID -> RetroPad Function)
        std::string b_btn = "1";      // RetroPad B (Bottom Action)
        std::string y_btn = "3";      // RetroPad Y (Left Action)
        std::string select_btn = "10";
        std::string start_btn = "2";
        
        std::string a_btn = "0";      // RetroPad A (Right Action)
        std::string x_btn = "4";      // RetroPad X (Top Action)
        
        std::string l_btn = "5";      // L1
        std::string r_btn = "6";      // R1
        std::string l2_btn = "";      // L2 (Optional)
        std::string r2_btn = "";      // R2 (Optional)
        
        // D-Pad (Usually Hat)
        std::string up_btn = "h0up";
        std::string down_btn = "h0down";
        std::string left_btn = "h0left";
        std::string right_btn = "h0right";
        
        // Analog Sticks
        std::string l_x_plus = "+0";
        std::string l_x_minus = "-0";
        std::string l_y_plus = "+1";
        std::string l_y_minus = "-1";
        
        // D-Pad Axis Mappings (Explicit Analog-to-Dpad)
        std::string up_axis = "";
        std::string down_axis = "";
        std::string left_axis = "";
        std::string right_axis = "";

        // Hotkeys
        std::string enable_hotkey_btn = ""; // The "modifier" button (must be held)
        std::string menu_toggle_btn = "";   // The button to press with modifier
        std::string exit_emulator_btn = ""; // Optional exit button
    };

    // N64 Controller Physical Button IDs (verified via evtest):
    //   0=C-Left, 1=B, 2=A, 3=C-Down, 4=L shoulder, 5=R shoulder,
    //   6=Z trigger, 8=C-Right, 9=C-Up, 10=unused, 12=Start
    //   Axes: 0/1=Analog Stick, Hat0X/Hat0Y=D-pad
    //
    // NOTE: evdev names are misleading on this adapter:
    //   BTN_Z (309) = physical R shoulder (button 5)
    //   BTN_TL (310) = physical Z trigger (button 6)
    //
    // Hotkey: Z trigger (button 6) + Start (button 12) = toggle RetroArch menu
    // This combo is consistent across ALL cores.

    ControllerMapping get_mapping_for_core(const std::string& core_name) {
        ControllerMapping map; // Starts with defaults

        if (core_name.find("nestopia") != std::string::npos || core_name.find("fceumm") != std::string::npos) {
            map.name = "NES (N64 Controller)";
            map.analog_dpad_mode = "0"; // Disable auto-analog, use explicit mapping

            map.b_btn = "1";  // NES B (Run) -> N64 B
            map.a_btn = "2";  // NES A (Jump) -> N64 A

            map.select_btn = "9";  // Select -> C-Up
            map.start_btn = "12";  // Start -> Start

            // Turbo Buttons
            map.x_btn = "3";  // Turbo A -> C-Down
            map.y_btn = "0";  // Turbo B -> C-Left

            // Analog Stick -> D-Pad (so stick works for Mario)
            map.right_axis = "+0";
            map.left_axis = "-0";
            map.down_axis = "+1";
            map.up_axis = "-1";

            // Hotkeys: Z trigger + Start for Menu
            map.enable_hotkey_btn = "6"; // Z trigger (under center grip)
            map.menu_toggle_btn = "12";  // Start

            map.extra_config = "nestopia_audio_vol_sq1 = \"100\"\n"
                               "nestopia_audio_vol_sq2 = \"100\"\n"
                               "nestopia_audio_vol_tri = \"100\"\n"
                               "nestopia_audio_vol_noise = \"100\"\n"
                               "nestopia_audio_vol_dpcm = \"100\"\n";

        } else if (core_name.find("pcsx") != std::string::npos || core_name.find("beetle_psx") != std::string::npos || core_name.find("swanstation") != std::string::npos) {
            map.name = "PS1 (N64 Controller)";
            map.core_option_pad_type = "analog";
            map.analog_dpad_mode = "0";

            // PS1 face buttons on right-hand buttons (A, B, C-cluster):
            map.b_btn = "2";  // Cross (primary action) -> A button
            map.a_btn = "1";  // Circle (secondary) -> B button
            map.y_btn = "3";  // Square (attack/action) -> C-Down
            map.x_btn = "0";  // Triangle (menu/special) -> C-Left

            map.start_btn = "12"; // Start -> Start
            map.select_btn = "9"; // Select -> C-Up

            // Shoulder buttons:
            map.l_btn = "4";  // L1 -> L shoulder
            map.r_btn = "5";  // R1 -> R shoulder
            map.r2_btn = "8"; // R2 -> C-Right

            // Analog Stick
            map.l_x_plus = "+0";
            map.l_x_minus = "-0";
            map.l_y_plus = "+1";
            map.l_y_minus = "-1";

            // Analog Stick -> D-Pad
            map.right_axis = "+0";
            map.left_axis = "-0";
            map.down_axis = "+1";
            map.up_axis = "-1";

            // Hotkeys: Z trigger + Start for Menu
            map.enable_hotkey_btn = "6"; // Z trigger (under center grip)
            map.menu_toggle_btn = "12";  // Start

        } else if (core_name.find("prosystem") != std::string::npos) {
            map.name = "Atari 7800";
            map.analog_dpad_mode = "0"; // Digital only

            map.b_btn = "1"; // Button 1 -> RetroPad B
            map.a_btn = "2"; // Button 2 -> RetroPad A

            map.select_btn = "10";
            map.start_btn = "12";

            map.up_btn = "h0up";
            map.down_btn = "h0down";
            map.left_btn = "h0left";
            map.right_btn = "h0right";

            // Hotkeys: Z trigger + Start for Menu
            map.enable_hotkey_btn = "6";
            map.menu_toggle_btn = "12";

        } else if (core_name.find("genesis_plus_gx") != std::string::npos) {
            map.name = "Sega Genesis";
            map.analog_dpad_mode = "0"; // Digital only

            // Genesis 3-button: A, B, C
            map.a_btn = "2"; // C
            map.b_btn = "1"; // B
            map.y_btn = "3"; // A

            map.start_btn = "12";

            map.up_btn = "h0up";
            map.down_btn = "h0down";
            map.left_btn = "h0left";
            map.right_btn = "h0right";

            // Hotkeys: Z trigger + Start for Menu
            map.enable_hotkey_btn = "6";
            map.menu_toggle_btn = "12";

        } else if (core_name.find("snes9x") != std::string::npos) {
            map.name = "Super Nintendo";
            map.analog_dpad_mode = "0"; // Digital only

            // SNES Layout: B, A, Y, X, L, R
            map.b_btn = "1";
            map.a_btn = "2";
            map.y_btn = "3";
            map.x_btn = "0";

            // Shoulders on physical shoulder buttons
            map.l_btn = "4"; // L -> L shoulder
            map.r_btn = "5"; // R -> R shoulder

            map.start_btn = "12";
            map.select_btn = "10";

            map.up_btn = "h0up";
            map.down_btn = "h0down";
            map.left_btn = "h0left";
            map.right_btn = "h0right";

            // Hotkeys: Z trigger + Start for Menu
            map.enable_hotkey_btn = "6"; // Z trigger (under center grip)
            map.menu_toggle_btn = "12";  // Start

        } else if (core_name.find("mednafen_pce_fast") != std::string::npos) {
            map.name = "PC Engine / TurboGrafx-16";
            map.analog_dpad_mode = "0"; // Digital only

            // PCE: I and II buttons
            map.b_btn = "1";  // II
            map.a_btn = "2";  // I

            map.start_btn = "12"; // Run
            map.select_btn = "10"; // Select

            // Turbo buttons
            map.y_btn = "0"; // Turbo II -> C-Left
            map.x_btn = "3"; // Turbo I -> C-Down

            // Hotkeys: Z trigger + Start for Menu
            map.enable_hotkey_btn = "6";
            map.menu_toggle_btn = "12";

        } else if (core_name.find("fbneo") != std::string::npos) {
            map.name = "Arcade (FinalBurn Neo)";
            map.analog_dpad_mode = "0";

            // Standard 6-button arcade layout
            // 1 2 3    ->  Y  X  L
            // 4 5 6    ->  B  A  R
            map.y_btn = "0"; // 1 -> C-Left
            map.x_btn = "3"; // 2 -> C-Down
            map.l_btn = "4"; // 3 -> L shoulder

            map.b_btn = "1"; // 4 -> B
            map.a_btn = "2"; // 5 -> A
            map.r_btn = "5"; // 6 -> R shoulder

            map.select_btn = "9";  // Coin -> C-Up
            map.start_btn = "12";  // Start

            // Analog Stick -> D-Pad
            map.l_x_plus = "+0";
            map.l_x_minus = "-0";
            map.l_y_plus = "+1";
            map.l_y_minus = "-1";

            map.right_axis = "+0";
            map.left_axis = "-0";
            map.down_axis = "+1";
            map.up_axis = "-1";

            // Hotkeys: Z trigger + Start for Menu
            map.enable_hotkey_btn = "6";
            map.menu_toggle_btn = "12";
        }
        return map;
    }
}

RetroArchLauncher::RetroArchLauncher() : retroarch_available_(false) {
}

bool RetroArchLauncher::initialize() {
    retroarch_bin_ = find_retroarch();
    
    retroarch_available_ = retroarch_bin_.has_value();
    
    if (retroarch_available_) {
        std::cout << "RetroArch found at: " << retroarch_bin_.value() << std::endl;
    } else {
        std::cerr << "RetroArch not found. Install with: sudo apt install retroarch" << std::endl;
    }
    
    return retroarch_available_;
}

std::optional<std::string> RetroArchLauncher::find_retroarch() {
    std::vector<std::string> paths = {
        "/opt/retropie/emulators/retroarch/bin/retroarch",  // RetroPie
        "/usr/bin/retroarch",                              // Linux standard
        "/Applications/RetroArch.app/Contents/MacOS/RetroArch"  // macOS
    };
    
    for (const auto& path : paths) {
        if (fs::exists(path)) {
            return path;
        }
    }
    
    return std::nullopt;
}

bool RetroArchLauncher::launch_game(const GameLaunchInfo& game_info, int system_volume_percent, float volume_offset_db) {
    if (!retroarch_available_) {
        std::cerr << "RetroArch not available" << std::endl;
        return false;
    }
    
    // Validate ROM exists
    if (!fs::exists(game_info.rom_path)) {
        std::cerr << "ROM not found: " << game_info.rom_path << std::endl;
        return false;
    }
    
    release_controllers();
    
    // Always use DRM/KMS launch (matches app architecture)
    std::cout << "Launching RetroArch in DRM/KMS mode" << std::endl;
    return launch_drm(game_info, system_volume_percent, volume_offset_db);
}


bool RetroArchLauncher::launch_drm(const GameLaunchInfo& game_info, int system_volume_percent, float volume_offset_db) {
    std::cout << "=== RetroArch Launcher Called ===" << std::endl;
    std::cout << "ROM: " << game_info.rom_path << std::endl;
    std::cout << "Core: " << game_info.core_name << std::endl;
    std::cout << "Overlay: " << game_info.overlay_path << std::endl;
    std::cout << "Launching RetroArch in DRM/KMS mode" << std::endl;
    
    // Stop GStreamer and cleanup audio resources first
    stop_gstreamer_and_cleanup();

    // Build command
    // RetroArch expects the full core name with _libretro suffix for -L argument
    std::string core_name = game_info.core_name;
    // Ensure _libretro suffix is present for RetroArch -L flag
    if (core_name.find("_libretro") == std::string::npos) {
        core_name += "_libretro";
    }

    // Detect core location
    std::string libretro_dir = "/usr/lib/aarch64-linux-gnu/libretro";
    std::string user_core_dir = config::retroarch::get_cores_dir();
    
    // Check if core exists in system dir
    std::string system_core_path = libretro_dir + "/" + core_name + ".so";
    if (!fs::exists(system_core_path)) {
        // Check user dir
        std::string user_core_path = user_core_dir + "/" + core_name + ".so";
        if (fs::exists(user_core_path)) {
            libretro_dir = user_core_dir;
            std::cout << "Found core in user directory: " << user_core_path << std::endl;
        } else {
            std::cout << "Core not found in system or user directory, defaulting to system: " << system_core_path << std::endl;
        }
    } else {
        std::cout << "Found core in system directory: " << system_core_path << std::endl;
    }

    std::vector<std::string> cmd = {
        retroarch_bin_.value(),
        "--config", "/tmp/retroarch_mdb.cfg",
        "-L", core_name,
        game_info.rom_path,
        "--verbose"
    };

    // DRM cleanup will be handled by the main application shutdown
    // The systemd-run service will wait for cleanup to complete before launching RetroArch

    // Build the RetroArch command (skip the binary path which is already in cmd[0])
    std::string retroarch_cmd = "/usr/bin/retroarch";
    for (size_t i = 1; i < cmd.size(); ++i) {  // Start from index 1 to skip the binary path
        const auto& arg = cmd[i];
        // Escape double quotes in arguments for shell safety
            std::string escaped_arg = arg;
            size_t pos = 0;
        while ((pos = escaped_arg.find("\"", pos)) != std::string::npos) {
            escaped_arg.replace(pos, 1, "\\\"");
            pos += 2;
        }
        retroarch_cmd += " \"" + escaped_arg + "\"";
    }

    // Detect ALSA device
    std::string alsa_device = detect_alsa_device();

    // Create a simple launcher script (persistent for debugging)
    std::string launcher_script = config::retroarch::get_launcher_script();
    {
        std::ofstream script_file(launcher_script);
        if (script_file.is_open()) {
            script_file << "#!/bin/bash\n";
            script_file << "set -e\n";  // Exit on any error
            
            // ISOLATED CONFIG STRATEGY (Matches Manual Test)
            // We write a fresh config to /tmp/retroarch_ui.cfg and pass it via --config
            script_file << "# Use isolated temp config to avoid overwriting user's RetroArch config\n";
            script_file << "UI_CONFIG=\"/tmp/retroarch_mdb.cfg\"\n";
            
            script_file << "# CRITICAL: Create a minimal default config to prevent RetroArch from creating one with autoconfig enabled\n";
            script_file << "mkdir -p \"$HOME/.config/retroarch\"\n";
            script_file << "mkdir -p \"/tmp/empty_autoconfig\"\n";
            
            // Create save directories
            script_file << "# Create save directories for game progress persistence\n";
            script_file << "mkdir -p \"" << config::retroarch::get_saves_dir() << "\"\n";
            script_file << "mkdir -p \"" << config::retroarch::get_states_dir() << "\"\n";
            
            // No backup needed - using isolated temp config in /tmp
            
            script_file << "# CRITICAL: Ensure autoconfig file exists and is accessible (DO NOT disable it!)\n";
            script_file << "# Autoconfig is ENABLED, so we need the autoconfig file to be present\n";
            script_file << "AUTOCONFIG_DIR=\"$HOME/.config/retroarch/autoconfig/udev\"\n";
            script_file << "mkdir -p \"$AUTOCONFIG_DIR\"\n";
            script_file << "AUTOCONFIG_FILE=\"$AUTOCONFIG_DIR/0e6d_111d.cfg\"\n";
            
            // Restore any backup files from previous runs (in case they exist)
            script_file << "for backup in \"$AUTOCONFIG_FILE.backup.\"*; do\n";
            script_file << "    if [ -f \"$backup\" ]; then\n";
            script_file << "        mv \"$backup\" \"$AUTOCONFIG_FILE\" 2>/dev/null || true\n";
            script_file << "        echo 'Launcher: Restored autoconfig file from backup' >> /tmp/retroarch_launcher.log\n";
            script_file << "        break\n";
            script_file << "    fi\n";
            script_file << "done\n";
            script_file << "# Ensure autoconfig file exists (create if missing)\n";
            script_file << "if [ ! -f \"$AUTOCONFIG_FILE\" ]; then\n";
            script_file << "    echo '# RetroArch Autoconfig for SWITCH CO.,LTD. Controller' > \"$AUTOCONFIG_FILE\"\n";
            script_file << "    echo 'input_device = \"SWITCH CO.,LTD. Controller\"' >> \"$AUTOCONFIG_FILE\"\n";
            script_file << "    echo 'input_driver = \"udev\"' >> \"$AUTOCONFIG_FILE\"\n";
            script_file << "    echo 'input_vendor_id = \"3677\"' >> \"$AUTOCONFIG_FILE\"\n";
            script_file << "    echo 'input_product_id = \"4381\"' >> \"$AUTOCONFIG_FILE\"\n";
            script_file << "    echo 'input_a_btn = \"0\"' >> \"$AUTOCONFIG_FILE\"\n";
            script_file << "    echo 'input_b_btn = \"1\"' >> \"$AUTOCONFIG_FILE\"\n";
            script_file << "    echo 'input_x_btn = \"4\"' >> \"$AUTOCONFIG_FILE\"\n";
            script_file << "    echo 'input_y_btn = \"3\"' >> \"$AUTOCONFIG_FILE\"\n";
            script_file << "    echo 'input_l_btn = \"5\"' >> \"$AUTOCONFIG_FILE\"\n";
            script_file << "    echo 'input_r_btn = \"6\"' >> \"$AUTOCONFIG_FILE\"\n";
            script_file << "    echo 'input_start_btn = \"2\"' >> \"$AUTOCONFIG_FILE\"\n";
            script_file << "    echo 'input_select_btn = \"10\"' >> \"$AUTOCONFIG_FILE\"\n";
            script_file << "    echo 'input_up_btn = \"h0up\"' >> \"$AUTOCONFIG_FILE\"\n";
            script_file << "    echo 'input_down_btn = \"h0down\"' >> \"$AUTOCONFIG_FILE\"\n";
            script_file << "    echo 'input_left_btn = \"h0left\"' >> \"$AUTOCONFIG_FILE\"\n";
            script_file << "    echo 'input_right_btn = \"h0right\"' >> \"$AUTOCONFIG_FILE\"\n";
            script_file << "    echo 'Launcher: Created autoconfig file' >> /tmp/retroarch_launcher.log\n";
            script_file << "fi\n";
            script_file << "# CRITICAL: Audio settings will be in the main config file (simpler approach)\n";
            // Get service name for restart
            const char* service_name_env = std::getenv("MAGIC_UI_SERVICE");
            if (!service_name_env) {
                service_name_env = "magic-dingus-box-cpp.service";
            }

            // Service name already obtained above

            script_file << "echo \"$(date): Launcher: Starting RetroArch launcher script\" >> /tmp/retroarch_launcher.log\n";
            script_file << "echo \"$(date): Launcher: Detected ALSA device: " << alsa_device << "\" >> /tmp/retroarch_launcher.log\n";
            script_file << "echo \"$(date): Launcher: GStreamer cleanup completed\" >> /tmp/retroarch_launcher.log\n";
            script_file << "# Running in background of main service - DRM master already dropped\n";
            script_file << "echo 'Launcher: Preparing to launch RetroArch...'\n";
            script_file << "echo 'Launcher: DRM master access already dropped by main service, launching RetroArch...'\n";
            script_file << "echo 'Launcher: Creating RetroArch config...'\n";
            script_file << "echo 'Launcher: ALSA device: " << alsa_device << "'\n";
            script_file << "echo 'Launcher: aplay -l output:' >> /tmp/retroarch_launcher.log\n";
            script_file << "aplay -l >> /tmp/retroarch_launcher.log 2>&1 || true\n";
            
            // Create Core Options file with performance-tuned settings
            script_file << "cat > /tmp/retroarch_core_options.cfg << 'OPTS'\n";
            if (core_name.find("pcsx") != std::string::npos || core_name.find("beetle_psx") != std::string::npos || core_name.find("swanstation") != std::string::npos) {
                script_file << "pcsx_rearmed_pad1type = \"analog\"\n";
                // Performance: offload SPU audio to separate CPU core (Pi 4B has 4 cores)
                script_file << "pcsx_rearmed_spu_thread = \"enabled\"\n";
                // Audio: enable CD audio and XA decoding for full game experience
                script_file << "pcsx_rearmed_nocdaudio = \"disabled\"\n";
                script_file << "pcsx_rearmed_noxadecoding = \"disabled\"\n";
                // Performance: auto-frameskip as safety net (only skips when falling behind)
                script_file << "pcsx_rearmed_frameskip_type = \"auto_threshold\"\n";
                script_file << "pcsx_rearmed_frameskip_threshold = \"33\"\n";
                script_file << "pcsx_rearmed_frameskip_interval = \"3\"\n";
                // Performance: fast GPU linked list processing
                script_file << "pcsx_rearmed_gpu_slow_llists = \"disabled\"\n";
                // Keep DRC (dynamic recompiler) enabled - critical for performance
                script_file << "pcsx_rearmed_drc = \"enabled\"\n";
                // Keep icache emulation for compatibility (disabling breaks some games)
                script_file << "pcsx_rearmed_icache_emulation = \"enabled\"\n";
                // Standard PSX clock (57 = native speed)
                script_file << "pcsx_rearmed_psxclock = \"57\"\n";
                // Audio quality
                script_file << "pcsx_rearmed_spu_interpolation = \"simple\"\n";
                script_file << "pcsx_rearmed_spu_reverb = \"enabled\"\n";
                // Rendering at native 1x resolution (no upscaling overhead)
                script_file << "pcsx_rearmed_neon_enhancement_enable = \"disabled\"\n";
                script_file << "pcsx_rearmed_dithering = \"enabled\"\n";
            }
            script_file << "OPTS\n";

            // Delete per-core .opt override file so our core options take effect
            // RetroArch's per-core .opt files override core_options_path
            script_file << "rm -f \"$HOME/.config/retroarch/config/PCSX-ReARMed/PCSX-ReARMed.opt\" 2>/dev/null\n";
            
            // Write the FULL config to our ISOLATED config location
            script_file << "cat > \"$UI_CONFIG\" << 'EOF'\n";
            script_file << "# DRM/KMS RetroArch config for Magic Dingus Box (Isolated)\n";
            script_file << "libretro_system_directory = \"" << config::retroarch::get_system_dir() << "\"\n";
            
            // Save/State directories for game progress persistence
            script_file << "savefile_directory = \"" << config::retroarch::get_saves_dir() << "\"\n";
            script_file << "savestate_directory = \"" << config::retroarch::get_states_dir() << "\"\n";
            script_file << "sort_savefiles_by_content_enable = \"true\"\n";
            script_file << "sort_savestates_by_content_enable = \"true\"\n";
            script_file << "savestate_auto_save = \"true\"\n";
            script_file << "savestate_auto_load = \"true\"\n";
            
            // Dynamic Video Driver Selection
            // Dynamic Video Driver Selection
            // Default to Vulkan for others
            script_file << "video_driver = \"vulkan\"\n";
            script_file << "video_threaded = \"false\"\n";
            script_file << "audio_driver = \"alsa\"\n"; // Default to alsa for other cores
            script_file << "audio_resampler = \"sinc\"\n"; // High quality for other cores
            
            script_file << "video_fullscreen = \"true\"\n";
            
            // Resolution Logic
            // Resolution Logic
            // Others: 640x480
            script_file << "video_fullscreen_x = \"640\"\n";
            script_file << "video_fullscreen_y = \"480\"\n";
            script_file << "video_windowed_fullscreen = \"false\"\n"; 
            
            script_file << "video_scale_integer = \"false\"\n";
            
            script_file << "# CRITICAL: Ensure RetroArch sets CRTC mode (don't assume it's already set)\n";
            script_file << "video_gpu_screenshot = \"false\"\n";
            script_file << "input_joypad_driver = \"udev\"\n";
            script_file << "input_max_users = \"4\"\n";
            script_file << "# Enhanced controller detection and configuration\n";
            script_file << "# CRITICAL: Enable autodetect so RetroArch detects the controller\n";
            script_file << "# But disable autoconfig so it doesn't load autoconfig files\n";
            script_file << "input_autodetect_enable = \"true\"\n";
            script_file << "# CRITICAL: Disable remap binds since autoconfig is disabled\n";
            script_file << "input_remap_binds_enable = \"true\"\n";  // CRITICAL: Enable so core can receive input
            script_file << "input_player1_analog_dpad_mode = \"0\"\n";  // Digital only for NES (matches working test)
            script_file << "# CRITICAL: Force RetroArch to use built-in default button mappings (auto-assignment)\n";
            script_file << "input_player1_bind_defaults = \"false\"\n";
            script_file << "# CRITICAL: This forces RetroArch to automatically assign standard button mappings\n";
            script_file << "# RetroArch will map: A=0, B=1, X=2, Y=3, L=4, R=5, Start=6, Select=7, D-pad=hat0\n";
            script_file << "# CRITICAL: Ensure player 1 controller is enabled and working\n";
            script_file << "input_player1_joypad_index = \"0\"\n";
            script_file << "input_player1_enable = \"true\"\n";
            script_file << "# Default mappings removed to prevent conflict with core-specific overrides\n";
            script_file << "# We rely on core-specific sections to define mappings\n";
            script_file << "# For NES: A=0 (jump), B=1 (run), Start=2, Select=10, D-pad=hat0\n";
            script_file << "input_enable_hotkey = \"true\"\n";
            script_file << "input_menu_toggle_gamepad_combo = \"1\"\n";  // L1+R1+Start+Select
            script_file << "input_auto_game_focus = \"true\"\n";
            script_file << "input_game_focus_enable = \"true\"\n";
            script_file << "input_logging_enable = \"false\"\n";
            script_file << "input_logging_level = \"0\"\n";
            script_file << "input_block_timeout = \"0\"\n";
            script_file << "input_hotkey_block_delay = \"0\"\n";
            script_file << "# CRITICAL: Ensure input is enabled and controller works in-game\n";
            script_file << "input_enabled = \"true\"\n";
            script_file << "input_driver = \"udev\"\n";
            script_file << "input_poll_type_behavior = \"0\"\n";
            script_file << "input_all_users_control_menu = \"true\"\n";
            script_file << "# CRITICAL: Ensure controller input reaches the core\n";
            script_file << "input_descriptor_label_show = \"true\"\n";  // Show descriptors (matches working test)
            script_file << "input_descriptor_hide_unbound = \"false\"\n";
            script_file << "# CRITICAL: Enable autoconfig to load button mappings from autoconfig file\n";
            script_file << "input_autoconfig_enable = \"false\"\n";
            script_file << "input_joypad_driver_autoconfig_dir = \"/tmp/empty_autoconfig\"\n"; // Hide autoconfig files
            script_file << "# CRITICAL: Ensure joypad driver is set (required for controller detection)\n";
            script_file << "input_joypad_driver = \"udev\"\n";
            script_file << "# CRITICAL: Force RetroArch to auto-assign default button mappings if autoconfig fails\n";
            script_file << "# When bind_defaults=true, RetroArch will automatically assign standard button mappings\n";
            script_file << "# This ensures buttons work even if autoconfig doesn't match perfectly\n";
            script_file << "input_joypad_driver_mapping_dir = \"\"\n";
            script_file << "# CRITICAL: Disable remap binds since autoconfig is disabled\n";
            script_file << "input_remap_binds_enable = \"true\"\n";  // CRITICAL: Enable so core can receive input
            script_file << "# Don't save config on exit (prevents overwriting our settings)\n";
            script_file << "config_save_on_exit = \"false\"\n";
            script_file << "# CRITICAL: Single press to quit (don't require double press)\n";
            script_file << "quit_press_twice = \"false\"\n";
            script_file << "core_options_path = \"/tmp/retroarch_core_options.cfg\"\n";
            script_file << "# Audio settings - use ALSA to match GStreamer (simplified for reliability)\n";
//             script_file << "audio_driver = \"alsa\"\n";
            script_file << "audio_device = \"" << alsa_device << "\"\n";
            script_file << "audio_enable = \"true\"\n";
            script_file << "audio_mute_enable = \"false\"\n";
            // Convert system volume (0-100) to RetroArch dB format
            // RetroArch uses decibels: 0 dB = 100%, negative dB = quieter
            // Formula: dB = 20 * log10(volume_percent / 100)
            // For safety, clamp to reasonable range: -60 dB to 0 dB
            float volume_decimal = system_volume_percent / 100.0f;
            float volume_db = (volume_decimal > 0.001f) ? (20.0f * log10f(volume_decimal)) : -60.0f;
            // Clamp to valid range
            if (volume_db > 0.0f) volume_db = 0.0f;
            if (volume_db < -60.0f) volume_db = -60.0f;
            // Apply user's game volume offset (e.g., -3dB, -6dB, -12dB)
            float final_volume_db = volume_db + volume_offset_db;
            if (final_volume_db < -60.0f) final_volume_db = -60.0f;
            script_file << "audio_volume = \"" << final_volume_db << "\"\n";
            script_file << "audio_mixer_volume = \"1.0\"\n";
            script_file << "audio_mixer_mute_enable = \"false\"\n";
            script_file << "# Simplified audio settings (matches Pi game version)\n";
            script_file << "audio_sync = \"true\"\n";
//             script_file << "audio_resampler = \"sinc\"\n";
            script_file << "audio_out_rate = \"48000\"\n";
            script_file << "audio_latency = \"48\"\n";  // Tighter audio sync (was 64)
            script_file << "# Audio buffer settings - ensure audio callback works\n";
//             script_file << "audio_block_frames = \"512\"\n";
//             script_file << "audio_rate_control = \"true\"\n";
//             script_file << "audio_rate_control_delta = \"0.005000\"\n";
            script_file << "audio_enable_menu = \"false\"\n";
            script_file << "audio_fastforward_mute = \"false\"\n";
            script_file << "audio_dsp_plugin = \"\"\n";
            script_file << "input_keyboard_layout = \"us\"\n";
            script_file << "libretro_directory = \"" << libretro_dir << "\"\n";
            script_file << "menu_show_online_updater = \"true\"\n";
            script_file << "core_updater_buildbot_cores_url = \"https://buildbot.libretro.com/nightly/linux/aarch64/latest\"\n";
            script_file << "core_updater_buildbot_assets_url = \"https://buildbot.libretro.com/assets/\"\n";
            script_file << "core_updater_auto_extract_archive = \"true\"\n";
            script_file << "# KMS driver handles context internally\n";
            script_file << "# video_context_driver not needed when using video_driver = \"kms\"\n";
            script_file << "video_allow_rotate = \"false\"\n";
            script_file << "video_crop_overscan = \"false\"\n";
            script_file << "# Force RetroArch to set display mode explicitly\n";
            script_file << "video_refresh_rate = \"60.000000\"\n";
            script_file << "# CRT native resolution: Force Mode Switch to 640x480\n";
//             script_file << "video_fullscreen_x = \"640\"\n";
//             script_file << "video_fullscreen_y = \"480\"\n";
            script_file << "video_windowed_width = \"640\"\n";
            script_file << "video_windowed_height = \"480\"\n";
            script_file << "video_windowed_fullscreen = \"false\"\n"; // False = True Exclusive Mode Switch
            script_file << "video_fullscreen = \"true\"\n";
            script_file << "# CRITICAL: Render games at native resolution (e.g., 256x224 for NES), scale to 640x480\n";
            script_file << "video_custom_viewport_enable = \"false\"\n";
            script_file << "# Let cores render at their native resolution - RetroArch will scale automatically\n";
            script_file << "video_aspect_ratio = \"1.333\"\n";
            script_file << "video_force_aspect = \"true\"\n";
            script_file << "aspect_ratio_index = \"23\"\n";
            script_file << "# Allow non-integer scaling to fill 640x480 while maintaining 4:3 aspect ratio\n";
            script_file << "video_scale_integer = \"false\"\n";
            script_file << "video_scale = \"1.0\"\n";
            script_file << "video_scale_filter = \"0\"\n";
            script_file << "video_smooth = \"false\"\n";
            script_file << "# Ensure games use their native internal resolution\n";
            script_file << "video_crop_overscan = \"false\"\n";
            script_file << "video_rotation = \"0\"\n";
            script_file << "# Critical video rendering settings for KMS\n";
            // script_file << "video_threaded = \"false\"\n"; // Handled dynamically above
            script_file << "video_hard_sync = \"false\"\n";
            script_file << "video_vsync = \"true\"\n";
            script_file << "video_frame_delay = \"4\"\n";  // Reduce input lag by ~4ms
            script_file << "video_max_swapchain_images = \"3\"\n";  // Triple buffering for smoother frame pacing
            script_file << "video_shader_enable = \"false\"\n";
            script_file << "video_filter = \"\"\n";
            script_file << "video_frame_blend = \"false\"\n";
            script_file << "video_gpu_record = \"false\"\n";
            script_file << "video_record = \"false\"\n";
            script_file << "# Ensure core actually runs\n";
            script_file << "rewind_enable = \"false\"\n";
            script_file << "run_ahead_enabled = \"false\"\n";
            script_file << "netplay_enable = \"false\"\n";
            script_file << "# CRITICAL: Ensure content actually loads and runs\n";
            script_file << "content_load_auto_remap = \"false\"\n";
            script_file << "content_load_mode_manual = \"false\"\n";
            script_file << "pause_nonactive = \"false\"\n";
            script_file << "video_disable_composition = \"false\"\n";
            
            // Trojan Horse moved to after EOF
            script_file << "echo 'Launcher: Core name is " << core_name << "' >> /tmp/retroarch_launcher.log\n";
            
            // 1. Get the mapping configuration
            ControllerMapping map = get_mapping_for_core(core_name);
            
            script_file << "# === Controller Mapping: " << map.name << " ===\n";
            script_file << "echo 'Launcher: Applying controller mapping for: " << map.name << "' >> /tmp/retroarch_launcher.log\n";
            
            // 2. Apply Settings
            script_file << "input_player1_analog_dpad_mode = \"" << map.analog_dpad_mode << "\"\n";
            
            // 3. Apply Buttons
            script_file << "input_player1_b_btn = \"" << map.b_btn << "\"\n";
            script_file << "input_player1_y_btn = \"" << map.y_btn << "\"\n";
            script_file << "input_player1_select_btn = \"" << map.select_btn << "\"\n";
            script_file << "input_player1_start_btn = \"" << map.start_btn << "\"\n";
            
            script_file << "input_player1_up_btn = \"" << map.up_btn << "\"\n";
            script_file << "input_player1_down_btn = \"" << map.down_btn << "\"\n";
            script_file << "input_player1_left_btn = \"" << map.left_btn << "\"\n";
            script_file << "input_player1_right_btn = \"" << map.right_btn << "\"\n";
            
            script_file << "input_player1_a_btn = \"" << map.a_btn << "\"\n";
            script_file << "input_player1_x_btn = \"" << map.x_btn << "\"\n";
            
            script_file << "input_player1_l_btn = \"" << map.l_btn << "\"\n";
            script_file << "input_player1_r_btn = \"" << map.r_btn << "\"\n";
            
            script_file << "input_player1_l2_btn = \"" << map.l2_btn << "\"\n";
            script_file << "input_player1_r2_btn = \"" << map.r2_btn << "\"\n";

            // 4. Apply Analog Axes
            script_file << "input_player1_l_x_plus_axis = \"" << map.l_x_plus << "\"\n";
            script_file << "input_player1_l_x_minus_axis = \"" << map.l_x_minus << "\"\n";
            script_file << "input_player1_l_y_plus_axis = \"" << map.l_y_plus << "\"\n";
            script_file << "input_player1_l_y_minus_axis = \"" << map.l_y_minus << "\"\n";
            
            // 5. Apply D-Pad Axis Mappings
            script_file << "input_player1_up_axis = \"" << map.up_axis << "\"\n";
            script_file << "input_player1_down_axis = \"" << map.down_axis << "\"\n";
            script_file << "input_player1_left_axis = \"" << map.left_axis << "\"\n";
            script_file << "input_player1_right_axis = \"" << map.right_axis << "\"\n";

            // 6. Apply Core Options (if any)
            if (!map.core_option_pad_type.empty()) {
                 script_file << "pcsx_rearmed_pad1type = \"" << map.core_option_pad_type << "\"\n";
            }
            
            // 7. Apply Hotkeys
            if (!map.enable_hotkey_btn.empty()) {
                script_file << "input_enable_hotkey_btn = \"" << map.enable_hotkey_btn << "\"\n";
                
                if (!map.menu_toggle_btn.empty()) {
                    script_file << "input_menu_toggle_btn = \"" << map.menu_toggle_btn << "\"\n";
                }
                
                if (!map.exit_emulator_btn.empty()) {
                    script_file << "input_exit_emulator_btn = \"" << map.exit_emulator_btn << "\"\n";
                }
            }

            // 8. Apply Extra Config (if any)
            if (!map.extra_config.empty()) {
                script_file << map.extra_config;
            }
            script_file << "# CRITICAL: Ensure input reaches the core (not just RetroArch menu)\n";
            script_file << "input_driver_block_input = \"false\"\n";  // Don't block input
            script_file << "input_driver_block_libretro_input = \"false\"\n";  // Don't block libretro input
            script_file << "# Controller auto-configuration enabled - configure when game launches\n";
            script_file << "\n";
            script_file << "EOF\n";



            // NUCLEAR OPTION 2.0: Delete autoconfig file to FORCE manual mapping from retroarch.cfg
            // The Trojan Horse method (overwriting autoconfig) failed to produce correct results.
            // By deleting the file and disabling autoconfig, we force RetroArch to use the explicit
            // mappings defined in the main config file.
            script_file << "echo 'Launcher: Deleting autoconfig file to force manual mapping' >> /tmp/retroarch_launcher.log\n";
            script_file << "rm -f \"$AUTOCONFIG_FILE\"\n";
            // Also ensure no other autoconfigs are found
            script_file << "mkdir -p /tmp/empty_autoconfig\n";

                script_file << "echo 'Launcher: Starting RetroArch...'\n";
            script_file << "echo 'Launcher: User: $(whoami)' >> /tmp/retroarch_launcher.log\n";
            script_file << "echo 'Launcher: Groups: $(groups)' >> /tmp/retroarch_launcher.log\n";

            script_file << "# CRITICAL: Verify controller devices are accessible before launching RetroArch\n";
            script_file << "echo 'Launcher: Verifying controller device accessibility...' >> /tmp/retroarch_launcher.log\n";
            script_file << "CONTROLLER_ACCESSIBLE=false\n";
            script_file << "for js_device in /dev/input/js*; do\n";
            script_file << "    if [ -c \"$js_device\" ] && [ -r \"$js_device\" ]; then\n";
            script_file << "        echo \"Launcher: Controller device accessible: $js_device\" >> /tmp/retroarch_launcher.log\n";
            script_file << "        CONTROLLER_ACCESSIBLE=true\n";
            script_file << "        # Get device permissions for debugging\n";
            script_file << "        ls -la \"$js_device\" >> /tmp/retroarch_launcher.log 2>&1 || true\n";
            script_file << "        break\n";
            script_file << "    fi\n";
            script_file << "done\n";
            script_file << "if [ \"$CONTROLLER_ACCESSIBLE\" = \"false\" ]; then\n";
            script_file << "    echo 'Launcher: WARNING - No accessible controller devices found!' >> /tmp/retroarch_launcher.log\n";
            script_file << "    echo 'Launcher: Checking user groups...' >> /tmp/retroarch_launcher.log\n";
            script_file << "    groups >> /tmp/retroarch_launcher.log 2>&1 || true\n";
            script_file << "    echo 'Launcher: Checking device permissions...' >> /tmp/retroarch_launcher.log\n";
            script_file << "    ls -la /dev/input/js* >> /tmp/retroarch_launcher.log 2>&1 || true\n";
            script_file << "fi\n";
            script_file << "# Set essential environment for RetroArch\n";
            script_file << "export XDG_RUNTIME_DIR=/run/user/" << getuid() << "\n";
            script_file << "export HOME=" << config::get_home_path() << "\n";
            script_file << "# CRITICAL: Ensure we have access to input devices\n";
            script_file << "export DISPLAY=:0\n";
            script_file << "# CRITICAL: Check who is holding the input device\n";
            script_file << "echo 'Launcher: Checking input device usage...' >> /tmp/retroarch_launcher.log\n";
            script_file << "fuser -v /dev/input/event0 >> /tmp/retroarch_launcher.log 2>&1 || true\n";
            
            script_file << "# CRITICAL: Wake up controller and ensure it's ready before RetroArch starts\n";
            script_file << "# Controller may be in sleep mode after GStreamer/DRM cleanup\n";
            script_file << "# The manual test works because the USER presses buttons, waking the controller\n";
            script_file << "# We need to simulate this by actually reading from the controller\n";
            script_file << "echo 'Launcher: Waking up controller...' >> /tmp/retroarch_launcher.log\n";
            script_file << "# Trigger udev events to ensure controller is active\n";
            script_file << "sudo udevadm trigger --action=change --sysname-match=js* 2>/dev/null || true\n";
            script_file << "sudo udevadm trigger --action=change --sysname-match=event* 2>/dev/null || true\n";
            script_file << "udevadm settle --timeout=2 2>/dev/null || true\n";
            script_file << "# CRITICAL: Actually read from controller to wake it (like user pressing buttons)\n";
            script_file << "# This simulates the manual test where user interaction wakes the controller\n";
            script_file << "echo 'Launcher: Reading from controller to wake it (simulating user interaction)...' >> /tmp/retroarch_launcher.log\n";
            script_file << "# Open device and read a few events (this wakes it up)\n";
            script_file << "( timeout 0.5 hexdump -C /dev/input/event0 2>/dev/null | head -5 >/dev/null 2>&1 & )\n";
            script_file << "WAKE_PID=$!\n";
            script_file << "sleep 0.6\n";
            script_file << "kill $WAKE_PID 2>/dev/null || true\n";
            script_file << "# Also try js device\n";
            script_file << "( timeout 0.5 hexdump -C /dev/input/js0 2>/dev/null | head -5 >/dev/null 2>&1 & )\n";
            script_file << "WAKE_PID2=$!\n";
            script_file << "sleep 0.6\n";
            script_file << "kill $WAKE_PID2 2>/dev/null || true\n";
            script_file << "# Small delay to ensure controller is fully ready\n";
            script_file << "sleep 0.3\n";
            script_file << "echo 'Launcher: Controller wake-up complete' >> /tmp/retroarch_launcher.log\n";
            script_file << "# NOTE: We rely on the main app releasing its grab (InputManager::cleanup)\n";
            script_file << "# and the wake-up sequence above to ensure controller works.\n";
            script_file << "# Background keepalive processes are removed as they may steal events from RetroArch.\n";
            script_file << "sleep 0.2\n";
            script_file << "# CRITICAL: Autoconfig file should already exist (we ensured it above)\n";
            script_file << "# Verify it exists before launching RetroArch\n";
            script_file << "if [ ! -f \"$AUTOCONFIG_FILE\" ]; then\n";
            script_file << "    echo 'Launcher: WARNING - Autoconfig file missing!' >> /tmp/retroarch_launcher.log\n";
            script_file << "fi\n";
            script_file << "# CRITICAL: Ensure udev has processed controller events before RetroArch starts\n";
            script_file << "sudo udevadm trigger --action=change --sysname-match=js* 2>/dev/null || true\n";
            script_file << "sudo udevadm trigger --action=change --sysname-match=event* 2>/dev/null || true\n";
            script_file << "udevadm settle --timeout=1 2>/dev/null || true\n";
            script_file << "# CRITICAL: Redirect stdout/stderr to log file\n";
            script_file << "exec 1>>" << config::retroarch::get_launcher_log() << " 2>&1\n";
            script_file << "echo 'Launcher: Launching RetroArch directly...' >> /tmp/retroarch_launcher.log\n";
            script_file << "# CRITICAL: Run RetroArch in foreground (not exec) so cleanup can run\n";
            script_file << "# Background keepalive processes keep controller awake while RetroArch runs\n";
            script_file << "# RetroArch will open input devices itself (they're kept awake by background processes)\n";
            script_file << retroarch_cmd << "\n";
            script_file << "RETROARCH_EXIT=$?\n";
            script_file << "echo \"Launcher: RetroArch exited with code $RETROARCH_EXIT\" >> /tmp/retroarch_launcher.log\n";
            script_file << "# Stop keepalive processes when RetroArch exits\n";
            script_file << "kill $KEEPALIVE_PID1 2>/dev/null || true\n";
            script_file << "kill $KEEPALIVE_PID2 2>/dev/null || true\n";
            script_file << "wait $KEEPALIVE_PID1 2>/dev/null || true\n";
            script_file << "wait $KEEPALIVE_PID2 2>/dev/null || true\n";
            // Clean up temp config files (no restore needed - we used isolated /tmp config)
            script_file << "rm -f \"$UI_CONFIG\"\n";
            script_file << "rm -f /tmp/retroarch_core_options.cfg\n";
            script_file << "# CRITICAL: Autoconfig file should remain in place (not backed up/restored)\n";
            script_file << "# Clean up any old backup files from previous runs\n";
            script_file << "find \"$AUTOCONFIG_DIR\" -name '*.backup.*' -type f -mtime +1 -delete 2>/dev/null || true\n";
                script_file << "echo 'Launcher: RetroArch finished'\n";
                script_file << "# Script will exit, main service continues running\n";

            script_file.close();

            // Make script executable
            chmod(launcher_script.c_str(), 0755);
            std::cout << "Created launcher script: " << launcher_script << std::endl;
        } else {
            std::cerr << "Failed to create launcher script" << std::endl;
            return false;
            }
        }

        // Set environment variables for the RetroArch process
        std::string xdg_runtime = "/run/user/" + std::to_string(getuid());
        setenv("XDG_RUNTIME_DIR", xdg_runtime.c_str(), 1);
        setenv("HOME", config::get_home_path().c_str(), 1);
        setenv("DISPLAY", ":0", 1);
        
        // CRITICAL: Verify controller device is accessible before forking
        std::cout << "Verifying controller device accessibility..." << std::endl;
        bool controller_accessible = false;
        for (int i = 0; i < 4; ++i) {
            std::string js_path = "/dev/input/js" + std::to_string(i);
            if (access(js_path.c_str(), R_OK) == 0) {
                std::cout << "Controller device accessible: " << js_path << std::endl;
                controller_accessible = true;
                break;
            }
        }
        if (!controller_accessible) {
            std::cerr << "WARNING: No accessible controller devices found before RetroArch launch!" << std::endl;
            std::cerr << "This may cause controller input to not work in RetroArch" << std::endl;
        }

        // CRITICAL: Launch RetroArch DIRECTLY (no systemd-run)
        // The service already has correct permissions and session access.
        // Using systemd-run --user was isolating the process from the input devices.
        // Since we manually drop DRM master and release input devices, direct launch is safe.
        std::cout << "Launching RetroArch DIRECTLY (inheriting service environment)..." << std::endl;
        
        // Execute the launcher script directly
        std::string launch_cmd = "/bin/bash " + launcher_script;
        
        std::cout << "Command: " << launch_cmd << std::endl;
        
        // CRITICAL: Fork to run command in background (non-blocking for UI)
        pid_t launch_pid = fork();
        if (launch_pid == 0) {
            // Child process - execute the launch command
            // Redirect output to log file
            int log_fd = open(config::retroarch::get_launcher_log().c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (log_fd != -1) {
                dup2(log_fd, STDOUT_FILENO);
                dup2(log_fd, STDERR_FILENO);
                close(log_fd);
            }
            
            // Reset signal handlers
            signal(SIGCHLD, SIG_DFL);
            signal(SIGPIPE, SIG_DFL);
            
            // Close file descriptors 3 and up to prevent inheritance
            // This is crucial to ensure RetroArch doesn't inherit input FDs
            for (int fd = 3; fd < 256; fd++) {
                close(fd);
            }

            execl("/bin/bash", "bash", launcher_script.c_str(), nullptr);
            // If we reach here, exec failed
            std::cerr << "Failed to execute launch command" << std::endl;
            exit(1);
        } else if (launch_pid > 0) {
            // Parent process - WAIT for RetroArch to exit (Blocking)
            std::cout << "RetroArch launch initiated (Blocking, PID: " << launch_pid << ")" << std::endl;
            
            int status;
            waitpid(launch_pid, &status, 0);
            
            if (WIFEXITED(status)) {
                std::cout << "RetroArch exited with status " << WEXITSTATUS(status) << std::endl;
            } else if (WIFSIGNALED(status)) {
                std::cout << "RetroArch killed by signal " << WTERMSIG(status) << std::endl;
            }
        } else {
            std::cerr << "Failed to fork launch process" << std::endl;
            return false;
        }

        // Return true indicating game has finished
        return true;
}

bool RetroArchLauncher::open_core_downloader(int system_volume_percent) {
    if (!retroarch_available_) {
        std::cerr << "RetroArch not available" << std::endl;
        return false;
    }
    
    release_controllers();
    
    // Always use direct launch with DRM/KMS
    return open_core_downloader_direct(system_volume_percent);
}

bool RetroArchLauncher::open_core_downloader_direct(int system_volume_percent) {
    std::cout << "Opening RetroArch Core Downloader in DRM/KMS mode" << std::endl;
    
    // Stop GStreamer and cleanup audio resources first
    stop_gstreamer_and_cleanup();
    
    // Detect ALSA device
    std::string alsa_device = detect_alsa_device();

    // Build command for core downloader
    std::vector<std::string> cmd = {
        retroarch_bin_.value(),
        "--menu",
        "--verbose",
        "--config", "/tmp/retroarch_launcher.cfg"  // Will be created by launcher script
    };

    // Build the RetroArch command (skip the binary path which is already in cmd[0])
    std::string retroarch_cmd = "/usr/bin/retroarch";
    for (size_t i = 1; i < cmd.size(); ++i) {  // Start from index 1 to skip the binary path
        const auto& arg = cmd[i];
        if (arg == "--config") {
            // Replace the config flag with the launcher config and skip the next argument
            retroarch_cmd += " --config \"/tmp/retroarch_launcher.cfg\"";
            ++i; // Skip the original config file path argument
        } else {
            // Escape quotes in arguments for shell safety
            std::string escaped_arg = arg;
            size_t pos = 0;
            while ((pos = escaped_arg.find("'", pos)) != std::string::npos) {
                escaped_arg.replace(pos, 1, "'\"'\"'");
                pos += 5;
            }
            retroarch_cmd += " '" + escaped_arg + "'";
        }
    }

    // Create a simple launcher script (more reliable than inline bash)
    std::string launcher_script = "/tmp/retroarch_downloader.sh";
    {
        std::ofstream script_file(launcher_script);
        if (script_file.is_open()) {
            script_file << "#!/bin/bash\n";
            script_file << "set -e\n";  // Exit on any error
            script_file << "echo \"$(date): Downloader: Starting RetroArch downloader script\" >> /tmp/retroarch_launcher.log\n";
            script_file << "echo \"$(date): Downloader: Detected ALSA device: " << alsa_device << "\" >> /tmp/retroarch_launcher.log\n";
            script_file << "echo \"$(date): Downloader: GStreamer cleanup completed\" >> /tmp/retroarch_launcher.log\n";
            script_file << "echo 'Downloader: Waiting for main app cleanup...'\n";
            script_file << "sleep 3\n";  // Wait for main app to fully exit and clean up DRM resources
            script_file << "echo 'Downloader: Creating RetroArch config...'\n";
            script_file << "echo 'Downloader: ALSA device: " << alsa_device << "'\n";
            script_file << "echo 'Downloader: aplay -l output:' >> /tmp/retroarch_launcher.log\n";
            script_file << "aplay -l >> /tmp/retroarch_launcher.log 2>&1 || true\n";
            script_file << "cat > /tmp/retroarch_launcher.cfg << 'EOF'\n";
            script_file << "# DRM/KMS RetroArch config for Magic Dingus Box\n";
            script_file << "# CRITICAL: Use Vulkan driver (works best with KMS/DRM)\n";
            script_file << "video_driver = \"vulkan\"\n";
            script_file << "video_fullscreen = \"true\"\n";
            script_file << "# Auto-detect resolution (match UI)\n";
            script_file << "video_windowed_fullscreen = \"true\"\n";
            script_file << "# CRITICAL: Ensure RetroArch sets CRTC mode (don't assume it's already set)\n";
            script_file << "video_gpu_screenshot = \"false\"\n";
            script_file << "input_joypad_driver = \"udev\"\n";
            script_file << "input_max_users = \"4\"\n";
            script_file << "# Enhanced controller detection and configuration\n";
            script_file << "# CRITICAL: Enable autodetect so RetroArch detects the controller\n";
            script_file << "# But disable autoconfig so it doesn't load autoconfig files\n";
            script_file << "input_autodetect_enable = \"true\"\n";
            script_file << "# CRITICAL: Disable remap binds since autoconfig is disabled\n";
            script_file << "input_remap_binds_enable = \"true\"\n";  // CRITICAL: Enable so core can receive input
            script_file << "input_player1_analog_dpad_mode = \"0\"\n";  // Digital only for NES (matches working test)
            script_file << "# CRITICAL: Force RetroArch to use built-in default button mappings (auto-assignment)\n";
            script_file << "input_player1_bind_defaults = \"false\"\n";
            script_file << "# CRITICAL: This forces RetroArch to automatically assign standard button mappings\n";
            script_file << "# RetroArch will map: A=0, B=1, X=2, Y=3, L=4, R=5, Start=6, Select=7, D-pad=hat0\n";
            script_file << "# CRITICAL: Ensure player 1 controller is enabled and working\n";
            script_file << "input_player1_joypad_index = \"0\"\n";
            script_file << "input_player1_enable = \"true\"\n";
            script_file << "# Default mappings removed to prevent conflict with core-specific overrides\n";
            script_file << "# We rely on core-specific sections to define mappings\n";
            script_file << "# For NES: A=0 (jump), B=1 (run), Start=2, Select=10, D-pad=hat0\n";
            script_file << "input_enable_hotkey = \"true\"\n";
            script_file << "input_menu_toggle_gamepad_combo = \"1\"\n";  // L1+R1+Start+Select
            script_file << "input_auto_game_focus = \"true\"\n";
            script_file << "input_game_focus_enable = \"true\"\n";
            script_file << "input_logging_enable = \"false\"\n";
            script_file << "input_logging_level = \"0\"\n";
            script_file << "input_block_timeout = \"0\"\n";
            script_file << "input_hotkey_block_delay = \"0\"\n";
            script_file << "# CRITICAL: Ensure input is enabled and controller works in-game\n";
            script_file << "input_enabled = \"true\"\n";
            script_file << "input_driver = \"udev\"\n";
            script_file << "input_poll_type_behavior = \"0\"\n";
            script_file << "input_all_users_control_menu = \"true\"\n";
            script_file << "# CRITICAL: Ensure controller input reaches the core\n";
            script_file << "input_descriptor_label_show = \"true\"\n";  // Show descriptors (matches working test)
            script_file << "input_descriptor_hide_unbound = \"false\"\n";
            script_file << "# CRITICAL: Enable autoconfig to load button mappings from autoconfig file\n";
            script_file << "input_autoconfig_enable = \"false\"\n";
            script_file << "input_joypad_driver_autoconfig_dir = \"/tmp/empty_autoconfig\"\n"; // Hide autoconfig files
            script_file << "# CRITICAL: Ensure joypad driver is set (required for controller detection)\n";
            script_file << "input_joypad_driver = \"udev\"\n";
            script_file << "# CRITICAL: Force RetroArch to auto-assign default button mappings if autoconfig fails\n";
            script_file << "# When bind_defaults=true, RetroArch will automatically assign standard button mappings\n";
            script_file << "# This ensures buttons work even if autoconfig doesn't match perfectly\n";
            script_file << "input_joypad_driver_mapping_dir = \"\"\n";
            script_file << "# CRITICAL: Disable remap binds since autoconfig is disabled\n";
            script_file << "input_remap_binds_enable = \"true\"\n";  // CRITICAL: Enable so core can receive input
            script_file << "# Don't save config on exit (prevents overwriting our settings)\n";
            script_file << "config_save_on_exit = \"false\"\n";
            script_file << "core_options_path = \"/tmp/retroarch_core_options.cfg\"\n";
            script_file << "# Audio settings - use ALSA to match GStreamer (simplified for reliability)\n";
            script_file << "audio_driver = \"alsa\"\n";
            script_file << "audio_device = \"" << alsa_device << "\"\n";
            script_file << "audio_enable = \"true\"\n";
            script_file << "audio_mute_enable = \"false\"\n";
            // Convert system volume (0-100) to RetroArch dB format
            // RetroArch uses decibels: 0 dB = 100%, negative dB = quieter
            // Formula: dB = 20 * log10(volume_percent / 100)
            // For safety, clamp to reasonable range: -60 dB to 0 dB
            float volume_decimal = system_volume_percent / 100.0f;
            float volume_db = (volume_decimal > 0.001f) ? (20.0f * log10f(volume_decimal)) : -60.0f;
            // Clamp to valid range
            if (volume_db > 0.0f) volume_db = 0.0f;
            if (volume_db < -60.0f) volume_db = -60.0f;
            script_file << "audio_volume = \"" << volume_db << "\"\n";
            script_file << "audio_mixer_volume = \"1.0\"\n";
            script_file << "audio_mixer_mute_enable = \"false\"\n";
            script_file << "# Simplified audio settings (matches Pi game version)\n";
            script_file << "audio_sync = \"true\"\n";
            script_file << "audio_resampler = \"sinc\"\n";
            script_file << "audio_out_rate = \"48000\"\n";
            script_file << "audio_latency = \"48\"\n";  // Tighter audio sync (was 64)
            script_file << "# Audio buffer settings - ensure audio callback works\n";
            script_file << "audio_block_frames = \"512\"\n";
            script_file << "audio_rate_control = \"true\"\n";
            script_file << "audio_rate_control_delta = \"0.005000\"\n";
            script_file << "audio_enable_menu = \"false\"\n";
            script_file << "audio_fastforward_mute = \"false\"\n";
            script_file << "audio_dsp_plugin = \"\"\n";
            script_file << "input_keyboard_layout = \"us\"\n";
            script_file << "libretro_directory = \"/usr/lib/aarch64-linux-gnu/libretro\"\n";
            script_file << "menu_show_online_updater = \"true\"\n";
            script_file << "core_updater_buildbot_cores_url = \"https://buildbot.libretro.com/nightly/linux/aarch64/latest\"\n";
            script_file << "core_updater_buildbot_assets_url = \"https://buildbot.libretro.com/assets/\"\n";
            script_file << "core_updater_auto_extract_archive = \"true\"\n";
            script_file << "# KMS driver handles context internally\n";
            script_file << "# video_context_driver not needed when using video_driver = \"kms\"\n";
            script_file << "video_allow_rotate = \"false\"\n";
            script_file << "video_crop_overscan = \"false\"\n";
            script_file << "# Force RetroArch to set display mode explicitly\n";
            script_file << "video_refresh_rate = \"60.000000\"\n";
            script_file << "# CRT native resolution: Force Mode Switch to 640x480\n";
            script_file << "video_fullscreen_x = \"640\"\n";
            script_file << "video_fullscreen_y = \"480\"\n";
            script_file << "video_windowed_width = \"640\"\n";
            script_file << "video_windowed_height = \"480\"\n";
            script_file << "video_windowed_fullscreen = \"false\"\n"; // False = True Exclusive Mode Switch
            script_file << "video_fullscreen = \"true\"\n";
            script_file << "# CRITICAL: Render games at native resolution (e.g., 256x224 for NES), scale to 640x480\n";
            script_file << "video_custom_viewport_enable = \"false\"\n";
            script_file << "# Let cores render at their native resolution - RetroArch will scale automatically\n";
            script_file << "video_aspect_ratio = \"1.333\"\n";
            script_file << "video_force_aspect = \"true\"\n";
            script_file << "aspect_ratio_index = \"23\"\n";
            script_file << "# Allow non-integer scaling to fill 640x480 while maintaining 4:3 aspect ratio\n";
            script_file << "video_scale_integer = \"false\"\n";
            script_file << "video_scale = \"1.0\"\n";
            script_file << "video_scale_filter = \"0\"\n";
            script_file << "video_smooth = \"false\"\n";
            script_file << "# Ensure games use their native internal resolution\n";
            script_file << "video_crop_overscan = \"false\"\n";
            script_file << "video_rotation = \"0\"\n";
            script_file << "# Critical video rendering settings for KMS\n";
            // script_file << "video_threaded = \"false\"\n"; // Handled dynamically above
            script_file << "video_hard_sync = \"false\"\n";
            script_file << "video_vsync = \"true\"\n";
            script_file << "video_frame_delay = \"4\"\n";  // Reduce input lag by ~4ms
            script_file << "video_max_swapchain_images = \"3\"\n";  // Triple buffering for smoother frame pacing
            script_file << "video_shader_enable = \"false\"\n";
            script_file << "video_filter = \"\"\n";
            script_file << "video_frame_blend = \"false\"\n";
            script_file << "video_gpu_record = \"false\"\n";
            script_file << "video_record = \"false\"\n";
            script_file << "# Ensure core actually runs\n";
            script_file << "rewind_enable = \"false\"\n";
            script_file << "run_ahead_enabled = \"false\"\n";
            script_file << "netplay_enable = \"false\"\n";
            script_file << "# CRITICAL: Ensure content actually loads and runs\n";
            script_file << "content_load_auto_remap = \"false\"\n";
            script_file << "content_load_mode_manual = \"false\"\n";
            script_file << "pause_nonactive = \"false\"\n";
            script_file << "video_disable_composition = \"false\"\n";
            script_file << "# NES core-specific audio settings for full sound\n";
            script_file << "nestopia_audio_vol_sq1 = \"100\"\n";
            script_file << "nestopia_audio_vol_sq2 = \"100\"\n";
            script_file << "nestopia_audio_vol_tri = \"100\"\n";
            script_file << "nestopia_audio_vol_noise = \"100\"\n";
            script_file << "nestopia_audio_vol_dpcm = \"100\"\n";
            script_file << "# CRITICAL: NES core-specific input settings\n";
            script_file << "# NES uses digital input only (no analog sticks)\n";
            script_file << "input_player1_analog_dpad_mode = \"1\"\n";  // 0 = digital only (NES doesn't have analog)
            script_file << "# CRITICAL: Ensure input reaches the core (not just RetroArch menu)\n";
            script_file << "input_driver_block_input = \"false\"\n";  // Don't block input
            script_file << "input_driver_block_libretro_input = \"false\"\n";  // Don't block libretro input
            script_file << "# Controller auto-configuration enabled - configure when game launches\n";
            script_file << "\n";
            script_file << "EOF\n";
            script_file << "echo 'Downloader: Starting RetroArch...'\n";
            script_file << "# CRITICAL: Ensure we have input group access before launching RetroArch\n";
            script_file << "# CRITICAL: Disable exit-on-error so we restart the UI even if RetroArch crashes\n";
            script_file << "set +e\n";
            script_file << "sg input -c \"" << retroarch_cmd << "\" || " << retroarch_cmd << "\n";
            script_file << "DOWNLOADER_EXIT=$?\n";
            script_file << "set -e\n";
            script_file << "echo \"Downloader: RetroArch exited with code $DOWNLOADER_EXIT\"\n";
            script_file << "rm -f /tmp/retroarch_launcher.cfg\n";
            script_file << "sleep 0.5\n";  // Small delay to ensure RetroArch releases resources

            // Get service name
            const char* service_name_env = std::getenv("MAGIC_UI_SERVICE");
            if (!service_name_env) {
                service_name_env = "magic-dingus-box-cpp.service";
            }

            script_file << "echo 'Downloader: Restarting UI service...'\n";
            script_file << "sudo systemctl start " << service_name_env << "\n";
            script_file << "echo 'Downloader: Service restart complete'\n";

            script_file.close();

            // Make script executable
            chmod(launcher_script.c_str(), 0755);
            std::cout << "Created downloader script: " << launcher_script << std::endl;
        } else {
            std::cerr << "Failed to create downloader script" << std::endl;
            return false;
        }
    }

    // Launch the script using systemd-run
    std::string unique_unit = "retroarch-downloader-" + std::to_string(getpid()) + "-" + std::to_string(time(nullptr));
    std::string systemd_run_cmd = "systemd-run --unit=" + unique_unit + " --service-type=oneshot --remain-after-exit /bin/bash " + launcher_script;

    std::cout << "Launching RetroArch Core Downloader via systemd-run: " << systemd_run_cmd << std::endl;

    int result = std::system(systemd_run_cmd.c_str());
    if (result != 0) {
        std::cerr << "Failed to launch systemd-run command" << std::endl;
        return false;
    }

    std::cout << "RetroArch downloader service started via systemd-run" << std::endl;

    // Return to caller so it can release DRM resources for RetroArch
    std::cout << "RetroArch downloader service started, returning to caller..." << std::endl;
    return true;
}

void RetroArchLauncher::release_controllers() {
    std::cout << "Releasing controller devices before RetroArch launch" << std::endl;
    
    // Iterate through joystick devices
    for (int i = 0; i < 4; ++i) {
        std::string js_path = "/dev/input/js" + std::to_string(i);
        
        // Check if device exists and is readable
        if (access(js_path.c_str(), R_OK) == 0) {
            std::cout << "Releasing controller device: " << js_path << std::endl;
            
            // Trigger udev to reset the device
            std::string udev_cmd = "udevadm trigger --action=change --sysname-match=js" + std::to_string(i);
            int result = std::system(udev_cmd.c_str());
            if (result != 0) {
                std::cerr << "Warning: Failed to trigger udev for " << js_path << std::endl;
            }
        }
    }
    
    // Small delay for devices to settle
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

std::string RetroArchLauncher::detect_alsa_device() {
    std::cout << "Detecting ALSA device (matching Pi game version priority)..." << std::endl;
    
    // PRIORITY 1: Try sysdefault:CARD=vc4hdmi0 (highest priority, matches Pi game version)
    FILE* pipe_l = popen("aplay -L 2>&1", "r");
    if (pipe_l) {
        char buffer[256];
        std::string output_l;
        while (fgets(buffer, sizeof(buffer), pipe_l) != nullptr) {
            output_l += buffer;
        }
        pclose(pipe_l);
        
        // Check for sysdefault:CARD=vc4hdmi0
        std::regex sysdefault_vc4hdmi0_regex(R"(^sysdefault:CARD=vc4hdmi0)");
        if (std::regex_search(output_l, sysdefault_vc4hdmi0_regex)) {
            std::cout << "Found sysdefault:CARD=vc4hdmi0 (PRIORITY 1)" << std::endl;
            return "sysdefault:CARD=vc4hdmi0";
        }
        
        // Check for sysdefault:CARD=vc4hdmi1
        std::regex sysdefault_vc4hdmi1_regex(R"(^sysdefault:CARD=vc4hdmi1)");
        if (std::regex_search(output_l, sysdefault_vc4hdmi1_regex)) {
            std::cout << "Found sysdefault:CARD=vc4hdmi1 (PRIORITY 1)" << std::endl;
            return "sysdefault:CARD=vc4hdmi1";
        }
    }
    
    // PRIORITY 2: Try plughw format (fallback, matches Pi game version)
    FILE* pipe = popen("aplay -l 2>&1", "r");
    if (!pipe) {
        std::cerr << "Warning: Failed to execute aplay -l, using default device" << std::endl;
        return "plughw:1,0";
    }
    
    std::string output;
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    pclose(pipe);
    
    // Log the output for debugging
    std::cout << "aplay -l output:" << std::endl << output << std::endl;
    
    // Look for vc4hdmi0 on card 1 - use plughw: format (PRIORITY 2, matches Pi game version)
    std::regex vc4hdmi0_regex(R"(card\s+1.*vc4hdmi0)");
    if (std::regex_search(output, vc4hdmi0_regex)) {
        std::cout << "Found vc4hdmi0 on card 1, using plughw:1,0 (PRIORITY 2)" << std::endl;
        return "plughw:1,0";
    }
    
    // Look for vc4hdmi1 on card 2 - use plughw: format (PRIORITY 2)
    std::regex vc4hdmi1_regex(R"(card\s+2.*vc4hdmi1)");
    if (std::regex_search(output, vc4hdmi1_regex)) {
        std::cout << "Found vc4hdmi1 on card 2, using plughw:2,0 (PRIORITY 2)" << std::endl;
        return "plughw:2,0";
    }
    
    // Default fallback - use plughw: format (matches Pi game version)
    std::cout << "No specific HDMI device found, using default plughw:1,0" << std::endl;
    return "plughw:1,0";
}

void RetroArchLauncher::stop_gstreamer_and_cleanup() {
    std::cout << "Stopping GStreamer and cleaning up audio resources..." << std::endl;
    
    // Kill GStreamer processes that are children of our app (avoid killing unrelated processes)
    std::cout << "Killing GStreamer child processes..." << std::endl;
    std::string our_pid = std::to_string(getpid());
    auto run_pkill = [](const char* const args[]) {
        pid_t pid = fork();
        if (pid == 0) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
            execvp(args[0], const_cast<char* const*>(args));
            _exit(127);
        }
        if (pid > 0) { int s; waitpid(pid, &s, 0); }
    };
    const char* kill_gst1[] = {"pkill", "-9", "-P", our_pid.c_str(), "-f", "gst", nullptr};
    const char* kill_gst2[] = {"pkill", "-9", "gst-launch-1.0", nullptr};
    run_pkill(kill_gst1);
    run_pkill(kill_gst2);

    // Wait for processes to exit
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Check if ALSA device is still in use
    FILE* lsof_pipe = popen("lsof 2>/dev/null | grep snd || true", "r");
    if (lsof_pipe) {
        char buffer[256];
        bool device_busy = false;
        while (fgets(buffer, sizeof(buffer), lsof_pipe) != nullptr) {
            std::string line(buffer);
            if (line.find("snd") != std::string::npos) {
                device_busy = true;
                std::cout << "ALSA device still in use: " << line;
            }
        }
        pclose(lsof_pipe);

        if (device_busy) {
            std::cout << "ALSA device still busy, waiting additional 300ms..." << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }
    
    std::cout << "GStreamer cleanup complete" << std::endl;
}

} // namespace retroarch
