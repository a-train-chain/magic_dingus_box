#include "path_resolver.h"
#include "config.h"

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace utils {

// All videos are now pre-transcoded to the correct format
// This function is kept for backward compatibility but videos are already in correct format
static std::string check_30fps_version(const fs::path& path) {
    // Since all videos are now transcoded, just return empty string
    // The calling code will use the original path
    return "";
}

// Helper to find a file that starts with the given filename (ignoring extension and suffix)
static std::string find_fuzzy_match(const fs::path& dir, const std::string& filename) {
    if (!fs::exists(dir) || !fs::is_directory(dir)) return "";
    
    std::string target_stem = fs::path(filename).stem().string();
    std::string target_ext = fs::path(filename).extension().string();
    
    // std::cout << "Fuzzy searching in " << dir << " for " << filename << " (stem: " << target_stem << ")" << std::endl;
    
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (fs::is_regular_file(entry)) {
            std::string entry_name = entry.path().filename().string();
            
            // Check if entry starts with stem
            // We want to match "Title" with "Title (ID).mp4" or "Title.mp4"
            // But be careful not to match "Title 2" with "Title"
            if (entry_name.find(target_stem) == 0) {
                // Check if it has the same extension (optional, but good practice)
                if (entry.path().extension() == target_ext) {
                    std::cout << "Fuzzy match found: " << filename << " -> " << entry_name << std::endl;
                    return entry.path().string();
                }
            }
        }
    }
    return "";
}

std::string resolve_video_path(const std::string& item_path, const std::string& playlist_dir) {
    if (item_path.empty()) {
        return "";
    }

    fs::path p(item_path);
    std::string path_str = item_path;
    
    // Strategy 1: Check absolute path or relative to app path
    // Handle relative paths that start with dev_data/ or data/
    if (path_str.find("dev_data/") == 0 || path_str.find("data/") == 0) {
        std::string absolute_path = config::get_app_path() + "/" + path_str;
        if (fs::exists(absolute_path)) {
            std::cout << "Resolved path: " << item_path << " -> " << absolute_path << std::endl;
            return absolute_path;
        }

        // Fuzzy match in the target directory
        fs::path abs_p(absolute_path);
        std::string fuzzy = find_fuzzy_match(abs_p.parent_path(), abs_p.filename().string());
        if (!fuzzy.empty()) return fuzzy;

        // Fallback: If dev_data/, try data/
        if (path_str.find("dev_data/") == 0) {
            std::string data_path_str = path_str;
            data_path_str.replace(0, 8, "data"); // replace dev_data with data
            std::string data_absolute_path = config::get_app_path() + "/" + data_path_str;

            if (fs::exists(data_absolute_path)) {
                std::cout << "Resolved path (dev_data -> data): " << item_path << " -> " << data_absolute_path << std::endl;
                return data_absolute_path;
            }

            // Fuzzy match in data/ directory
            fs::path data_p(data_absolute_path);
            fuzzy = find_fuzzy_match(data_p.parent_path(), data_p.filename().string());
            if (!fuzzy.empty()) {
                std::cout << "Resolved path (fuzzy dev_data -> data): " << item_path << " -> " << fuzzy << std::endl;
                return fuzzy;
            }
        }

        // Fallback: try old dev_data mapping for backward compatibility
        if (path_str.find("dev_data/") == 0) {
            std::string old_data_path = config::get_home_path() + "/magic_dingus_box/" + path_str;
            if (fs::exists(old_data_path)) {
                std::cout << "Resolved path (old location): " << item_path << " -> " << old_data_path << std::endl;
                return old_data_path;
            }
        }
    }
    
    // Strategy 2: Check absolute path directly
    if (p.is_absolute()) {
        // Check for 30fps version first
        std::string fps_path = check_30fps_version(p);
        if (!fps_path.empty()) {
            return fps_path;
        }
        
        if (fs::exists(p)) {
            return p.string();
        }
        
        // Fuzzy match
        std::string fuzzy = find_fuzzy_match(p.parent_path(), p.filename().string());
        if (!fuzzy.empty()) return fuzzy;
    }
    
    // Strategy 3: Try relative to playlist directory
    if (!playlist_dir.empty()) {
        fs::path playlist_path(playlist_dir);
        fs::path candidate = playlist_path / p;
        try {
            // Check for 30fps version first
            std::string fps_path = check_30fps_version(candidate);
            if (!fps_path.empty()) {
                return fps_path;
            }
            
            if (fs::exists(candidate)) {
                fs::path normalized = fs::canonical(candidate);
                return normalized.string();
            }
            
            // Fuzzy match
            std::string fuzzy = find_fuzzy_match(candidate.parent_path(), candidate.filename().string());
            if (!fuzzy.empty()) return fuzzy;
            
        } catch (const std::exception& e) {
            // Canonical failed, continue to next option
        }
        
        // Also try sibling media/ directory (for dev_data/media/)
        fs::path media_candidate = playlist_path / ".." / "media" / p.filename();
        try {
            // Check for 30fps version first
            std::string fps_path = check_30fps_version(media_candidate);
            if (!fps_path.empty()) {
                return fps_path;
            }
            
            if (fs::exists(media_candidate)) {
                fs::path normalized = fs::canonical(media_candidate);
                return normalized.string();
            }
            
            // Fuzzy match
            std::string fuzzy = find_fuzzy_match(media_candidate.parent_path(), media_candidate.filename().string());
            if (!fuzzy.empty()) return fuzzy;
            
        } catch (const std::exception& e) {
            // Canonical failed, continue to next option
        }
    }
    
    // Strategy 4: Try relative to current working directory
    fs::path cwd_candidate = fs::current_path() / p;
    try {
        // Check for 30fps version first
        std::string fps_path = check_30fps_version(cwd_candidate);
        if (!fps_path.empty()) {
            return fps_path;
        }
        
        if (fs::exists(cwd_candidate)) {
            fs::path normalized = fs::canonical(cwd_candidate);
            return normalized.string();
        }
        
        // Fuzzy match
        std::string fuzzy = find_fuzzy_match(cwd_candidate.parent_path(), cwd_candidate.filename().string());
        if (!fuzzy.empty()) return fuzzy;
        
    } catch (const std::exception& e) {
        // Canonical failed, continue to next option
    }
    
    // Fallback: return original path (mpv might handle it)
    std::cerr << "Warning: Could not resolve video path: " << item_path << std::endl;
    std::cerr << "  Tried all resolution strategies, file not found" << std::endl;
    return item_path;
}

} // namespace utils

