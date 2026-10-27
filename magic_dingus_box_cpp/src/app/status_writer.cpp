#include "status_writer.h"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <json/json.h>
#include "utils/logger.h"
#include "ui/virtual_keyboard.h"

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

} // namespace

StatusWriter::StatusWriter(std::string path)
    : StatusWriter(std::move(path), write_file_atomically) {}

StatusWriter::StatusWriter(std::string path, FileSink sink)
    : path_(std::move(path)),
      tmp_path_(path_ + ".tmp"),
      sink_(std::move(sink)),
      owning_thread_(std::this_thread::get_id()) {
    writer_ = std::thread([this] { writer_loop(); });
}

StatusWriter::~StatusWriter() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (writer_.joinable()) writer_.join();
}

void StatusWriter::writer_loop() {
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
        cv_.wait(lk, [this] { return has_pending_ || stop_; });
        if (!has_pending_) break;  // stopping, and the final state is out
        std::string body = std::move(pending_);
        has_pending_ = false;
        writing_ = true;
        // The sink runs WITHOUT the lock: it is the part that can take
        // seconds, and write_now() must be able to replace pending_ (and
        // see write_failed_) the whole time.
        lk.unlock();
        const bool ok = sink_(tmp_path_, path_, body);
        lk.lock();
        writing_ = false;
        if (!ok) write_failed_ = true;
        cv_.notify_all();  // flush()
    }
}

void StatusWriter::flush() {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [this] { return !has_pending_ && !writing_; });
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
    bool resend = false;
    {
        // Held only for flag/mailbox swaps — the writer never holds mu_
        // across a write, so this cannot wait on the SD card.
        std::lock_guard<std::mutex> lk(mu_);
        resend = write_failed_;
        write_failed_ = false;
    }
    if (!resend && body == last_body_ &&
        mono_now - last_write_ < std::chrono::seconds(2)) {
        return;
    }

    using namespace std::chrono;
    root["ts"] = duration_cast<duration<double>>(
        system_clock::now().time_since_epoch()).count();
    std::string out = Json::writeString(b, root);

    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_ = std::move(out);
        has_pending_ = true;
    }
    cv_.notify_all();
    // A failed write is reported back through write_failed_ and resent on
    // the next call (the old synchronous code left last_body_ stale for the
    // same effect).
    last_body_ = body;
    last_write_ = mono_now;
}

} // namespace app
