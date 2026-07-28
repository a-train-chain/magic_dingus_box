#include "playlist_loader.h"

#include "rom_title.h"

#include <yaml-cpp/yaml.h>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

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
    
    // Sort playlists alphabetically by title (case-insensitive)
    std::sort(playlists.begin(), playlists.end(),
        [](const Playlist& a, const Playlist& b) {
            // Case-insensitive string comparison
            std::string a_lower = a.title;
            std::string b_lower = b.title;
            std::transform(a_lower.begin(), a_lower.end(), a_lower.begin(),
                [](unsigned char c) { return std::tolower(c); });
            std::transform(b_lower.begin(), b_lower.end(), b_lower.begin(),
                [](unsigned char c) { return std::tolower(c); });
            return a_lower < b_lower;
        });
    
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
        
        // Per-playlist loop. Absent means false; see Playlist::loop.
        if (node["loop"]) {
            try {
                pl.loop = node["loop"].as<bool>();
            } catch (const std::exception&) {
                pl.loop = false;  // non-bool value: treat as not looping
            }
        }

        if (node["items"]) {
            for (const auto& item : node["items"]) {
                PlaylistItem playlist_item;
                
                if (item.IsScalar()) {
                    // Simple string path - assume it's a local video
                    playlist_item.path = item.as<std::string>();
                    playlist_item.source_type = "local";
                    // Derive a DISPLAY title from the filename: drops the
                    // No-Intro region/revision tags so the UI shows
                    // "Super Mario 64", not "Super Mario 64 (USA)".
                    playlist_item.title = title_from_rom_path(playlist_item.path);
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
                        // No explicit title — derive one from the filename,
                        // minus the No-Intro region/revision tags.
                        playlist_item.title = title_from_rom_path(playlist_item.path);
                    }
                    if (item["artist"]) {
                        playlist_item.artist = item["artist"].as<std::string>();
                    } else {
                        playlist_item.artist = "";  // Empty if not provided
                    }
                    // Parse emulator fields (for games)
                    // Trim points. Both are optional and default to 0.0.
                    // A malformed value must not take the whole playlist down,
                    // so parse defensively — yaml-cpp throws on a bad cast.
                    if (item["start"]) {
                        try {
                            playlist_item.start = item["start"].as<double>();
                        } catch (const std::exception&) {
                            playlist_item.start = 0.0;
                        }
                    }
                    if (item["end"]) {
                        try {
                            playlist_item.end = item["end"].as<double>();
                        } catch (const std::exception&) {
                            playlist_item.end = 0.0;
                        }
                    }
                    // An end at or before start would stop playback instantly;
                    // treat that as "no trim" rather than an unplayable item.
                    if (playlist_item.end > 0.0 && playlist_item.end <= playlist_item.start) {
                        std::cerr << "Warning: ignoring end <= start for '"
                                  << playlist_item.title << "' in " << path << std::endl;
                        playlist_item.end = 0.0;
                    }
                    if (playlist_item.start < 0.0) playlist_item.start = 0.0;

                    if (item["emulator_core"]) {
                        playlist_item.emulator_core = item["emulator_core"].as<std::string>();
                    } else {
                        playlist_item.emulator_core = "";
                    }
                    if (item["emulator_system"]) {
                        playlist_item.emulator_system = item["emulator_system"].as<std::string>();
                    } else {
                        playlist_item.emulator_system = "";
                    }
                }

                // Validate: emulated_game items must have a core and path
                if (playlist_item.source_type == "emulated_game") {
                    if (playlist_item.emulator_core.empty()) {
                        std::cerr << "Warning: Skipping game '" << playlist_item.title
                                  << "' in " << path << " - missing emulator_core" << std::endl;
                        continue;
                    }
                    if (playlist_item.path.empty()) {
                        std::cerr << "Warning: Skipping game '" << playlist_item.title
                                  << "' in " << path << " - missing path" << std::endl;
                        continue;
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

#ifdef MEDIA_BROWSER_ENABLED
Playlist PlaylistLoader::load_movies_library(const std::string& directory) {
    Playlist pl;
    pl.title = "Movies";
    pl.curator = "";
    pl.path = directory;  // Virtual path — the library root

    // Radarr stores each movie in its own subdirectory (e.g.
    // "The Matrix (1999)/the.matrix.1999.mkv"). If the library root doesn't
    // exist yet (new install before any downloads), silently return an
    // empty playlist — callers are expected to check items.empty().
    std::error_code ec;
    if (!fs::exists(directory, ec) || !fs::is_directory(directory, ec)) {
        return pl;
    }

    // Video extensions we recognize as movie files. Lowercase comparison.
    static const std::vector<std::string> kVideoExts = {
        ".mkv", ".mp4", ".avi", ".m4v"
    };
    auto is_video_ext = [](const std::string& ext) {
        std::string lower = ext;
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) { return std::tolower(c); });
        for (const auto& v : kVideoExts) {
            if (lower == v) return true;
        }
        return false;
    };

    try {
        for (const auto& entry : fs::directory_iterator(directory, ec)) {
            if (ec) break;
            if (!entry.is_directory()) continue;

            // Find the first video file in this movie directory (non-recursive).
            std::string video_path;
            for (const auto& inner : fs::directory_iterator(entry.path(), ec)) {
                if (ec) break;
                if (!inner.is_regular_file()) continue;
                if (is_video_ext(inner.path().extension().string())) {
                    video_path = inner.path().string();
                    break;
                }
            }
            if (video_path.empty()) continue;  // Skip empty / no-video dirs

            PlaylistItem item;
            item.path = video_path;
            item.source_type = "local";  // Matches existing video playback dispatch
            item.title = entry.path().filename().string();  // e.g. "The Matrix (1999)"
            item.artist = "";
            item.emulator_core = "";
            item.emulator_system = "";
            pl.items.push_back(item);
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to scan movies library " << directory
                  << ": " << e.what() << std::endl;
    }

    // Sort alphabetically (case-insensitive) for stable display order.
    std::sort(pl.items.begin(), pl.items.end(),
        [](const PlaylistItem& a, const PlaylistItem& b) {
            std::string al = a.title, bl = b.title;
            std::transform(al.begin(), al.end(), al.begin(),
                [](unsigned char c) { return std::tolower(c); });
            std::transform(bl.begin(), bl.end(), bl.begin(),
                [](unsigned char c) { return std::tolower(c); });
            return al < bl;
        });

    return pl;
}
#endif

} // namespace app

