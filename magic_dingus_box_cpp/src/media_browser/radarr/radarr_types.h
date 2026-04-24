#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace media_browser {

struct MovieSearchHit {
    int tmdb_id = 0;
    std::string title;
    std::string original_title;
    std::string overview;
    std::string poster_url;
    std::string fanart_url;
    int year = 0;
    int runtime_minutes = 0;
    double rating = 0.0;
    std::string imdb_id;
};

// Full movie record (includes fields set only for movies in library)
struct Movie : MovieSearchHit {
    int radarr_id = 0;
    bool monitored = false;
    bool has_file = false;
    std::string file_path;       // relative to root folder
    std::string file_quality;    // e.g. "Bluray-1080p"
    int64_t file_size_bytes = 0;
    std::string added_at;        // ISO 8601
};

struct QueueItem {
    int id = 0;                  // queue row id (used for delete)
    int movie_id = 0;            // Radarr movie id
    std::string title;
    std::string poster_url;
    double progress = 0.0;       // 0.0 - 1.0
    int download_rate_bps = 0;
    int upload_rate_bps = 0;
    int peers = 0;
    int seeds = 0;
    int64_t size_bytes = 0;
    int64_t sizeleft_bytes = 0;
    std::string state;           // "queued", "downloading", "completed", "failed"
    int eta_seconds = 0;
};

struct QualityProfile {
    int id = 0;
    std::string name;
    int cutoff_quality_id = 0;
    std::vector<int> allowed_qualities;
};

struct RootFolder {
    int id = 0;
    std::string path;
    int64_t free_space_bytes = 0;
    int64_t total_space_bytes = 0;
};

struct IndexerInfo {
    int id = 0;
    std::string name;
    std::string protocol;        // "torrent", "usenet"
    bool enabled = false;
    int priority = 50;
};

struct SystemStatus {
    std::string version;
    std::string build_time;
    bool startup_completed = false;
};

}  // namespace media_browser
