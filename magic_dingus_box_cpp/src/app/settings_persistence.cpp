#include "settings_persistence.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <sys/stat.h>
#include <sys/types.h>

namespace app {

std::string SettingsPersistence::get_settings_path() {
    return "/opt/magic_dingus_box/config/settings.json";
}

bool SettingsPersistence::save_settings(const AppState& state) {
    std::string path = get_settings_path();
    
    // Create directory if it doesn't exist
    std::string dir = "/opt/magic_dingus_box/config";
    struct stat st = {0};
    if (stat(dir.c_str(), &st) == -1) {
        mkdir(dir.c_str(), 0755);
    }
    
    // Build JSON string manually (simple approach)
    std::ostringstream json;
    json << "{\n";
    json << "  \"display\": {\n";
    json << "    \"scanline_intensity\": " << state.display_settings.scanline_intensity << ",\n";
    json << "    \"warmth_intensity\": " << state.display_settings.warmth_intensity << ",\n";
    json << "    \"glow_intensity\": " << state.display_settings.glow_intensity << ",\n";
    json << "    \"rgb_mask_intensity\": " << state.display_settings.rgb_mask_intensity << ",\n";
    json << "    \"bloom_intensity\": " << state.display_settings.bloom_intensity << ",\n";
    json << "    \"interlacing_intensity\": " << state.display_settings.interlacing_intensity << ",\n";
    json << "    \"flicker_intensity\": " << state.display_settings.flicker_intensity << "\n";
    json << "  },\n";
    json << "  \"playback\": {\n";
    json << "    \"playlist_loop\": " << (state.playlist_loop ? "true" : "false") << ",\n";
    json << "    \"shuffle\": " << (state.shuffle ? "true" : "false") << ",\n";
    json << "    \"master_volume\": " << state.master_volume << "\n";
    json << "  }\n";
    json << "}\n";
    
    // Write to file
    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "Failed to save settings to " << path << std::endl;
        return false;
    }
    
    file << json.str();
    file.close();
    
    std::cout << "Settings saved to " << path << std::endl;
    return true;
}

bool SettingsPersistence::load_settings(AppState& state) {
    std::string path = get_settings_path();
    
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cout << "No settings file found at " << path << ", using defaults" << std::endl;
        return false;
    }
    
    // Simple JSON parsing (manual approach for simplicity)
    std::string line;
    while (std::getline(file, line)) {
        // Trim whitespace
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        
        // Parse key-value pairs
        if (line.find("\"scanline_intensity\":") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                std::string value = line.substr(pos + 1);
                state.display_settings.scanline_intensity = std::stof(value);
            }
        } else if (line.find("\"warmth_intensity\":") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                std::string value = line.substr(pos + 1);
                state.display_settings.warmth_intensity = std::stof(value);
            }
        } else if (line.find("\"glow_intensity\":") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                std::string value = line.substr(pos + 1);
                state.display_settings.glow_intensity = std::stof(value);
            }
        } else if (line.find("\"rgb_mask_intensity\":") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                std::string value = line.substr(pos + 1);
                state.display_settings.rgb_mask_intensity = std::stof(value);
            }
        } else if (line.find("\"bloom_intensity\":") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                std::string value = line.substr(pos + 1);
                state.display_settings.bloom_intensity = std::stof(value);
            }
        } else if (line.find("\"interlacing_intensity\":") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                std::string value = line.substr(pos + 1);
                state.display_settings.interlacing_intensity = std::stof(value);
            }
        } else if (line.find("\"flicker_intensity\":") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                std::string value = line.substr(pos + 1);
                state.display_settings.flicker_intensity = std::stof(value);
            }
        } else if (line.find("\"playlist_loop\":") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                std::string value = line.substr(pos + 1);
                state.playlist_loop = (value.find("true") != std::string::npos);
            }
        } else if (line.find("\"shuffle\":") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                std::string value = line.substr(pos + 1);
                state.shuffle = (value.find("true") != std::string::npos);
            }
        } else if (line.find("\"master_volume\":") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                std::string value = line.substr(pos + 1);
                try {
                    state.master_volume = std::stoi(value);
                    // Clamp volume
                    if (state.master_volume < 0) state.master_volume = 0;
                    if (state.master_volume > 100) state.master_volume = 100;
                } catch (...) {
                    state.master_volume = 100;
                }
            }
        }
    }
    
    file.close();
    
    std::cout << "Settings loaded from " << path << std::endl;
    return true;
}

} // namespace app
