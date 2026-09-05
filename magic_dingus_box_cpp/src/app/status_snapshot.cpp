#include "status_snapshot.h"

#include "app_state.h"
#include "ui/virtual_keyboard.h"

namespace app {

Json::Value build_status_json(const AppState& state) {
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

    return root;
}

}  // namespace app
