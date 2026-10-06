#pragma once

// The playlist vectors the main menu and the Settings game browser show,
// built once at boot and rebuilt by the web-admin playlist reload poke
// (data/playlists_reload_request) — and the re-anchoring that reload needs
// so whatever is on screen keeps playing.
//
// Pure AppState logic, moved out of main.cpp verbatim and unit-tested on the
// Mac (tests/app/test_playlist_reload.cpp). main.cpp keeps the marker
// polling and the Settings-menu calls; every decision is here.

#include <functional>
#include <string>
#include <vector>

#include "app_state.h"
#include "playlist_loader.h"

namespace app {

// Split for the two UI surfaces, then prepend the virtual Master Shuffle
// row. Boot and the runtime reload both go through this ONE function — a
// second hand-written copy is precisely how the boot menu and the reloaded
// menu would drift apart.
//
// The main menu gets only non-game items (it plays unattended —
// auto-advance, next/prev, Master Shuffle must never launch RetroArch), the
// Settings game browser gets only game items. A mixed playlist appears on
// both sides, each holding its kind.
PlaylistLoader::UiPlaylistSplit split_for_ui_with_master_shuffle(
    const std::vector<Playlist>& loaded);

// A playlist's identity across a reload. path is the playlist's file on disk
// and is unique; only the virtual Master Shuffle row has none.
std::string playlist_identity(const Playlist& p);
// Index of the playlist with that identity, or -1.
int find_playlist_by_identity(const std::vector<Playlist>& v, const std::string& id);

// Everything the kiosk is holding — what is playing, where the menu cursor
// sits, which game list is open — is an INDEX into the vectors about to be
// replaced, and importing one playlist renumbers every playlist after it.
// So re-find things by identity, not index. Taken BEFORE the swap.
struct PlaylistReloadSnapshot {
    std::string playing_id;
    std::string playing_item_path;
    std::string playing_item_title;
    std::string selected_id;
    std::string open_game_id;
};

// open_game_playlist_index: the Settings game browser's open game list
// (-1 when the browser is not viewing one).
PlaylistReloadSnapshot snapshot_for_reload(const AppState& state,
                                           int open_game_playlist_index);

// Swap the reloaded vectors in and repair every coordinate that pointed
// into the old ones: shuffle queues/history, the playing item, the
// auto-advance guard, the main-menu cursor and scroll offset. Playback is
// never interrupted, only re-anchored — except when the playlist that owned
// the playing video is gone (or Master Shuffle has no pool left), where
// `stop_playback` (controller.stop()) runs followed by stop_to_menu.
void apply_reloaded_playlists(AppState& state,
                              PlaylistLoader::UiPlaylistSplit reloaded,
                              const PlaylistReloadSnapshot& snap,
                              const std::function<void()>& stop_playback);

// What the Settings game browser must do with its open game list after the
// reload (only meaningful while it is viewing one).
struct OpenGameListRemap {
    enum class Action {
        Keep,   // same playlist, same index
        Exit,   // the open game playlist is gone: back out to the list
        Enter,  // it moved: re-enter it at `index`
    };
    Action action = Action::Keep;
    int index = -1;
};
OpenGameListRemap remap_open_game_list(const std::vector<Playlist>& game_playlists,
                                       const std::string& open_game_id,
                                       int current_open_index);

// Game count of game playlist `index` (0 when out of range) — the second
// bound navigate(0, ...) re-clamps the item cursor against.
int games_in_game_playlist(const std::vector<Playlist>& game_playlists, int index);

}  // namespace app
