#include "settings_persistence.h"
#include "mb_filter_persistence.h"
#include "../utils/config.h"
#include "../utils/fsync_util.h"
#include "../utils/latest_wins_worker.h"
#include "../utils/logger.h"
#include "../utils/services_env.h"
#include <json/json.h>
#include <fstream>
#include <iostream>
#include <string_view>
#include <filesystem>

namespace fs = std::filesystem;

namespace {
const char* mb_library_sort_to_string(app::AppState::DisplaySettings::MbLibrarySort s) {
    using S = app::AppState::DisplaySettings::MbLibrarySort;
    switch (s) {
        case S::Recent: return "recent";
        case S::Title:  return "title";
        case S::Year:   return "year";
        case S::Size:   return "size";
    }
    return "recent";
}
app::AppState::DisplaySettings::MbLibrarySort mb_library_sort_from_string(const std::string& s) {
    using R = app::AppState::DisplaySettings::MbLibrarySort;
    if (s == "title") return R::Title;
    if (s == "year")  return R::Year;
    if (s == "size")  return R::Size;
    return R::Recent;  // Default for "recent" or any unknown value.
}
const char* mb_library_filter_to_string(app::AppState::DisplaySettings::MbLibraryFilter f) {
    using F = app::AppState::DisplaySettings::MbLibraryFilter;
    switch (f) {
        case F::All:            return "all";
        case F::Unwatched:      return "unwatched";
        case F::MissingFiles:   return "missing_files";
        case F::RecentlyAdded:  return "recently_added";
    }
    return "all";
}
app::AppState::DisplaySettings::MbLibraryFilter mb_library_filter_from_string(const std::string& s) {
    using R = app::AppState::DisplaySettings::MbLibraryFilter;
    if (s == "unwatched")      return R::Unwatched;
    if (s == "missing_files")  return R::MissingFiles;
    if (s == "recently_added") return R::RecentlyAdded;
    return R::All;
}

#ifdef MEDIA_BROWSER_ENABLED
// True iff services/.env sets WIREGUARD_PRIVATE_KEY to a non-empty value.
// Failure-closed: a missing file or any I/O error reads as false. MB-only
// caller, so guarded (the MB-OFF test build compiles this file with -Wextra).
bool read_vpn_configured_from_services_env() {
    return !utils::read_env_value(utils::kServicesEnvPath,
                                  "WIREGUARD_PRIVATE_KEY").empty();
}
#endif

}  // namespace

namespace app {

std::string SettingsPersistence::get_settings_path() {
    return config::get_settings_file();
}

std::string SettingsPersistence::serialize(const AppState& state) {
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
    // Media Browser Library overlay sort + filter (v1.6.x). String
    // enums for human-readable JSON.
    display["mb_library_sort"]   = mb_library_sort_to_string(
        state.display_settings.mb_library_sort);
    display["mb_library_filter"] = mb_library_filter_to_string(
        state.display_settings.mb_library_filter);
    // Per-mode discover filters + Movies/TV mode. The movie key names are
    // unchanged from v1.6.4 so a live settings.json round-trips untouched;
    // the TV keys and mb_mode are MB-only so the ENABLE_MEDIA_BROWSER=OFF
    // binary's output stays byte-identical.
#ifdef MEDIA_BROWSER_ENABLED
    app::mb_filters_to_json(state.display_settings, display, /*include_tv=*/true);
#else
    app::mb_filters_to_json(state.display_settings, display, /*include_tv=*/false);
#endif
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

    // Same bytes the old stream writer produced (it wrote the document,
    // then std::endl).
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    return Json::writeString(builder, root) + "\n";
}

bool SettingsPersistence::write_file_durably(const std::string& path,
                                             const std::string& body) {
    // Create directory if it doesn't exist
    fs::path dir = fs::path(path).parent_path();
    if (!dir.empty() && !fs::exists(dir)) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            LOG_ERROR("Failed to create config directory: {}", ec.message());
            return false;
        }
    }

    // Write to temporary file first for atomic save (crash-safe)
    std::string tmp_path = path + ".tmp";
    std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        LOG_ERROR("Failed to open temp settings file for writing: {}", tmp_path);
        return false;
    }
    file.write(body.data(), static_cast<std::streamsize>(body.size()));
    file.close();

    // Check for write errors
    if (file.fail()) {
        LOG_ERROR("Failed to write settings to temp file: {}", tmp_path);
        std::error_code ec;
        fs::remove(tmp_path, ec);
        return false;
    }

    // Durability: flush the temp file's DATA before the rename, or a
    // power cut can journal the rename ahead of the data and leave a
    // zero-length settings.json — which peek_is_crt_native() silently
    // reads as "reboot into CRT mode". Best-effort (see fsync_util.h):
    // a failed fsync degrades to exactly the old behavior, so it warns
    // rather than failing the save. This is the call that can take
    // seconds on a busy card — the reason this whole function runs on the
    // writer thread and never on the render loop.
    if (!utils::fsync_file(tmp_path)) {
        LOG_WARN("fsync of {} failed — save proceeds without durability guarantee", tmp_path);
    }

    // Atomically rename temp file to target
    std::error_code ec;
    fs::rename(tmp_path, path, ec);
    if (ec) {
        LOG_ERROR("Failed to rename settings file: {}", ec.message());
        fs::remove(tmp_path, ec);
        return false;
    }

    // Make the rename itself durable (directory entry update).
    (void)utils::fsync_parent_dir(path);

    LOG_DEBUG("Settings saved to {}", path);
    return true;
}

namespace {

// The background writer. Process-lifetime: main() ends in _exit() (after
// flush_pending_saves), and on an ordinary return from main its destructor
// finishes the pending save.
utils::LatestWinsWorker& settings_writer() {
    // The writer logs from its own thread, also while it drains during
    // static destruction. Touch spdlog's registry first so it is
    // constructed before — and therefore destroyed after — the worker.
    (void)spdlog::default_logger();
    static utils::LatestWinsWorker worker;
    return worker;
}

// Main-thread-only: the last snapshot handed to the writer and where it
// goes, kept for flush_pending_saves' one retry.
struct LastSave {
    std::string path;
    std::string body;
    SettingsPersistence::FileSink sink;
};
LastSave& last_save() {
    static LastSave s;
    return s;
}

SettingsPersistence::FileSink& test_sink() {
    static SettingsPersistence::FileSink s;
    return s;
}

void submit_last_save() {
    // Captured by value: the job outlives this call, and a newer save
    // overwrites last_save() while it runs.
    const LastSave& s = last_save();
    settings_writer().submit([path = s.path, body = s.body, sink = s.sink] {
        return sink(path, body);
    });
}

}  // namespace

void SettingsPersistence::set_file_sink_for_tests(FileSink sink) {
    test_sink() = std::move(sink);
}

utils::Result<> SettingsPersistence::save_settings(const AppState& state) {
    LastSave& s = last_save();
    // Path and sink are resolved here, on the caller, so the job depends
    // on nothing that can change before it runs.
    s.path = get_settings_path();
    s.body = serialize(state);
    s.sink = test_sink() ? test_sink() : FileSink(&SettingsPersistence::write_file_durably);
    submit_last_save();
    return utils::Result<>::ok();
}

bool SettingsPersistence::save_pending() {
    return settings_writer().busy();
}

bool SettingsPersistence::flush_pending_saves(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto& writer = settings_writer();
    if (!writer.flush_until(deadline)) return false;
    if (!writer.take_failure()) return true;
    // The last save failed (card full or read-only at that moment). One
    // more attempt: at shutdown this is the last chance to keep the
    // operator's final change.
    if (!last_save().sink) return false;
    LOG_WARN("Last settings save failed — retrying once");
    submit_last_save();
    if (!writer.flush_until(deadline)) return false;
    return !writer.take_failure();
}

utils::Result<> SettingsPersistence::reload_from_disk(AppState& state) {
    // Sampled BEFORE the load. Only the main thread queues saves, so
    // "nothing outstanding" here stays true for the whole load.
    const bool save_outstanding = save_pending();
    auto result = load_settings(state);
    if (save_outstanding) {
        LOG_INFO("Settings reload overtook an unfinished save — writing the "
                 "reloaded settings back so the disk matches memory");
        (void)save_settings(state);
    }
    return result;
}

bool SettingsPersistence::peek_is_crt_native() {
    // Deliberately duplicates a few lines of load_settings' file/parse
    // handling rather than depending on AppState: this runs before the
    // display exists, and must not pull in or mutate application state.
    // Every failure path returns true (CRT_NATIVE) — the conservative
    // answer, since that is the 720p path an HDMI->composite converter
    // is known to accept.
    std::ifstream file(get_settings_path());
    if (!file.is_open()) return true;

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    if (!Json::parseFromStream(builder, file, &root, &errors)) return true;

    if (!root.isObject() || !root.isMember("display")) return true;
    const Json::Value& display = root["display"];
    if (!display.isObject() || !display.isMember("mode")) return true;

    try {
        return display["mode"].asString() != "modern_tv";
    } catch (const std::exception&) {
        return true;  // wrong-typed mode: same safe default as unreadable
    }
}

// jsoncpp's as*()/isMember() THROW on a type mismatch ("bezel_index":"2",
// "display":[...]). settings.json arrives from web-admin restores and
// hand edits, and an escaped exception here crashed the kiosk before the
// first frame — then again on every systemd restart, a black screen that
// only SSH could fix. A file that cannot be read is moved aside (kept for
// diagnosis, never overwritten by the next save) and the box boots on
// defaults plus whatever fields were applied before the bad one.
utils::Result<> SettingsPersistence::load_settings(AppState& state) {
    try {
        return load_settings_unguarded(state);
    } catch (const std::exception& e) {
        const std::string path = get_settings_path();
        const std::string quarantine = path + ".corrupt";
        std::error_code ec;
        std::filesystem::rename(path, quarantine, ec);
        LOG_ERROR("settings.json unreadable ({}); moved to {}{} — using defaults",
                  e.what(), quarantine,
                  ec ? " FAILED: " + ec.message() : std::string());
        return utils::Result<>::fail(std::string("settings.json unreadable: ") + e.what());
    }
}

utils::Result<> SettingsPersistence::load_settings_unguarded(AppState& state) {
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
        // Library overlay sort + filter — defaults are the same as the
        // struct defaults (Recent / All) so an operator upgrading from
        // pre-v1.6.x sees no visual change to their library on first
        // launch.
        state.display_settings.mb_library_sort = mb_library_sort_from_string(
            display.get("mb_library_sort", "recent").asString());
        state.display_settings.mb_library_filter = mb_library_filter_from_string(
            display.get("mb_library_filter", "all").asString());
        // Per-mode discover filters + Movies/TV mode. Every key is
        // absent-tolerant: a pre-2c settings.json loads its movie filters
        // and gets default (unfiltered) TV sets with mb_mode = movies.
        app::mb_filters_from_json(display, state.display_settings);
    }

    // Load playback settings with safe defaults
    if (root.isMember("playback")) {
        const Json::Value& playback = root["playback"];

        state.playlist_loop = playback.get("playlist_loop", true).asBool();
        state.shuffle = playback.get("shuffle", false).asBool();
#ifdef MEDIA_BROWSER_ENABLED
        state.media_browser_unlocked = playback.get("media_browser_unlocked", false).asBool();
        state.media_browser_vpn_configured = read_vpn_configured_from_services_env();
        // Diagnostic: log the loaded gate values so operator can debug
        // "menu doesn't show" issues without recompiling. Three layers
        // must all pass for the menu row to appear; this surfaces the
        // first two at boot (third is set later by VpnHealthMonitor).
        LOG_INFO("[settings] media_browser gate at load: unlocked={} vpn_configured={}",
                 state.media_browser_unlocked,
                 state.media_browser_vpn_configured);
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
