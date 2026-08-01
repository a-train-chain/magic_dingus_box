#pragma once

#include "media_browser/radarr/radarr_client.h"

namespace media_browser {

// In-memory mock Radarr for UI development and tests without a real
// Radarr instance running. Seeds with a small deterministic dataset.
class RadarrMockClient : public RadarrClient {
public:
    RadarrMockClient();

    bool is_reachable() override;
    std::optional<SystemStatus> get_status() override;
    std::vector<MovieSearchHit> lookup(const std::string& query) override;
    std::vector<Movie> get_library() override;
    std::optional<std::vector<Movie>> get_library_checked() override;
    std::optional<Movie> get_movie(int radarr_id) override;
    bool add_movie(int tmdb_id, int quality_profile_id, bool monitor) override;
    bool remove_movie(int radarr_id, bool delete_files) override;
    bool trigger_search(int radarr_id) override;
    std::vector<QueueItem> get_queue() override;
    bool cancel_queue_item(int queue_id) override;
    ActiveSearches get_active_searches() override { return {}; }
    std::vector<std::string> get_movie_download_hashes(int movie_id) override;
    std::vector<QualityProfile> get_quality_profiles() override;
    std::vector<RootFolder> get_root_folders() override;

private:
    std::vector<Movie> library_;
    std::vector<QueueItem> queue_;
    std::vector<QualityProfile> profiles_;
    int next_id_ = 1;
};

}  // namespace media_browser
