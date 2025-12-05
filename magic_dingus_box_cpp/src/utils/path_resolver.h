#pragma once

#include <string>

namespace utils {

// Resolve a video file path from a playlist item
// Handles:
// - Absolute paths
// - Relative paths (relative to playlist directory)
// - dev_data/ paths (maps to /data/ on Pi)
// - Current working directory fallback
std::string resolve_video_path(const std::string& item_path, const std::string& playlist_dir = "");

} // namespace utils

