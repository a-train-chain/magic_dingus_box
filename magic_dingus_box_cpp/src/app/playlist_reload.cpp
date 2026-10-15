#include "playlist_reload.h"

#include "playback_reset.h"

#include <algorithm>
#include <utility>

namespace app {

PlaylistLoader::UiPlaylistSplit split_for_ui_with_master_shuffle(
    const std::vector<Playlist>& loaded) {
    auto split = PlaylistLoader::split_for_ui(loaded);
    // Insert "Master Shuffle" playlist at the beginning
    Playlist master_shuffle;
    master_shuffle.title = "Master Shuffle";
    master_shuffle.path = ""; // Virtual path
    master_shuffle.items.push_back({}); // Dummy item to make it selectable
    split.video.insert(split.video.begin(), master_shuffle);
    return split;
}

std::string playlist_identity(const Playlist& p) {
    // path is the playlist's file on disk and is unique; only the virtual
    // Master Shuffle row has none.
    return p.path.empty() ? ("title:" + p.title) : ("path:" + p.path);
}

int find_playlist_by_identity(const std::vector<Playlist>& v, const std::string& id) {
    for (size_t i = 0; i < v.size(); ++i) {
        if (playlist_identity(v[i]) == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

PlaylistReloadSnapshot snapshot_for_reload(const AppState& state,
                                           int open_game_playlist_index) {
    PlaylistReloadSnapshot snap;
    if (state.current_playlist_index >= 0 &&
        state.current_playlist_index <
            static_cast<int>(state.playlists.size())) {
        const auto& old_pl =
            state.playlists[state.current_playlist_index];
        snap.playing_id = playlist_identity(old_pl);
        if (state.current_item_index >= 0 &&
            state.current_item_index <
                static_cast<int>(old_pl.items.size())) {
            snap.playing_item_path =
                old_pl.items[state.current_item_index].path;
            snap.playing_item_title =
                old_pl.items[state.current_item_index].title;
        }
    }
    if (state.selected_index >= 0 &&
        state.selected_index <
            static_cast<int>(state.playlists.size())) {
        snap.selected_id = playlist_identity(state.playlists[state.selected_index]);
    }
    if (open_game_playlist_index >= 0 &&
        open_game_playlist_index < static_cast<int>(state.game_playlists.size())) {
        snap.open_game_id = playlist_identity(state.game_playlists[open_game_playlist_index]);
    }
    return snap;
}

void apply_reloaded_playlists(AppState& state,
                              PlaylistLoader::UiPlaylistSplit reloaded,
                              const PlaylistReloadSnapshot& snap,
                              const std::function<void()>& stop_playback) {
    state.playlists = std::move(reloaded.video);
    // main.cpp's game_playlists is a REFERENCE to this member: the Settings
    // browser's ROM launch and the renderer read the same vector, so they
    // cannot disagree about which ROM row 3 is.
    state.game_playlists = std::move(reloaded.games);

    // Both shuffle queues and the master-shuffle history are (playlist,
    // item) coordinates into the vectors just replaced. Keeping them would
    // play files nobody picked; they regenerate lazily on the next advance.
    state.shuffle_queue.clear();
    state.shuffle_queue_position = 0;
    state.shuffle_queue_playlist_id = -1;
    state.master_shuffle_queue.clear();
    state.master_shuffle_queue_position = 0;
    state.shuffle_history.clear();

    // ── Playback is never interrupted, only re-anchored ──
    // Explicit decision: whatever is on screen keeps playing to its natural
    // end. Only the coordinates describing what comes NEXT are repaired.
    if (!snap.playing_id.empty()) {
        const int new_pl = find_playlist_by_identity(state.playlists, snap.playing_id);
        if (new_pl >= 0) {
            state.current_playlist_index = new_pl;
            const auto& pl = state.playlists[new_pl];
            int new_item = -1;
            for (size_t i = 0; i < pl.items.size(); ++i) {
                if (pl.items[i].path == snap.playing_item_path &&
                    pl.items[i].title == snap.playing_item_title) {
                    new_item = static_cast<int>(i);
                    break;
                }
            }
            if (new_item >= 0) {
                state.current_item_index = new_item;
            } else if (!pl.items.empty()) {
                // The playing item was edited out. Keep advancing inside the
                // playlist the operator chose; just clamp.
                if (state.current_item_index < 0) {
                    state.current_item_index = 0;
                }
                if (state.current_item_index >=
                    static_cast<int>(pl.items.size())) {
                    state.current_item_index =
                        static_cast<int>(pl.items.size()) - 1;
                }
            } else {
                state.current_item_index = -1;
            }
        } else if (state.master_shuffle_active &&
                   state.playlists.size() > 1) {
            // Master Shuffle's SOURCE playlist is gone but the pool is not
            // empty. Park on row 0 — the virtual Master Shuffle entry,
            // always present — so auto-advance and NEXT still fire; both
            // call play_random_global_video(), which re-picks from the new
            // pool and overwrites these coordinates. Row 0 is also the
            // correct "now playing" highlight while Master Shuffle runs.
            state.current_playlist_index = 0;
            state.current_item_index = 0;
        } else {
            // Either the playlist that owned this video is gone, or Master
            // Shuffle has no pool left (size() <= 1 means only the virtual
            // row survived).
            //
            // Both must STOP, not merely unset the index. Leaving
            // video_active true parks the TV on the last decoded frame with
            // no UI, recoverable only by an operator guessing to press
            // SELECT. And an empty Master Shuffle pool is worse: EOS calls
            // play_random_global_video() every frame, which early-returns
            // without clearing playback_started_, so it re-fires at frame
            // rate and floods stderr until the box is power-cycled.
            state.master_shuffle_active = false;
            if (stop_playback) stop_playback();
            app::stop_to_menu(state);
        }

        // The auto-advance guard is an item index into the OLD playlist
        // too. If a remap happens to land current_item_index on that stale
        // value, can_advance stays false and the video ends with nothing
        // following it. Clearing it is what the loop does anyway one frame
        // into the next item.
        state.last_advanced_item_index = -1;
        state.last_advanced_duration = 0.0;
    }

    // ── Main-menu cursor follows its playlist ────
    int new_sel = snap.selected_id.empty()
                      ? -1
                      : find_playlist_by_identity(state.playlists, snap.selected_id);
    if (new_sel < 0) new_sel = state.selected_index;
    if (new_sel >= static_cast<int>(state.playlists.size())) {
        new_sel = static_cast<int>(state.playlists.size()) - 1;
    }
    if (new_sel < 0) new_sel = 0;
    state.selected_index = new_sel;
    {
        const int max_visible = std::max(1, state.playlist_max_visible);
        int max_scroll =
            static_cast<int>(state.playlists.size()) - max_visible;
        if (max_scroll < 0) max_scroll = 0;
        if (state.selected_index < state.playlist_scroll_offset) {
            state.playlist_scroll_offset = state.selected_index;
        }
        if (state.selected_index >=
            state.playlist_scroll_offset + max_visible) {
            state.playlist_scroll_offset =
                state.selected_index - max_visible + 1;
        }
        if (state.playlist_scroll_offset > max_scroll) {
            state.playlist_scroll_offset = max_scroll;
        }
        if (state.playlist_scroll_offset < 0) {
            state.playlist_scroll_offset = 0;
        }
    }
}

OpenGameListRemap remap_open_game_list(const std::vector<Playlist>& game_playlists,
                                       const std::string& open_game_id,
                                       int current_open_index) {
    OpenGameListRemap r;
    const int gi =
        open_game_id.empty()
            ? -1
            : find_playlist_by_identity(game_playlists, open_game_id);
    if (gi < 0) {
        // The open game playlist is gone. Back out to the list: at item
        // level the only exit ("Back") is bounds-checked against that
        // playlist, so a dead index would trap the operator on a screen
        // with no way out but a power cycle.
        r.action = OpenGameListRemap::Action::Exit;
    } else if (gi != current_open_index) {
        r.action = OpenGameListRemap::Action::Enter;
        r.index = gi;
    }
    return r;
}

int games_in_game_playlist(const std::vector<Playlist>& game_playlists, int index) {
    if (index >= 0 && index < static_cast<int>(game_playlists.size())) {
        return static_cast<int>(game_playlists[index].items.size());
    }
    return 0;
}

}  // namespace app
