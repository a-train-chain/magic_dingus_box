#include "status_writer.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <json/json.h>
#include "utils/logger.h"

namespace app {

StatusWriter::StatusWriter(std::string path)
    : path_(std::move(path)),
      tmp_path_(path_ + ".tmp"),
      owning_thread_(std::this_thread::get_id()) {}

void StatusWriter::write_now(const AppState& state) {
    assert(std::this_thread::get_id() == owning_thread_ &&
           "StatusWriter::write_now called from non-owning thread");
    Json::Value root;
    root["schema"] = 1;

    using namespace std::chrono;
    auto now = system_clock::now().time_since_epoch();
    root["ts"] = duration_cast<duration<double>>(now).count();
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

    if (state.screen_mode.load() == ScreenMode::RetroArch) {
        Json::Value ra;
        ra["rom_name"] = state.retroarch_rom_name;
        ra["core"]     = state.retroarch_core;
        root["retroarch"] = ra;
    } else {
        root["retroarch"] = Json::Value::null;
    }

    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    std::string out = Json::writeString(b, root);

    {
        std::ofstream f(tmp_path_, std::ios::binary | std::ios::trunc);
        f.write(out.data(), static_cast<std::streamsize>(out.size()));
        if (!f.good()) {
            LOG_WARN("StatusWriter: failed to write tmp file {}", tmp_path_);
            std::remove(tmp_path_.c_str());
            return;
        }
    }
    if (std::rename(tmp_path_.c_str(), path_.c_str()) != 0) {
        LOG_WARN("StatusWriter: rename {} -> {} failed (errno {})",
                 tmp_path_, path_, errno);
        std::remove(tmp_path_.c_str());
    }
}

} // namespace app
