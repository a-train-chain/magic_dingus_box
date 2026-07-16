#include "retroarch_launcher.h"
#include "controller_detector.h"
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
    // Escape a string for safe embedding inside a SINGLE-QUOTED shell context.
    //
    // Use this for any C++ value that ends up between single quotes in a
    // shell command emitted by the launcher script — for example:
    //
    //   script_file << "echo 'Core: " << shell_sq_escape(core_name)
    //               << "' >> /tmp/log\n";
    //
    // The result becomes literal '<value>' in the shell, with embedded single
    // quotes correctly escaped via the canonical close-quote/escape/reopen
    // pattern: ' → '\''. Backslashes, $, backticks, and double quotes survive
    // unchanged because the surrounding single quotes prevent shell expansion.
    //
    // This is NOT needed for values written inside `cat > ... << 'EOF' ... EOF`
    // heredoc bodies — the single-quoted delimiter makes those literal already.
    // It IS needed anywhere a value is emitted in the launcher script outside
    // a single-quoted heredoc: shell echo statements, `mkdir -p "$path"`,
    // anywhere bash actually evaluates the line.
    //
    // INVARIANT: any C++ value embedded in shell context that could contain
    // operator-controlled input (ROM titles, paths, custom names) MUST go
    // through this helper. Values that are programmatic identifiers
    // (core_name like "pcsx_rearmed_libretro", static map names) are safe in
    // practice but routed through this helper anyway as defense-in-depth.
    std::string shell_sq_escape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            if (c == '\'') {
                out += "'\\''";
            } else {
                out += c;
            }
        }
        return out;
    }

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

    ControllerMapping get_mapping_n64_adapter(const std::string& core_name) {
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
            map.analog_dpad_mode = "0"; // Disable auto-analog, use explicit mapping

            map.b_btn = "1"; // Button 1 -> RetroPad B
            map.a_btn = "2"; // Button 2 -> RetroPad A

            map.select_btn = "10";
            map.start_btn = "12";

            map.up_btn = "h0up";
            map.down_btn = "h0down";
            map.left_btn = "h0left";
            map.right_btn = "h0right";

            // Analog Stick -> D-Pad (so stick works for movement)
            map.right_axis = "+0";
            map.left_axis = "-0";
            map.down_axis = "+1";
            map.up_axis = "-1";

            // Hotkeys: Z trigger + Start for Menu
            map.enable_hotkey_btn = "6";
            map.menu_toggle_btn = "12";

        } else if (core_name.find("genesis_plus_gx") != std::string::npos) {
            map.name = "Sega Genesis";
            map.analog_dpad_mode = "0"; // Disable auto-analog, use explicit mapping

            // Genesis 3-button: A, B, C
            map.a_btn = "2"; // C
            map.b_btn = "1"; // B
            map.y_btn = "3"; // A

            map.start_btn = "12";

            map.up_btn = "h0up";
            map.down_btn = "h0down";
            map.left_btn = "h0left";
            map.right_btn = "h0right";

            // Analog Stick -> D-Pad (so stick works for movement)
            map.right_axis = "+0";
            map.left_axis = "-0";
            map.down_axis = "+1";
            map.up_axis = "-1";

            // Hotkeys: Z trigger + Start for Menu
            map.enable_hotkey_btn = "6";
            map.menu_toggle_btn = "12";

        } else if (core_name.find("snes9x") != std::string::npos) {
            map.name = "Super Nintendo";
            map.analog_dpad_mode = "0"; // Disable auto-analog, use explicit mapping

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

            // Analog Stick -> D-Pad (so stick works for movement)
            map.right_axis = "+0";
            map.left_axis = "-0";
            map.down_axis = "+1";
            map.up_axis = "-1";

            // Hotkeys: Z trigger + Start for Menu
            map.enable_hotkey_btn = "6"; // Z trigger (under center grip)
            map.menu_toggle_btn = "12";  // Start

        } else if (core_name.find("mednafen_pce_fast") != std::string::npos) {
            map.name = "PC Engine / TurboGrafx-16";
            map.analog_dpad_mode = "0"; // Disable auto-analog, use explicit mapping

            // PCE: I and II buttons
            map.b_btn = "1";  // II
            map.a_btn = "2";  // I

            map.start_btn = "12"; // Run
            map.select_btn = "10"; // Select

            // Turbo buttons
            map.y_btn = "0"; // Turbo II -> C-Left
            map.x_btn = "3"; // Turbo I -> C-Down

            // Analog Stick -> D-Pad (so stick works for movement)
            map.right_axis = "+0";
            map.left_axis = "-0";
            map.down_axis = "+1";
            map.up_axis = "-1";

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

    // PS-style USB pad (DragonRise/Microntek 0079:0006):
    //   Face buttons: 0=Triangle, 1=Circle, 2=Cross, 3=Square
    //   Shoulders:    4=L1, 5=R1, 6=L2, 7=R2
    //   Center:       8=Select, 9=Start
    //   D-pad:        hat0
    //   Left stick:   axes 0 (X) / 1 (Y)
    //   Right stick:  axes 2 (Rx) / 5 (Ry)
    //
    // Hotkey combo across all cores: Select (8) + Start (9) = RetroArch menu toggle.
    ControllerMapping get_mapping_ps_style(const std::string& core_name) {
        ControllerMapping map;
        map.analog_dpad_mode = "0";

        // Universal PS-pad hotkey
        map.enable_hotkey_btn = "8"; // Select
        map.menu_toggle_btn   = "9"; // Start

        // Left stick defaults (most cores use it for D-pad emulation)
        map.l_x_plus  = "+0";
        map.l_x_minus = "-0";
        map.l_y_plus  = "+1";
        map.l_y_minus = "-1";

        map.right_axis = "+0";
        map.left_axis  = "-0";
        map.down_axis  = "+1";
        map.up_axis    = "-1";

        if (core_name.find("nestopia") != std::string::npos || core_name.find("fceumm") != std::string::npos) {
            map.name = "NES (PS-style)";
            map.b_btn       = "2"; // Cross -> RetroPad B (NES B, run)
            map.a_btn       = "1"; // Circle -> RetroPad A (NES A, jump)
            map.y_btn       = "3"; // Square -> RetroPad Y (turbo B)
            map.x_btn       = "0"; // Triangle -> RetroPad X (turbo A)
            map.select_btn  = "8";
            map.start_btn   = "9";
            map.extra_config = "nestopia_audio_vol_sq1 = \"100\"\n"
                               "nestopia_audio_vol_sq2 = \"100\"\n"
                               "nestopia_audio_vol_tri = \"100\"\n"
                               "nestopia_audio_vol_noise = \"100\"\n"
                               "nestopia_audio_vol_dpcm = \"100\"\n";

        } else if (core_name.find("snes9x") != std::string::npos) {
            map.name = "Super Nintendo (PS-style)";
            map.b_btn      = "2"; // Cross -> B (bottom)
            map.a_btn      = "1"; // Circle -> A (right)
            map.y_btn      = "3"; // Square -> Y (left)
            map.x_btn      = "0"; // Triangle -> X (top)
            map.l_btn      = "4"; // L1
            map.r_btn      = "5"; // R1
            map.select_btn = "8";
            map.start_btn  = "9";

        } else if (core_name.find("genesis_plus_gx") != std::string::npos) {
            map.name = "Sega Genesis (PS-style)";
            // 3-button: A B C on face; 6-button adds X Y Z on top row
            map.y_btn      = "3"; // Square -> RetroPad Y -> Genesis A (left)
            map.b_btn      = "2"; // Cross -> RetroPad B -> Genesis B (middle)
            map.a_btn      = "1"; // Circle -> RetroPad A -> Genesis C (right)
            map.x_btn      = "0"; // Triangle -> RetroPad X -> Genesis X (6-btn top)
            map.l_btn      = "4"; // L1 -> RetroPad L -> Genesis Y
            map.r_btn      = "5"; // R1 -> RetroPad R -> Genesis Z
            map.start_btn  = "9";

        } else if (core_name.find("pcsx") != std::string::npos ||
                   core_name.find("beetle_psx") != std::string::npos ||
                   core_name.find("swanstation") != std::string::npos) {
            map.name = "PS1 (PS-style, 1:1)";
            map.core_option_pad_type = "analog";
            map.b_btn      = "2"; // Cross -> RetroPad B (== PS1 Cross)
            map.a_btn      = "1"; // Circle -> RetroPad A (== PS1 Circle)
            map.y_btn      = "3"; // Square -> RetroPad Y (== PS1 Square)
            map.x_btn      = "0"; // Triangle -> RetroPad X (== PS1 Triangle)
            map.l_btn      = "4"; // L1
            map.r_btn      = "5"; // R1
            map.l2_btn     = "6"; // L2
            map.r2_btn     = "7"; // R2
            map.select_btn = "8";
            map.start_btn  = "9";

        } else if (core_name.find("prosystem") != std::string::npos) {
            map.name = "Atari 7800 (PS-style)";
            map.b_btn      = "2"; // Cross -> Button 1
            map.a_btn      = "1"; // Circle -> Button 2
            map.select_btn = "8";
            map.start_btn  = "9";

        } else if (core_name.find("mednafen_pce_fast") != std::string::npos) {
            map.name = "PC Engine (PS-style)";
            map.b_btn      = "2"; // Cross -> II (secondary)
            map.a_btn      = "1"; // Circle -> I  (primary, right on real PCE pad)
            map.y_btn      = "3"; // Square -> Turbo II
            map.x_btn      = "0"; // Triangle -> Turbo I
            map.select_btn = "8";
            map.start_btn  = "9";

        } else if (core_name.find("fbneo") != std::string::npos) {
            map.name = "Arcade / FBNeo (PS-style)";
            // Classic Capcom 6-button fighter: top row = punches, bottom row = kicks.
            //     Square Triangle R1     (1 = LP, 2 = MP, 3 = HP)
            //     Cross  Circle   R2     (4 = LK, 5 = MK, 6 = HK)  <- note: R1 is HP, R2 position unused here
            // RetroPad assignments (RetroArch's internal "arcade button N" indices):
            //   Y=1, X=2, L=3, B=4, A=5, R=6
            map.y_btn      = "3"; // Square -> RetroPad Y -> arcade 1 (LP)
            map.x_btn      = "0"; // Triangle -> RetroPad X -> arcade 2 (MP)
            map.l_btn      = "4"; // L1 -> RetroPad L -> arcade 3 (HP)
            map.b_btn      = "2"; // Cross -> RetroPad B -> arcade 4 (LK)
            map.a_btn      = "1"; // Circle -> RetroPad A -> arcade 5 (MK)
            map.r_btn      = "5"; // R1 -> RetroPad R -> arcade 6 (HK)
            map.select_btn = "8";
            map.start_btn  = "9";
        }
        // else: leave map with defaults — shouldn't happen because every
        // shipped core matches one of the branches above.

        return map;
    }

    // Dispatch to the right per-core mapping based on the detected controller.
    // Unknown controllers fall back to the N64 adapter mapping, preserving
    // existing behavior for anyone without the PS pad plugged in.
    ControllerMapping get_mapping(ControllerType type, const std::string& core_name) {
        switch (type) {
            case ControllerType::PS_STYLE_DRAGONRISE:
                return get_mapping_ps_style(core_name);
            case ControllerType::N64_ADAPTER:
            case ControllerType::UNKNOWN:
            default:
                return get_mapping_n64_adapter(core_name);
        }
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

bool RetroArchLauncher::launch_game(const GameLaunchInfo& game_info, int system_volume_percent, float volume_offset_db, int audio_output, const LaunchOptions& opts) {
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
    return launch_drm(game_info, system_volume_percent, volume_offset_db, audio_output, opts);
}


bool RetroArchLauncher::launch_drm(const GameLaunchInfo& game_info, int system_volume_percent, float volume_offset_db, int audio_output, const LaunchOptions& opts) {
    const ReadyWatchOptions ready_options;

    std::cout << "=== RetroArch Launcher Called ===" << std::endl;
    std::cout << "ROM: " << game_info.rom_path << std::endl;
    std::cout << "Core: " << game_info.core_name << std::endl;
    std::cout << "Overlay: " << game_info.overlay_path << std::endl;
    std::cout << "Display mode: " << (opts.display_mode == app::DisplayMode::MODERN_TV ? "MODERN_TV" : "CRT_NATIVE") << std::endl;
    std::cout << "Bezel file: " << (opts.bezel_file.empty() ? "(none)" : opts.bezel_file) << std::endl;
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
    // Use single-quote wrapping for shell safety - handles $, `, ", spaces, etc.
    // Only need to escape literal single quotes: ' -> '\''
    std::string retroarch_cmd = "/usr/bin/retroarch";
    for (size_t i = 1; i < cmd.size(); ++i) {
        const auto& arg = cmd[i];
        std::string escaped_arg = arg;
        size_t pos = 0;
        while ((pos = escaped_arg.find("'", pos)) != std::string::npos) {
            escaped_arg.replace(pos, 1, "'\\''");
            pos += 4;
        }
        retroarch_cmd += " '" + escaped_arg + "'";
    }

    // Select ALSA device based on user's audio output preference
    // audio_output: 0=AUTO, 1=HDMI, 2=HEADPHONE
    std::string alsa_device;
    if (audio_output == 2) {
        // User selected headphone output
        alsa_device = "sysdefault:CARD=Headphones";
        std::cout << "Using headphone ALSA device: " << alsa_device << std::endl;
    } else {
        // AUTO or HDMI: detect HDMI device (existing behavior)
        alsa_device = detect_alsa_device();
    }

    // Create a simple launcher script (persistent for debugging)
    std::string launcher_script = config::retroarch::get_launcher_script();
    {
        std::ofstream script_file(launcher_script);
        if (script_file.is_open()) {
            script_file << "#!/bin/bash\n";
            // NOTE: deliberately NO `set -e`. This script is ~90 lines of
            // best-effort setup (controller wake-ups, udev triggers,
            // permission probes, autoconfig housekeeping) BEFORE the
            // RetroArch invocation. Under `set -e`, any one of those
            // non-critical commands returning non-zero — a udevadm trigger
            // that momentarily fails, a js* glob that doesn't expand
            // because the controller hasn't re-enumerated yet after the
            // DRM/input handoff, an empty-variable reference — silently
            // aborts the whole script and the game never launches. That
            // was the intermittent "launch didn't reach RetroArch" bug:
            // the log ended mid-preamble with no RetroArch line. We WANT
            // the launch to proceed even if a cosmetic setup step hiccups;
            // RetroArch's own exit code is captured explicitly after it
            // runs (RETROARCH_EXIT=$?), which is the outcome that matters.

            // ISOLATED CONFIG STRATEGY (Matches Manual Test)
            // We write a fresh config to /tmp/retroarch_ui.cfg and pass it via --config
            script_file << "# Use isolated temp config to avoid overwriting user's RetroArch config\n";
            script_file << "UI_CONFIG=\"/tmp/retroarch_mdb.cfg\"\n";
            
            script_file << "# CRITICAL: Create a minimal default config to prevent RetroArch from creating one with autoconfig enabled\n";
            script_file << "mkdir -p \"$HOME/.config/retroarch\"\n";
            script_file << "mkdir -p \"/tmp/empty_autoconfig\"\n";
            
            // Create save directories. Paths come from config::retroarch::*()
            // which are programmatic but may contain spaces (e.g., MAGIC_DATA_DIR
            // pointing under "magic_dingus_box /" with the trailing-space quirk).
            // Single-quote-wrap with shell_sq_escape so the path is literal.
            script_file << "# Create save directories for game progress persistence\n";
            script_file << "mkdir -p '" << shell_sq_escape(config::retroarch::get_saves_dir()) << "'\n";
            script_file << "mkdir -p '" << shell_sq_escape(config::retroarch::get_states_dir()) << "'\n";
            
            // No backup needed - using isolated temp config in /tmp

            // 1. Detect the connected controller for both autoconfig emission and mapping dispatch
            ControllerType controller_type = detect_primary_controller();
            std::cout << "Controller detected: " << controller_type_name(controller_type) << std::endl;

            script_file << "# CRITICAL: Ensure autoconfig file exists and is accessible (DO NOT disable it!)\n";
            script_file << "# Autoconfig is ENABLED, so we need the autoconfig file to be present\n";
            script_file << "AUTOCONFIG_DIR=\"$HOME/.config/retroarch/autoconfig/udev\"\n";
            script_file << "mkdir -p \"$AUTOCONFIG_DIR\"\n";
            // Defensive default: AUTOCONFIG_FILE is only assigned a real
            // path inside the N64_ADAPTER controller branch below, but it
            // is referenced UNCONDITIONALLY later (rm -f "$AUTOCONFIG_FILE"
            // and [ ! -f "$AUTOCONFIG_FILE" ]). On the PS-style / unknown
            // controller paths those references would otherwise expand to
            // an empty string ("rm -f ''", "[ ! -f '' ]") — harmless with
            // `set -e` removed, but the empty-path test spuriously logged
            // "Autoconfig file missing!" and, under the old `set -e`, was a
            // coin-flip abort. Seed it to a harmless sentinel so every
            // reference is well-defined regardless of controller type; the
            // N64 branch overrides it with the real path when applicable.
            script_file << "AUTOCONFIG_FILE=\"$AUTOCONFIG_DIR/.mdb_unused_autoconfig\"\n";

            // Autoconfig file emission is controller-specific. RetroArch's autoconfig
            // is disabled at runtime (see input_autoconfig_enable below), so this is
            // mainly for backup/restore hygiene and future-proofing.
            if (controller_type == ControllerType::N64_ADAPTER) {
                script_file << "AUTOCONFIG_FILE=\"$AUTOCONFIG_DIR/0e6d_111d.cfg\"\n";
                // Restore any backup files from previous runs
                script_file << "for backup in \"$AUTOCONFIG_FILE.backup.\"*; do\n";
                script_file << "    if [ -f \"$backup\" ]; then\n";
                script_file << "        mv \"$backup\" \"$AUTOCONFIG_FILE\" 2>/dev/null || true\n";
                script_file << "        echo 'Launcher: Restored autoconfig file from backup' >> /tmp/retroarch_launcher.log\n";
                script_file << "        break\n";
                script_file << "    fi\n";
                script_file << "done\n";
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
                script_file << "    echo 'Launcher: Created N64 autoconfig file' >> /tmp/retroarch_launcher.log\n";
                script_file << "fi\n";
            } else if (controller_type == ControllerType::PS_STYLE_DRAGONRISE) {
                script_file << "AUTOCONFIG_FILE=\"$AUTOCONFIG_DIR/0079_0006.cfg\"\n";
                script_file << "if [ ! -f \"$AUTOCONFIG_FILE\" ]; then\n";
                script_file << "    echo '# RetroArch Autoconfig for DragonRise/Microntek Generic USB Joystick' > \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'input_device = \"USB Joystick\"' >> \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'input_driver = \"udev\"' >> \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'input_vendor_id = \"121\"' >> \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'input_product_id = \"6\"' >> \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'input_a_btn = \"1\"' >> \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'input_b_btn = \"2\"' >> \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'input_x_btn = \"0\"' >> \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'input_y_btn = \"3\"' >> \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'input_l_btn = \"4\"' >> \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'input_r_btn = \"5\"' >> \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'input_l2_btn = \"6\"' >> \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'input_r2_btn = \"7\"' >> \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'input_select_btn = \"8\"' >> \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'input_start_btn = \"9\"' >> \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'input_up_btn = \"h0up\"' >> \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'input_down_btn = \"h0down\"' >> \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'input_left_btn = \"h0left\"' >> \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'input_right_btn = \"h0right\"' >> \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'input_l_x_plus_axis = \"+0\"' >> \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'input_l_x_minus_axis = \"-0\"' >> \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'input_l_y_plus_axis = \"+1\"' >> \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'input_l_y_minus_axis = \"-1\"' >> \"$AUTOCONFIG_FILE\"\n";
                script_file << "    echo 'Launcher: Created PS-style autoconfig file' >> /tmp/retroarch_launcher.log\n";
                script_file << "fi\n";
            }
            // UNKNOWN: skip autoconfig write entirely; RetroArch's autoconfig is
            // disabled anyway and the explicit input_player1_*_btn lines drive input.
            script_file << "# CRITICAL: Audio settings will be in the main config file (simpler approach)\n";
            // Get service name for restart
            const char* service_name_env = std::getenv("MAGIC_UI_SERVICE");
            if (!service_name_env) {
                service_name_env = "magic-dingus-box-cpp.service";
            }

            // Service name already obtained above

            script_file << "echo \"$(date): Launcher: Starting RetroArch launcher script\" >> /tmp/retroarch_launcher.log\n";
            // Use printf with separate %s args to keep $(date) evaluating at run
            // time while keeping alsa_device (and any other interpolated value)
            // hermetically sealed against $/`/" expansion.
            script_file << "printf '%s: Launcher: Detected ALSA device: %s\\n' \"$(date)\" '" << shell_sq_escape(alsa_device) << "' >> /tmp/retroarch_launcher.log\n";
            script_file << "echo \"$(date): Launcher: GStreamer cleanup completed\" >> /tmp/retroarch_launcher.log\n";
            script_file << "# Running in background of main service - DRM master already dropped\n";
            script_file << "echo 'Launcher: Preparing to launch RetroArch...'\n";
            script_file << "echo 'Launcher: DRM master access already dropped by main service, launching RetroArch...'\n";
            script_file << "echo 'Launcher: Creating RetroArch config...'\n";
            script_file << "echo 'Launcher: ALSA device: " << shell_sq_escape(alsa_device) << "'\n";
            script_file << "echo 'Launcher: aplay -l output:' >> /tmp/retroarch_launcher.log\n";
            script_file << "aplay -l >> /tmp/retroarch_launcher.log 2>&1 || true\n";
            
            // Create Core Options file with performance-tuned settings
            script_file << "cat > /tmp/retroarch_core_options.cfg << 'OPTS'\n";
            write_core_options(script_file, core_name);
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
            // Without these RA's auto-save state silently no-ops: with no global retroarch.cfg the default libretro_info_path points to a non-existent dir, core_info_list ends up empty, savestate_support_level reads 0, and command_event_save_auto_state early-returns at the support check.
            script_file << "libretro_info_path = \"/usr/share/libretro/info\"\n";
            script_file << "core_info_savestate_bypass = \"true\"\n";
            
            // Video config (driver, resolution, viewport, sync)
            write_video_config(script_file, opts);
            // RetroArch's threaded ALSA wrapper keeps the HDMI device fed
            // independently of brief emulation or Vulkan present stalls.
            script_file << "audio_driver = \"" << audio_driver_for_gameplay()
                        << "\"\n";
            script_file << "audio_resampler = \"sinc\"\n"; // High-quality gameplay resampling

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
            // Player 2 setup mirrors player 1 — same analog mode + bind_defaults
            // policy, but tied to joypad index 1. Without these explicit lines
            // RetroArch leaves player 2 unbound (no autoconfig is present;
            // we deleted that file to force manual mappings) and a 2nd
            // identical PS-pad shows up in /dev/input/js1 but generates no
            // input events the core can see. Per-core button mappings for
            // player 2 are emitted alongside player 1 below.
            script_file << "input_player2_analog_dpad_mode = \"0\"\n";
            script_file << "input_player2_bind_defaults = \"false\"\n";
            script_file << "input_player2_joypad_index = \"1\"\n";
            script_file << "input_player2_enable = \"true\"\n";
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
            script_file << "# Don't save config on exit (prevents overwriting our settings)\n";
            script_file << "config_save_on_exit = \"false\"\n";
            script_file << "# CRITICAL: Single press to quit (don't require double press)\n";
            script_file << "quit_press_twice = \"false\"\n";
            script_file << "core_options_path = \"/tmp/retroarch_core_options.cfg\"\n";
            script_file << "# Audio settings - use ALSA to match GStreamer (simplified for reliability)\n";
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
            script_file << "audio_latency = \""
                        << audio_latency_ms_for_core(core_name) << "\"\n";
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
            script_file << "# Ensure core actually runs\n";
            script_file << "rewind_enable = \"false\"\n";
            script_file << "run_ahead_enabled = \"false\"\n";
            script_file << "netplay_enable = \"false\"\n";
            script_file << "# CRITICAL: Ensure content actually loads and runs\n";
            script_file << "content_load_auto_remap = \"false\"\n";
            script_file << "content_load_mode_manual = \"false\"\n";
            script_file << "pause_nonactive = \"false\"\n";
            
            // Trojan Horse moved to after EOF
            script_file << "echo 'Launcher: Core name is " << shell_sq_escape(core_name) << "' >> /tmp/retroarch_launcher.log\n";

            // 2. Dispatch to the per-controller mapping (controller_type detected earlier)
            ControllerMapping map = get_mapping(controller_type, core_name);

            // map.name is a hardcoded string in get_mapping_*() helpers, but
            // route through shell_sq_escape anyway as defense-in-depth.
            script_file << "# === Controller Mapping: " << map.name << " ===\n";
            script_file << "echo 'Launcher: Applying controller mapping for: " << shell_sq_escape(map.name) << "' >> /tmp/retroarch_launcher.log\n";
            
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

            // 5b. Mirror the same mapping for player 2 — assumes the
            // 2nd controller (joypad index 1) is the same physical kind
            // as P1. In our hardware that's always true (kiosk ships
            // with two identical PS-style pads). Without these mirrored
            // bindings RetroArch's per-core remap covers only player 1
            // and the 2nd pad shows up in /dev/input/js1 but produces
            // no in-game effect — symptom: P2 character sits motionless
            // in 2-player Twisted Metal / Tony Hawk / Doom split-screen.
            //
            // Hotkeys (5+ below) intentionally stay player-1-only so
            // both controllers don't fight over RA menu toggle.
            script_file << "input_player2_analog_dpad_mode = \"" << map.analog_dpad_mode << "\"\n";
            script_file << "input_player2_b_btn = \"" << map.b_btn << "\"\n";
            script_file << "input_player2_y_btn = \"" << map.y_btn << "\"\n";
            script_file << "input_player2_select_btn = \"" << map.select_btn << "\"\n";
            script_file << "input_player2_start_btn = \"" << map.start_btn << "\"\n";
            script_file << "input_player2_up_btn = \"" << map.up_btn << "\"\n";
            script_file << "input_player2_down_btn = \"" << map.down_btn << "\"\n";
            script_file << "input_player2_left_btn = \"" << map.left_btn << "\"\n";
            script_file << "input_player2_right_btn = \"" << map.right_btn << "\"\n";
            script_file << "input_player2_a_btn = \"" << map.a_btn << "\"\n";
            script_file << "input_player2_x_btn = \"" << map.x_btn << "\"\n";
            script_file << "input_player2_l_btn = \"" << map.l_btn << "\"\n";
            script_file << "input_player2_r_btn = \"" << map.r_btn << "\"\n";
            script_file << "input_player2_l2_btn = \"" << map.l2_btn << "\"\n";
            script_file << "input_player2_r2_btn = \"" << map.r2_btn << "\"\n";
            script_file << "input_player2_l_x_plus_axis = \"" << map.l_x_plus << "\"\n";
            script_file << "input_player2_l_x_minus_axis = \"" << map.l_x_minus << "\"\n";
            script_file << "input_player2_l_y_plus_axis = \"" << map.l_y_plus << "\"\n";
            script_file << "input_player2_l_y_minus_axis = \"" << map.l_y_minus << "\"\n";
            script_file << "input_player2_up_axis = \"" << map.up_axis << "\"\n";
            script_file << "input_player2_down_axis = \"" << map.down_axis << "\"\n";
            script_file << "input_player2_left_axis = \"" << map.left_axis << "\"\n";
            script_file << "input_player2_right_axis = \"" << map.right_axis << "\"\n";

            // 5c. PCSX-rearmed needs the per-pad type set for both pads.
            // Without pad2type, the second controller is treated as
            // disconnected by the PS1 BIOS — multiplayer games like
            // Twisted Metal won't see a 2nd player even if RetroArch
            // is reading js1 events.
            if (!map.core_option_pad_type.empty()) {
                script_file << "pcsx_rearmed_pad2type = \"" << map.core_option_pad_type << "\"\n";
            }

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
            script_file << "# Launch RetroArch under the KMS readiness watcher so the parent can\n";
            script_file << "# distinguish real display takeover from a stuck startup.\n";
            script_file << build_kms_ready_watch_block(retroarch_cmd, ready_options);
            script_file << "echo \"Launcher: RetroArch exited with code $RETROARCH_EXIT\" >> /tmp/retroarch_launcher.log\n";
            // Clean up temp config files (no restore needed - we used isolated /tmp config)
            script_file << "rm -f \"$UI_CONFIG\"\n";
            script_file << "rm -f /tmp/retroarch_core_options.cfg\n";
            script_file << "rm -f \"$RETROARCH_READY_FILE\"\n";
            script_file << "# CRITICAL: Autoconfig file should remain in place (not backed up/restored)\n";
            script_file << "# Clean up any old backup files from previous runs\n";
            script_file << "find \"$AUTOCONFIG_DIR\" -name '*.backup.*' -type f -mtime +1 -delete 2>/dev/null || true\n";
                script_file << "echo 'Launcher: RetroArch finished'\n";
                script_file << "# Script will exit, main service continues running\n";
                script_file << "exit \"$RETROARCH_EXIT\"\n";

            script_file.close();

            // Make script executable
            chmod(launcher_script.c_str(), 0755);
            std::cout << "Created launcher script: " << launcher_script << std::endl;
        } else {
            std::cerr << "Failed to create launcher script" << std::endl;
            return false;
            }
        }

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

        // A stale marker must never make a new launch look ready. The script
        // removes it again immediately before starting RetroArch, but doing it
        // synchronously here closes the pre-fork window as well.
        std::error_code ready_remove_error;
        fs::remove(ready_options.ready_file, ready_remove_error);
        if (ready_remove_error) {
            std::cerr << "Failed to clear RetroArch readiness marker: "
                      << ready_remove_error.message() << std::endl;
            return false;
        }
        
        // CRITICAL: Fork to run command in background (non-blocking for UI)
        pid_t launch_pid = fork();
        if (launch_pid == 0) {
            // Isolate every pre-launch helper and RetroArch itself in one
            // process group so a startup timeout can terminate all of them.
            if (setpgid(0, 0) != 0) {
                _exit(126);
            }

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
            long max_fd = sysconf(_SC_OPEN_MAX);
            if (max_fd < 0) max_fd = 1024;
            for (int fd = 3; fd < max_fd; fd++) {
                close(fd);
            }

            execl("/bin/bash", "bash", launcher_script.c_str(), nullptr);
            // If we reach here, exec failed
            std::cerr << "Failed to execute launch command" << std::endl;
            _exit(127);
        } else if (launch_pid > 0) {
            // Repeat setpgid in the parent to close the fork/exec race. EACCES
            // means the child already exec'd after successfully grouping itself.
            if (setpgid(launch_pid, launch_pid) != 0 && errno != EACCES &&
                errno != ESRCH) {
                std::cerr << "Failed to create RetroArch launch process group: "
                          << std::strerror(errno) << std::endl;
                terminate_process_group(launch_pid, std::chrono::milliseconds(500));
                return false;
            }

            std::cout << "RetroArch launch initiated (PID: " << launch_pid
                      << ", waiting up to 15 seconds for KMS)" << std::endl;
            const StartupStatus startup = wait_for_startup(
                launch_pid, ready_options.ready_file, std::chrono::seconds(15));
            if (startup != StartupStatus::Ready) {
                switch (startup) {
                    case StartupStatus::Exited:
                        std::cerr << "RetroArch exited before taking over KMS" << std::endl;
                        break;
                    case StartupStatus::TimedOut:
                        std::cerr << "RetroArch did not take over KMS within 15 seconds"
                                  << std::endl;
                        break;
                    case StartupStatus::WaitError:
                        std::cerr << "Unable to supervise RetroArch startup" << std::endl;
                        break;
                    case StartupStatus::Ready:
                        break;
                }
                terminate_process_group(launch_pid, std::chrono::milliseconds(500));
                fs::remove(ready_options.ready_file, ready_remove_error);
                return false;
            }

            std::cout << "RetroArch has taken over the KMS display" << std::endl;

            // KMS is ready; supervision is finished and adds no gameplay
            // overhead. Block until the user exits RetroArch as before.
            int status = 0;
            pid_t wait_result;
            do {
                wait_result = waitpid(launch_pid, &status, 0);
            } while (wait_result < 0 && errno == EINTR);

            fs::remove(ready_options.ready_file, ready_remove_error);
            if (wait_result < 0) {
                std::cerr << "Failed while waiting for RetroArch to exit: "
                          << std::strerror(errno) << std::endl;
                return true;
            }

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

    // Core downloader doesn't have audio preference context, use HDMI detection
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
            // No `set -e` — same rationale as the game launcher above: the
            // best-effort setup preamble must not abort the RetroArch
            // (core-downloader) launch on a cosmetic non-zero exit. The
            // downloader's outcome is observed via RetroArch itself, not a
            // mid-preamble error.
            script_file << "echo \"$(date): Downloader: Starting RetroArch downloader script\" >> /tmp/retroarch_launcher.log\n";
            // printf form keeps $(date) live while shell_sq_escape'ing the value.
            // See the launcher path above for the full rationale.
            script_file << "printf '%s: Downloader: Detected ALSA device: %s\\n' \"$(date)\" '" << shell_sq_escape(alsa_device) << "' >> /tmp/retroarch_launcher.log\n";
            script_file << "echo \"$(date): Downloader: GStreamer cleanup completed\" >> /tmp/retroarch_launcher.log\n";
            script_file << "echo 'Downloader: Waiting for main app cleanup...'\n";
            script_file << "sleep 3\n";  // Wait for main app to fully exit and clean up DRM resources
            script_file << "echo 'Downloader: Creating RetroArch config...'\n";
            script_file << "echo 'Downloader: ALSA device: " << shell_sq_escape(alsa_device) << "'\n";
            script_file << "echo 'Downloader: aplay -l output:' >> /tmp/retroarch_launcher.log\n";
            script_file << "aplay -l >> /tmp/retroarch_launcher.log 2>&1 || true\n";
            script_file << "cat > /tmp/retroarch_launcher.cfg << 'EOF'\n";
            script_file << "# DRM/KMS RetroArch config for Magic Dingus Box\n";
            // Video config (driver, resolution, viewport, sync)
            LaunchOptions default_opts;  // core downloader always runs in CRT mode
            write_video_config(script_file, default_opts);
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
            // Player 2 setup mirrors player 1 — same analog mode + bind_defaults
            // policy, but tied to joypad index 1. Without these explicit lines
            // RetroArch leaves player 2 unbound (no autoconfig is present;
            // we deleted that file to force manual mappings) and a 2nd
            // identical PS-pad shows up in /dev/input/js1 but generates no
            // input events the core can see. Per-core button mappings for
            // player 2 are emitted alongside player 1 below.
            script_file << "input_player2_analog_dpad_mode = \"0\"\n";
            script_file << "input_player2_bind_defaults = \"false\"\n";
            script_file << "input_player2_joypad_index = \"1\"\n";
            script_file << "input_player2_enable = \"true\"\n";
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
            script_file << "# Ensure core actually runs\n";
            script_file << "rewind_enable = \"false\"\n";
            script_file << "run_ahead_enabled = \"false\"\n";
            script_file << "netplay_enable = \"false\"\n";
            script_file << "# CRITICAL: Ensure content actually loads and runs\n";
            script_file << "content_load_auto_remap = \"false\"\n";
            script_file << "content_load_mode_manual = \"false\"\n";
            script_file << "pause_nonactive = \"false\"\n";
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

    // ALSA settle wait. This used to be gated on `popen("lsof | grep snd")`
    // — a scan of EVERY process's fd table (100ms-1s+ on this Pi with the
    // Docker stack up) whose outcome was constant: PulseAudio always holds
    // /dev/snd devices open by design on this system, so "device busy" was
    // always true and the 300ms wait always fired anyway. Keeping the wait
    // unconditional preserves the exact effective timing while dropping the
    // scan cost from every game launch.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::cout << "GStreamer cleanup complete" << std::endl;
}

} // namespace retroarch
