#pragma once

#include "app_state.h"
#include <string>
#include <vector>

namespace app {

class PlaylistLoader {
public:
    // Load all playlists from directory
    static std::vector<Playlist> load_playlists(const std::string& directory);

    // Load single playlist from YAML file
    static Playlist load_playlist(const std::string& path);

    // Scan a Radarr-style movies library directory (one subdir per movie)
    // and synthesize a single "Movies" playlist. Safe to call when the
    // directory does not exist — returns a Playlist with an empty items
    // vector in that case. The default `directory` matches Radarr's root
    // folder on the Pi.
    static Playlist load_movies_library(
        const std::string& directory = "/mnt/ssd/library/Movies");
};

} // namespace app

