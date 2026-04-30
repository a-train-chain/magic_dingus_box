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
    // Enhanced CRT pipeline opt-in (Phase 1+ shader rework).
    // Persisted so operators can flip between classic and enhanced
    // visuals once and have the choice survive reboots.
    display["enhanced_crt_enabled"] = state.display_settings.enhanced_crt_enabled;
    // Media Browser wood-frame overlay during movie playback (v1.6.x).
    // Default true — see app_state.h docstring for behavior details.
    display["mb_playback_show_frame"] = state.display_settings.mb_playback_show_frame;
    // Media Browser CRT overlay intensities — independent from the
    // kiosk-side values above. Edited via MovieSettings → "CRT overlay"
    // rows. Persisted with the same `mb_` key prefix so the JSON layout
    // makes the dual-store relationship obvious to anyone inspecting
    // settings.json by hand.
    display["mb_scanline_intensity"]    = state.display_settings.mb_scanline_intensity;
    display["mb_warmth_intensity"]      = state.display_settings.mb_warmth_intensity;
    display["mb_glow_intensity"]        = state.display_settings.mb_glow_intensity;
    display["mb_rgb_mask_intensity"]    = state.display_settings.mb_rgb_mask_intensity;
    display["mb_bloom_intensity"]       = state.display_settings.mb_bloom_intensity;
    display["mb_interlacing_intensity"] = state.display_settings.mb_interlacing_intensity;
    display["mb_flicker_intensity"]     = state.display_settings.mb_flicker_intensity;
    root["display"] = display;

    // Playback settings
    Json::Value playback;
    playback["playlist_loop"] = state.playlist_loop;
    playback["shuffle"] = state.shuffle;
    playback["master_volume"] = state.master_volume;
#ifdef MEDIA_BROWSER_ENABLED
    playback["media_browser_unlocked"] = state.media_browser_unlocked;
#endif
    root["playback"] = playback;

    // Audio settings
    Json::Value audio;
    std::string output_str = "auto";
    if (state.audio_settings.output == AudioOutput::HDMI) output_str = "hdmi";
    else if (state.audio_settings.output == AudioOutput::HEADPHONE) output_str = "headphone";
    audio["output"] = output_str;
    audio["retroarch_volume_offset_db"] = state.audio_settings.retroarch_volume_offset_db;
    root["audio"] = audio;

    // Write to temporary file first for atomic save (crash-safe)
    std::string tmp_path = path + ".tmp";
    std::ofstream file(tmp_path);
    if (!file.is_open()) {
        std::string error = "Failed to open temp settings file for writing: " + tmp_path;
        LOG_ERROR("{}", error);
        return utils::Result<>::fail(error);
    }

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
    writer->write(root, &file);
    file << std::endl;
    file.close();

    // Check for write errors
    if (file.fail()) {
        std::string error = "Failed to write settings to temp file: " + tmp_path;
        LOG_ERROR("{}", error);
        std::error_code ec;
        fs::remove(tmp_path, ec);
        return utils::Result<>::fail(error);
    }

    // Atomically rename temp file to target
    std::error_code ec;
    fs::rename(tmp_path, path, ec);
    if (ec) {
        std::string error = "Failed to rename settings file: " + ec.message();
        LOG_ERROR("{}", error);
        fs::remove(tmp_path, ec);
        return utils::Result<>::fail(error);
    }

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
        return utils::Result<>::ok();
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
        // Enhanced CRT pipeline opt-in. Default false so an operator
        // upgrading from a settings.json that pre-dates this key keeps
        // the classic procedural-overlay behavior with zero visible
        // change until they explicitly turn it on.
        state.display_settings.enhanced_crt_enabled = display.get("enhanced_crt_enabled", false).asBool();
        // Media Browser wood-frame overlay during movie playback (v1.6.x).
        // Default true so operators upgrading from a settings.json that
        // pre-dates this key see the wood frame during playback by
        // default (matches the Marquee design intent).
        state.display_settings.mb_playback_show_frame =
            display.get("mb_playback_show_frame", true).asBool();
        // Media Browser CRT overlay intensities (v1.6.x). When the
        // mb_* keys aren't present in settings.json (upgrade path),
        // INHERIT the kiosk-side values that we just loaded above —
        // so an operator upgrading from a build that didn't have a
        // separate MB store sees no visual regression on their
        // Marquee menus the first frame after install. New installs
        // get 0.0 across the board (since the kiosk fields default
        // to 0.0 too). Once the operator changes any MB intensity
        // through the MovieSettings rows, the divergence is
        // persisted on the next save_settings call.
        state.display_settings.mb_scanline_intensity =
            display.get("mb_scanline_intensity",
                        state.display_settings.scanline_intensity).asFloat();
        state.display_settings.mb_warmth_intensity =
            display.get("mb_warmth_intensity",
                        state.display_settings.warmth_intensity).asFloat();
        state.display_settings.mb_glow_intensity =
            display.get("mb_glow_intensity",
                        state.display_settings.glow_intensity).asFloat();
        state.display_settings.mb_rgb_mask_intensity =
            display.get("mb_rgb_mask_intensity",
                        state.display_settings.rgb_mask_intensity).asFloat();
        state.display_settings.mb_bloom_intensity =
            display.get("mb_bloom_intensity",
                        state.display_settings.bloom_intensity).asFloat();
        state.display_settings.mb_interlacing_intensity =
            display.get("mb_interlacing_intensity",
                        state.display_settings.interlacing_intensity).asFloat();
        state.display_settings.mb_flicker_intensity =
            display.get("mb_flicker_intensity",
                        state.display_settings.flicker_intensity).asFloat();
    }

    // Load playback settings with safe defaults
    if (root.isMember("playback")) {
        const Json::Value& playback = root["playback"];

        state.playlist_loop = playback.get("playlist_loop", true).asBool();
        state.shuffle = playback.get("shuffle", false).asBool();
#ifdef MEDIA_BROWSER_ENABLED
        state.media_browser_unlocked = playback.get("media_browser_unlocked", false).asBool();
#endif

        int volume = playback.get("master_volume", config::audio::DEFAULT_VOLUME).asInt();
        // Clamp volume to valid range
        state.master_volume = std::max(config::audio::MIN_VOLUME, std::min(config::audio::MAX_VOLUME, volume));
    }

    // Load audio settings with safe defaults
    if (root.isMember("audio")) {
        const Json::Value& audio = root["audio"];
        
        std::string output_str = audio.get("output", "auto").asString();
        if (output_str == "hdmi") state.audio_settings.output = AudioOutput::HDMI;
        else if (output_str == "headphone") state.audio_settings.output = AudioOutput::HEADPHONE;
        else state.audio_settings.output = AudioOutput::AUTO;
        
        state.audio_settings.retroarch_volume_offset_db = audio.get("retroarch_volume_offset_db", 0.0f).asFloat();
        
        // Apply audio output setting on load
        state.audio_settings.apply_output();
    }

    LOG_DEBUG("Settings loaded from {}", path);
    return utils::Result<>::ok();
}

} // namespace app
