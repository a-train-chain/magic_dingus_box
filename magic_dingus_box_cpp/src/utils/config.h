#pragma once

#include <string>
#include <vector>

namespace config {

// =============================================================================
// Base Paths - Can be overridden by environment variables
// =============================================================================

// Get the base installation path (MAGIC_BASE_PATH or /opt/magic_dingus_box)
std::string get_base_path();

// Get the C++ application path ($BASE/magic_dingus_box_cpp)
std::string get_app_path();

// Get the data directory ($APP/data)
std::string get_data_path();

// Get the config directory ($BASE/config)
std::string get_config_path();

// Get the assets directory ($APP/assets)
std::string get_assets_path();

// Get the user home directory (HOME or /home/magic)
std::string get_home_path();

// =============================================================================
// Specific File/Directory Paths
// =============================================================================

// Settings file ($CONFIG/settings.json)
std::string get_settings_file();

// Log file ($CONFIG/magic_dingus_box.log)
std::string get_log_file();

// Playlists directory ($DATA/playlists)
std::string get_playlists_dir();

// Bezels JSON file ($ASSETS/bezels/bezels.json)
std::string get_bezels_file();

// Bezels directory ($ASSETS/bezels)
std::string get_bezels_dir();

// Font file ($ASSETS/fonts/ZenDots-Regular.ttf)
std::string get_font_file();

// Logo file ($ASSETS/Logos/logo_resized.png)
std::string get_logo_file();

// Intro video directory ($DATA/intro)
std::string get_intro_dir();

// =============================================================================
// RetroArch Paths
// =============================================================================

namespace retroarch {
    // RetroArch config directory ($HOME/.config/retroarch)
    std::string get_config_dir();

    // RetroArch cores directory ($HOME/.config/retroarch/cores)
    std::string get_cores_dir();

    // RetroArch system directory ($HOME/.config/retroarch/system)
    std::string get_system_dir();

    // Launcher script path ($HOME/retroarch_launcher.sh)
    std::string get_launcher_script();

    // Launcher log path ($HOME/retroarch_launcher.log)
    std::string get_launcher_log();

    // Saves directory ($DATA/saves) - SRAM saves
    std::string get_saves_dir();

    // Savestates directory ($DATA/states) - emulator snapshots
    std::string get_states_dir();
}

// =============================================================================
// Display Constants
// =============================================================================

namespace display {
    // Preferred resolutions in order of preference
    constexpr int PREFERRED_WIDTH = 1280;
    constexpr int PREFERRED_HEIGHT = 720;

    // Fallback resolutions
    constexpr int FALLBACK_WIDTH_1 = 1920;
    constexpr int FALLBACK_HEIGHT_1 = 1080;
    constexpr int FALLBACK_WIDTH_2 = 640;
    constexpr int FALLBACK_HEIGHT_2 = 480;

    // Maximum safe height for CRT native mode
    constexpr int CRT_MAX_HEIGHT = 720;
}

// =============================================================================
// Timing Constants (in milliseconds unless noted)
// =============================================================================

namespace timing {
    // Frame timing
    constexpr int FRAME_TIME_MS = 16;           // ~60 FPS target

    // UI visibility
    constexpr double UI_VISIBILITY_SEC = 3.0;   // Seconds to keep UI visible
    constexpr int FADE_DURATION_MS = 300;       // Fade in/out duration

    // Playlist/video transitions
    constexpr int PLAYLIST_SWITCH_TIMEOUT_MS = 5000;
    constexpr int STOP_DELAY_MS = 200;
    constexpr int PLAYBACK_START_DELAY_MS = 200;
    constexpr int PLAYBACK_VERIFY_DELAY_MS = 100;
    constexpr int MPV_STOP_RETRY_DELAY_MS = 50;
    constexpr int MPV_STOP_MAX_RETRIES = 5;

    // Intro video
    constexpr int INTRO_WAIT_FRAMES = 200;
    constexpr int INTRO_FADE_DURATION_MS = 500;

    // Input handling
    constexpr int LONG_PRESS_THRESHOLD_MS = 500;
    constexpr int KEY_REPEAT_INITIAL_MS = 500;
    constexpr int KEY_REPEAT_INTERVAL_MS = 100;
}

// =============================================================================
// Audio Constants
// =============================================================================

namespace audio {
    constexpr int DEFAULT_VOLUME = 100;
    constexpr int MIN_VOLUME = 0;
    constexpr int MAX_VOLUME = 100;
    constexpr int VOLUME_INCREMENT = 5;
    constexpr float DIMMED_VOLUME_FACTOR = 0.3f;  // Volume when UI is visible
}

// =============================================================================
// CRT Effect Defaults
// =============================================================================

namespace crt {
    constexpr float DEFAULT_SCANLINE = 0.1f;
    constexpr float DEFAULT_WARMTH = 0.0f;
    constexpr float DEFAULT_GLOW = 0.0f;
    constexpr float DEFAULT_RGB_MASK = 0.0f;
    constexpr float DEFAULT_BLOOM = 0.0f;
    constexpr float DEFAULT_INTERLACING = 0.0f;
    constexpr float DEFAULT_FLICKER = 0.0f;

    // Intensity cycle values: OFF -> Low -> Medium -> High -> OFF
    constexpr float INTENSITY_OFF = 0.0f;
    constexpr float INTENSITY_LOW = 0.15f;
    constexpr float INTENSITY_MEDIUM = 0.30f;
    constexpr float INTENSITY_HIGH = 0.50f;
}

// =============================================================================
// Path Resolution Helpers
// =============================================================================

// Get list of paths to search for a file (tries multiple locations)
std::vector<std::string> get_search_paths(const std::string& relative_path);

// Get list of paths to search for playlists
std::vector<std::string> get_playlist_search_paths();

// Get list of paths to search for fonts
std::vector<std::string> get_font_search_paths();

// Get list of paths to search for bezels
std::vector<std::string> get_bezel_search_paths();

// Get list of paths to search for intro videos
std::vector<std::string> get_intro_search_paths();

// Get list of paths to search for logo
std::vector<std::string> get_logo_search_paths();

} // namespace config
