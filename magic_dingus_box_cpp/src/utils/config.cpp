#include "config.h"
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace config {

// =============================================================================
// Base Paths
// =============================================================================

std::string get_base_path() {
    if (const char* env = std::getenv("MAGIC_BASE_PATH")) {
        return env;
    }
    return "/opt/magic_dingus_box";
}

std::string get_app_path() {
    if (const char* env = std::getenv("MAGIC_APP_PATH")) {
        return env;
    }
    return get_base_path() + "/magic_dingus_box_cpp";
}

std::string get_data_path() {
    if (const char* env = std::getenv("MAGIC_DATA_PATH")) {
        return env;
    }
    return get_app_path() + "/data";
}

std::string get_config_path() {
    if (const char* env = std::getenv("MAGIC_CONFIG_PATH")) {
        return env;
    }
    return get_base_path() + "/config";
}

std::string get_assets_path() {
    if (const char* env = std::getenv("MAGIC_ASSETS_PATH")) {
        return env;
    }
    return get_app_path() + "/assets";
}

std::string get_home_path() {
    if (const char* env = std::getenv("HOME")) {
        return env;
    }
    return "/home/magic";
}

// =============================================================================
// Specific File/Directory Paths
// =============================================================================

std::string get_settings_file() {
    if (const char* env = std::getenv("MAGIC_SETTINGS_FILE")) {
        return env;
    }
    return get_config_path() + "/settings.json";
}

std::string get_log_file() {
    if (const char* env = std::getenv("MAGIC_LOG_FILE")) {
        return env;
    }
    // Return empty string to disable file logging, or return a path to enable it
    // File logging can be disabled by setting MAGIC_LOG_FILE=""
    return get_config_path() + "/magic_dingus_box.log";
}

std::string get_playlists_dir() {
    return get_data_path() + "/playlists";
}

std::string get_bezels_file() {
    return get_assets_path() + "/bezels/bezels.json";
}

std::string get_bezels_dir() {
    return get_assets_path() + "/bezels";
}

std::string get_font_file() {
    return get_assets_path() + "/fonts/ZenDots-Regular.ttf";
}

std::string get_logo_file() {
    return get_assets_path() + "/Logos/logo_resized.png";
}

std::string get_intro_dir() {
    return get_data_path() + "/intro";
}

// =============================================================================
// RetroArch Paths
// =============================================================================

namespace retroarch {

std::string get_config_dir() {
    return get_home_path() + "/.config/retroarch";
}

std::string get_cores_dir() {
    return get_config_dir() + "/cores";
}

std::string get_system_dir() {
    return get_config_dir() + "/system";
}

std::string get_launcher_script() {
    return get_home_path() + "/retroarch_launcher.sh";
}

std::string get_launcher_log() {
    return get_home_path() + "/retroarch_launcher.log";
}

std::string get_saves_dir() {
    return get_data_path() + "/saves";
}

std::string get_states_dir() {
    return get_data_path() + "/states";
}

} // namespace retroarch

// =============================================================================
// Path Resolution Helpers
// =============================================================================

std::vector<std::string> get_search_paths(const std::string& relative_path) {
    return {
        "../" + relative_path,                              // Relative to build dir
        relative_path,                                      // Current directory
        get_app_path() + "/" + relative_path               // Absolute path
    };
}

std::vector<std::string> get_playlist_search_paths() {
    return {
        "../data/playlists",
        "data/playlists",
        get_playlists_dir()
    };
}

std::vector<std::string> get_font_search_paths() {
    return {
        "../assets/fonts/ZenDots-Regular.ttf",
        "assets/fonts/ZenDots-Regular.ttf",
        get_font_file()
    };
}

std::vector<std::string> get_bezel_search_paths() {
    return {
        "../assets/bezels/bezels.json",
        "assets/bezels/bezels.json",
        get_bezels_file()
    };
}

std::vector<std::string> get_intro_search_paths() {
    std::vector<std::string> paths;
    std::vector<std::string> filenames = {
        "intro.30fps.mov",
        "intro.mov",
        "intro.30fps.mp4",
        "intro.mp4"
    };

    // For each base path, add all filename variants
    std::vector<std::string> base_paths = {
        "../data/intro",
        "data/intro",
        get_intro_dir()
    };

    for (const auto& base : base_paths) {
        for (const auto& filename : filenames) {
            paths.push_back(base + "/" + filename);
        }
    }

    return paths;
}

std::vector<std::string> get_logo_search_paths() {
    return {
        "../assets/Logos/logo_resized.png",
        "assets/Logos/logo_resized.png",
        get_logo_file()
    };
}

} // namespace config
