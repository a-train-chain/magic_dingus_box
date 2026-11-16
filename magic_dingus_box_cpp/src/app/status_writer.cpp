#include "status_writer.h"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <thread>
#include <json/json.h>
#include "utils/logger.h"
#include "ui/virtual_keyboard.h"
#include "tube/tube_frame_class.h"

namespace app {

namespace {

bool write_file_atomically(const std::string& tmp_path,
                           const std::string& path,
                           const std::string& body) {
    {
        std::ofstream f(tmp_path, std::ios::binary | std::ios::trunc);
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!f.good()) {
            LOG_WARN("StatusWriter: failed to write tmp file {}", tmp_path);
            std::remove(tmp_path.c_str());
            return false;
        }
    }
    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        LOG_WARN("StatusWriter: rename {} -> {} failed (errno {})",
                 tmp_path, path, errno);
        std::remove(tmp_path.c_str());
        return false;
    }
    return true;
}

// A string_view as a JSON string without the intermediate std::string.
Json::Value json_word(std::string_view word) {
    return Json::Value(word.data(), word.data() + word.size());
}

// The Tube engine's published state (the web page reads it). Offered: the
// engine, the resolved look/mask/44 values, what was moved and the web op log's
// progress. Not offered: the engine and the reason only. Runs on every status
// refresh, so it only reads the state and builds one small object.
Json::Value tube_status_json(const AppState& state) {
    const bool offered = tube_offered(state);
    const CrtEngine saved = state.display_settings.crt_engine;
    const CrtEngine effective = effective_crt_engine(saved, offered);

    Json::Value t(Json::objectValue);
    t["gate"] = offered;
    t["engine"] = json_word(crt_engine_name(saved));
    t["effective_engine"] = json_word(crt_engine_name(effective));
    if (!offered) {
        t["reason"] = json_word(tube_not_offered_reason(state));
        return t;
    }

    const tube::TubeSetting& setting = state.display_settings.tube;
    const tube::Resolved resolved = tube::resolve(setting);
    t["crt_native"] = state.display_settings.mode == DisplayMode::CRT_NATIVE;
    t["look"] = json_word(tube::look_name(resolved.look));
    t["mask"] = json_word(tube::mask_name(resolved.mask));
    t["custom"] = resolved.custom;
    Json::Value moved(Json::arrayValue);
    for (std::string_view key : tube::moved_keys(setting)) moved.append(json_word(key));
    t["moved"] = std::move(moved);
    Json::Value values(Json::objectValue);
    for (std::size_t i = 0; i < tube::kEffectCount; ++i)
        values[std::string(tube::effect_key(tube::kEffects[i]))] = resolved.effects[i];
    t["values"] = std::move(values);
    // The live rate while the renderer is drawing; before that (or with it idle) the
    // rate this setting will draw at, so the page never shows a stale default.
    const bool crt_native = state.display_settings.mode == DisplayMode::CRT_NATIVE;
    t["frame_rate"] = state.tube_render.drawing
        ? state.tube_render.frame_rate
        : tube::frames_per_second(tube::setting_frame_class(resolved, crt_native));
    t["too_heavy"] = state.tube_render.too_heavy;
    if (state.tube_render.unavailable.empty()) t["unavailable"] = Json::Value::null;
    else t["unavailable"] = state.tube_render.unavailable;
    t["handled_seq"] = static_cast<Json::UInt64>(state.tube_live.handled_seq);
    t["degauss_requests"] = static_cast<Json::UInt64>(state.tube_live.degauss_requests);
    return t;
}

} // namespace

StatusWriter::StatusWriter(std::string path)
    : StatusWriter(std::move(path), write_file_atomically) {}

StatusWriter::StatusWriter(std::string path, FileSink sink)
    : path_(std::move(path)),
      tmp_path_(path_ + ".tmp"),
      sink_(std::move(sink)),
      owning_thread_(std::this_thread::get_id()) {}

// worker_ is the last member, so its destructor runs first: it finishes
// the write in hand and the latest pending one, then joins.
StatusWriter::~StatusWriter() = default;

void StatusWriter::flush() {
    worker_.flush();
}

void StatusWriter::write_now(const AppState& state) {
    assert(std::this_thread::get_id() == owning_thread_ &&
           "StatusWriter::write_now called from non-owning thread");
    Json::Value root;
    root["schema"] = 1;

    root["screen"] = screen_mode_to_string(state.screen_mode.load());

    // Playlist context — best-effort; readers tolerate nulls.
    Json::Value playlist;
    playlist["name"]       = state.current_playlist_name;
    // current_item_index is a plain int (not atomic); read directly.
    playlist["item_index"] = state.current_item_index;
    playlist["item_count"] = state.current_item_count;
    root["playlist"] = playlist;

    Json::Value np;
    np["title"]    = state.now_playing_title;
    np["subtitle"] = state.now_playing_subtitle;
    np["kind"]     = state.now_playing_kind;
    root["now_playing"] = np;

    Json::Value playback;
    // position/duration are private with thread-safe accessors (get_position/get_duration).
    playback["position_sec"] = state.get_position();
    playback["duration_sec"] = state.get_duration();
    // paused is the existing atomic<bool> field (plan called it is_paused — adapted here).
    playback["is_paused"]    = state.paused.load();
    root["playback"] = playback;

    // Is the main UI drawn over the video right now? During playback with
    // the overlay hidden, SELECT does NOT select — it reveals the overlay
    // and returns early (see main.cpp's SELECT handler). The phone remote
    // relabels its centre key "Browse" in that state so it advertises what
    // the press will actually do, rather than claiming "Enter" and doing
    // something else.
    root["overlay_visible"] = state.ui_visible_when_playing;

    if (state.screen_mode.load() == ScreenMode::RetroArch) {
        Json::Value ra;
        ra["rom_name"] = state.retroarch_rom_name;
        ra["core"]     = state.retroarch_core;
        root["retroarch"] = ra;
    } else {
        root["retroarch"] = Json::Value::null;
    }

    // Settings / game-browser cursor — enables closed-loop navigation for
    // the emulator smoke-test harness. game_playlist_name/games_in_playlist
    // are derived from game_playlists for the currently-open game playlist.
    Json::Value sm(Json::objectValue);
    sm["active"]                = state.sm_active;
    sm["highlighted_label"]     = state.sm_highlighted_label;
    sm["selected_index"]        = state.sm_selected_index;
    sm["game_browser_active"]   = state.sm_game_browser_active;
    sm["viewing_games"]         = state.sm_viewing_games;
    sm["game_browser_selected"] = state.sm_game_browser_selected;
    sm["game_playlist_index"]   = state.sm_game_playlist_index;
    sm["selected_game_index"]   = state.sm_selected_game_index;
    sm["game_playlist_count"]   = static_cast<int>(state.game_playlists.size());
    // Index -> title for every game playlist, in browser order. The harness
    // needs this to target one system ("--playlist dreamcast") instead of
    // launching N games out of all nine; without it the only way to learn a
    // playlist's name was to open it, so covering one system's whole library
    // meant a full sweep of every other system too.
    Json::Value playlist_names(Json::arrayValue);
    for (const auto& gp : state.game_playlists) {
        playlist_names.append(gp.title);
    }
    sm["game_playlist_names"]   = playlist_names;
    if (state.sm_game_playlist_index >= 0 &&
        state.sm_game_playlist_index < static_cast<int>(state.game_playlists.size())) {
        const auto& gp = state.game_playlists[state.sm_game_playlist_index];
        sm["game_playlist_name"] = gp.title;
        sm["games_in_playlist"]  = static_cast<int>(gp.items.size());
    } else {
        sm["game_playlist_name"] = "";
        sm["games_in_playlist"]  = 0;
    }
    root["settings"] = sm;

    Json::Value text_input(Json::objectValue);
    if (state.active_text_keyboard != nullptr) {
        text_input["active"] = true;
        text_input["title"]  = state.active_text_title;
        text_input["buffer"] = state.active_text_keyboard->get_text();
    } else {
        text_input["active"] = false;
    }
    root["text_input"] = text_input;

    root["tube"] = tube_status_json(state);

    // TEST-ONLY MDB_PLATFORM_POLICY_OVERRIDE ("pi4" while a Pi 5 runs the
    // Pi 4B's software policies, null otherwise). Published so the state
    // can never ship silently: verify_box.sh FAILS when this is non-null.
    if (state.platform_profile.policy_override.empty()) {
        root["platform_policy_override"] = Json::Value::null;
    } else {
        root["platform_policy_override"] = state.platform_profile.policy_override;
    }

    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    // Serialize WITHOUT ts first: this is the change-detection key. The
    // writer used to run at 5Hz unconditionally — ~432k tmp+rename
    // cycles/day of SD metadata churn on a box that idles at a menu for
    // most of its life, with ts guaranteeing every body differed. Now:
    // skip when nothing but the timestamp would change, EXCEPT every 2s
    // as a heartbeat. Both freshness consumers stay honest — verify_box
    // asserts ts < 5s old, and the phone remote's broadcaster polls
    // mtime (it also gets fewer no-op parse wakeups). Real content
    // changes (position ticks during playback, screen transitions, game
    // session begin/end) still write immediately.
    const std::string body = Json::writeString(b, root);
    const auto mono_now = std::chrono::steady_clock::now();
    // A failed write is reported back by the worker and resent here (the
    // old synchronous code left last_body_ stale for the same effect).
    // Cannot wait on the SD card: the worker holds its lock only for
    // flag/mailbox swaps, never across a write.
    const bool resend = worker_.take_failure();
    if (!resend && body == last_body_ &&
        mono_now - last_write_ < std::chrono::seconds(2)) {
        return;
    }

    using namespace std::chrono;
    root["ts"] = duration_cast<duration<double>>(
        system_clock::now().time_since_epoch()).count();
    std::string out = Json::writeString(b, root);

    // Latest wins: this replaces a snapshot the worker has not started.
    // sink_ and the paths never change after construction and outlive
    // worker_ (see the header), so capturing `this` is safe.
    worker_.submit([this, out = std::move(out)] {
        return sink_(tmp_path_, path_, out);
    });
    last_body_ = body;
    last_write_ = mono_now;
}

} // namespace app
