#include "playlist_loader.h"

#include <yaml-cpp/yaml.h>
#include <fstream>
#include <iostream>
#include <experimental/filesystem>

namespace fs = std::experimental::filesystem;

namespace app {

std::vector<Playlist> PlaylistLoader::load_playlists(const std::string& directory) {
    std::vector<Playlist> playlists;
    
    try {
        for (const auto& entry : fs::directory_iterator(directory)) {
            // Check if it's a regular file using status
            auto status = entry.status();
            if (fs::is_regular_file(status) && entry.path().extension() == ".yaml") {
                try {
                    Playlist pl = load_playlist(entry.path().string());
                    playlists.push_back(pl);
                } catch (const std::exception& e) {
                    std::cerr << "Failed to load playlist " << entry.path() << ": " << e.what() << std::endl;
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to read playlist directory: " << e.what() << std::endl;
    }
    
    return playlists;
}

Playlist PlaylistLoader::load_playlist(const std::string& path) {
    Playlist pl;
    pl.path = path;
    
    try {
        YAML::Node node = YAML::LoadFile(path);
        
        if (node["title"]) {
            pl.title = node["title"].as<std::string>();
        }
        
        if (node["curator"]) {
            pl.curator = node["curator"].as<std::string>();
        }
        
        if (node["items"]) {
            for (const auto& item : node["items"]) {
                PlaylistItem playlist_item;
                
                if (item.IsScalar()) {
                    // Simple string path - assume it's a local video
                    playlist_item.path = item.as<std::string>();
                    playlist_item.source_type = "local";
                    // Extract title from filename (without extension)
                    std::string path_str = playlist_item.path;
                    size_t last_slash = path_str.find_last_of("/\\");
                    size_t last_dot = path_str.find_last_of(".");
                    if (last_dot != std::string::npos && (last_slash == std::string::npos || last_dot > last_slash)) {
                        playlist_item.title = path_str.substr((last_slash == std::string::npos ? 0 : last_slash + 1), last_dot - (last_slash == std::string::npos ? 0 : last_slash + 1));
                    } else {
                        playlist_item.title = path_str.substr(last_slash == std::string::npos ? 0 : last_slash + 1);
                    }
                    playlist_item.artist = "";  // No artist for simple path format
                } else {
                    // Object with path and potentially source_type, title, artist
                    if (item["path"]) {
                        playlist_item.path = item["path"].as<std::string>();
                    }
                    if (item["source_type"]) {
                        playlist_item.source_type = item["source_type"].as<std::string>();
                    } else {
                        // Default to "local" if not specified
                        playlist_item.source_type = "local";
                    }
                    if (item["title"]) {
                        playlist_item.title = item["title"].as<std::string>();
                    } else {
                        // Extract title from filename if not provided
                        std::string path_str = playlist_item.path;
                        size_t last_slash = path_str.find_last_of("/\\");
                        size_t last_dot = path_str.find_last_of(".");
                        if (last_dot != std::string::npos && (last_slash == std::string::npos || last_dot > last_slash)) {
                            playlist_item.title = path_str.substr((last_slash == std::string::npos ? 0 : last_slash + 1), last_dot - (last_slash == std::string::npos ? 0 : last_slash + 1));
                        } else {
                            playlist_item.title = path_str.substr(last_slash == std::string::npos ? 0 : last_slash + 1);
                        }
                    }
                    if (item["artist"]) {
                        playlist_item.artist = item["artist"].as<std::string>();
                    } else {
                        playlist_item.artist = "";  // Empty if not provided
                    }
                    // Parse emulator fields (for games)
                    if (item["emulator_core"]) {
                        playlist_item.emulator_core = item["emulator_core"].as<std::string>();
                    } else {
                        playlist_item.emulator_core = "";  // Empty if not provided
                    }
                    if (item["emulator_system"]) {
                        playlist_item.emulator_system = item["emulator_system"].as<std::string>();
                    } else {
                        playlist_item.emulator_system = "";  // Empty if not provided
                    }
                }
                
                pl.items.push_back(playlist_item);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error parsing YAML: " << e.what() << std::endl;
        throw;
    }
    
    return pl;
}

} // namespace app

