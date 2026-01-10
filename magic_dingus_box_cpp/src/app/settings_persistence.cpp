#include "settings_persistence.h"
#include "../utils/config.h"
#include "../utils/logger.h"
#include <json/json.h>
#include <fstream>
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

namespace app {

std::string SettingsPersistence::get_settings_path() {
    return config::get_settings_file();
}

utils::Result<> SettingsPersistence::save_settings(const AppState& state) {
    std::string path = get_settings_path();

    // Create directory if it doesn't exist
    fs::path dir = fs::path(path).parent_path();
    if (!fs::exists(dir)) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            std::string error = "Failed to create config directory: " + ec.message();
            LOG_ERROR("{}", error);
            return utils::Result<>::fail(error);
        }
    }

    // Build JSON using JsonCpp
    Json::Value root;

    // Display settings
    Json::Value display;
    display["mode"] = (state.display_settings.mode == DisplayMode::CRT_NATIVE) ? "crt_native" : "modern_tv";
    display["bezel_index"] = state.display_settings.bezel_index;
    display["scanline_intensity"] = state.display_settings.scanline_intensity;
    display["warmth_intensity"] = state.display_settings.warmth_intensity;
    display["glow_intensity"] = state.display_settings.glow_intensity;
    display["rgb_mask_intensity"] = state.display_settings.rgb_mask_intensity;
    display["bloom_intensity"] = state.display_settings.bloom_intensity;
    display["interlacing_intensity"] = state.display_settings.interlacing_intensity;
    display["flicker_intensity"] = state.display_settings.flicker_intensity;
    root["display"] = display;

    // Playback settings
    Json::Value playback;
    playback["playlist_loop"] = state.playlist_loop;
    playback["shuffle"] = state.shuffle;
    playback["master_volume"] = state.master_volume;
    root["playback"] = playback;

    // Write to file with styled formatting
    std::ofstream file(path);
    if (!file.is_open()) {
        std::string error = "Failed to open settings file for writing: " + path;
        LOG_ERROR("{}", error);
        return utils::Result<>::fail(error);
    }

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
    writer->write(root, &file);
    file << std::endl;
    file.close();

    LOG_DEBUG("Settings saved to {}", path);
    return utils::Result<>::ok();
}

utils::Result<> SettingsPersistence::load_settings(AppState& state) {
    std::string path = get_settings_path();

    std::ifstream file(path);
    if (!file.is_open()) {
        std::string info = "No settings file found at " + path + ", using defaults";
        LOG_DEBUG("{}", info);
        // This is not really an error - just no file yet
        return utils::Result<>::fail(info);
    }

    // Parse JSON using JsonCpp
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;

    if (!Json::parseFromStream(builder, file, &root, &errors)) {
        std::string error = "Failed to parse settings JSON: " + errors;
        LOG_ERROR("{}", error);
        file.close();
        return utils::Result<>::fail(error);
    }
    file.close();

    // Load display settings with safe defaults
    if (root.isMember("display")) {
        const Json::Value& display = root["display"];

        std::string mode = display.get("mode", "crt_native").asString();
        state.display_settings.mode = (mode == "modern_tv") ? DisplayMode::MODERN_TV : DisplayMode::CRT_NATIVE;

        state.display_settings.bezel_index = display.get("bezel_index", 0).asInt();
        state.display_settings.scanline_intensity = display.get("scanline_intensity", 0.0f).asFloat();
        state.display_settings.warmth_intensity = display.get("warmth_intensity", 0.0f).asFloat();
        state.display_settings.glow_intensity = display.get("glow_intensity", 0.0f).asFloat();
        state.display_settings.rgb_mask_intensity = display.get("rgb_mask_intensity", 0.0f).asFloat();
        state.display_settings.bloom_intensity = display.get("bloom_intensity", 0.0f).asFloat();
        state.display_settings.interlacing_intensity = display.get("interlacing_intensity", 0.0f).asFloat();
        state.display_settings.flicker_intensity = display.get("flicker_intensity", 0.0f).asFloat();
    }

    // Load playback settings with safe defaults
    if (root.isMember("playback")) {
        const Json::Value& playback = root["playback"];

        state.playlist_loop = playback.get("playlist_loop", true).asBool();
        state.shuffle = playback.get("shuffle", false).asBool();

        int volume = playback.get("master_volume", config::audio::DEFAULT_VOLUME).asInt();
        // Clamp volume to valid range
        state.master_volume = std::max(config::audio::MIN_VOLUME, std::min(config::audio::MAX_VOLUME, volume));
    }

    LOG_DEBUG("Settings loaded from {}", path);
    return utils::Result<>::ok();
}

} // namespace app
