#pragma once

#include <filesystem>
#include <string>

namespace utils {

// The fuzzy-match rule behind resolve_video_path's fallback: does this
// directory entry count as "the same title" as the missing target file?
//
// A bare prefix test is NOT enough — it silently resolved "Title.mp4" to
// "Title 2.mp4" (or "Titles.mp4") when the real file was missing, and
// WHICH wrong sibling won depended on unspecified directory-iteration
// order. The stem must be followed by a recognized suffix boundary:
//   - nothing            ("Title.mp4" — exact stem)
//   - " ("               ("Title (1993).mp4" — the ID/year pattern)
//   - " ["               ("Title [VLX2_eOKev4].mp4" — yt-dlp source IDs,
//                         which is what the shipped media actually uses)
//   - "."                ("Title.30fps.mp4" — encode-variant suffixes)
// Extension must match exactly. Header-inline so the tests in
// tests/utils/ exercise the same code find_fuzzy_match runs.
inline bool fuzzy_name_matches(const std::string& entry_name,
                               const std::string& target_stem,
                               const std::string& target_ext) {
    std::filesystem::path entry(entry_name);
    if (entry.extension().string() != target_ext) return false;

    const std::string entry_stem = entry.stem().string();
    if (entry_stem.size() < target_stem.size()) return false;
    if (entry_stem.compare(0, target_stem.size(), target_stem) != 0) return false;
    if (entry_stem.size() == target_stem.size()) return true;  // exact stem

    const std::string rest = entry_stem.substr(target_stem.size());
    return rest.rfind(" (", 0) == 0 ||
           rest.rfind(" [", 0) == 0 ||
           rest[0] == '.';
}

// Resolve a video file path from a playlist item
// Handles:
// - Absolute paths
// - Relative paths (relative to playlist directory)
// - dev_data/ paths (maps to /data/ on Pi)
// - Current working directory fallback
std::string resolve_video_path(const std::string& item_path, const std::string& playlist_dir = "");

} // namespace utils

