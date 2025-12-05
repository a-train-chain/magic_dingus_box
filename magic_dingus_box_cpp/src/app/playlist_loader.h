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
};

} // namespace app

