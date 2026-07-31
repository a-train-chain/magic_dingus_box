#pragma once

#include "app_state.h"
#include "platform/platform_profile.h"
#include <string>
#include <vector>

namespace app {

class PlaylistLoader {
public:
    // Load all playlists from directory
    static std::vector<Playlist> load_playlists(const std::string& directory);

    // Remove emulated_game items whose emulator_system this board cannot
    // run (platform::supports_game_system) and drop playlists that end up
    // with no items. Video/youtube items always pass. One golden image
    // serves Pi 4B and Pi 5, so the image carries every system's content;
    // this is the gate that keeps Pi 5-only systems (N64, Dreamcast) off
    // a Pi 4 menu. Playlists that were ALREADY empty on disk pass
    // through untouched — hiding those is not this gate's business.
    static std::vector<Playlist> filter_for_platform(
        std::vector<Playlist> playlists,
        const platform::PlatformProfile& profile);

    // Partition playlists between the two UI surfaces. The main menu is
    // lean-back (auto-advance, next/prev, Master Shuffle run unattended),
    // so it must never hold an emulated_game item — games launch ONLY
    // from the Settings game browser, which is deliberate/lean-forward.
    // A mixed video+game playlist appears on BOTH sides, each copy
    // holding only its kind of item:
    //   - video: game items removed; kept only if at least one real
    //     video item remains (unknown source_types ride along, matching
    //     the old main-menu behavior)
    //   - games: emulated_game items only; kept only if non-empty
    // Runs AFTER filter_for_platform, so a Pi 4B's split never
    // resurrects N64/Dreamcast items.
    struct UiPlaylistSplit {
        std::vector<Playlist> video;   // main menu
        std::vector<Playlist> games;   // Settings game browser
    };
    static UiPlaylistSplit split_for_ui(const std::vector<Playlist>& playlists);

    // Load single playlist from YAML file
    static Playlist load_playlist(const std::string& path);

#ifdef MEDIA_BROWSER_ENABLED
    // Host path of Radarr's root folder on the Pi. Must agree with
    // RadarrClient::Config::host_library_prefix and setup_services.sh's
    // storage layout: movies live directly at library/<Title (Year)>/ —
    // there is NO Movies/ subdirectory (the old library/Movies/ default
    // pointed at a dir Radarr never wrote to, so the synthetic playlist
    // never populated on fielded boxes).
    static constexpr const char* kMoviesLibraryRoot = "/mnt/ssd/library";

    // Scan a Radarr-style movies library directory (one subdir per movie)
    // and synthesize a single "Movies" playlist. Safe to call when the
    // directory does not exist — returns a Playlist with an empty items
    // vector in that case.
    static Playlist load_movies_library(
        const std::string& directory = kMoviesLibraryRoot);
#endif
};

} // namespace app
