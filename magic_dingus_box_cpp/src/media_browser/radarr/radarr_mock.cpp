#include "media_browser/radarr/radarr_mock.h"

#include <algorithm>

namespace media_browser {

RadarrMockClient::RadarrMockClient() : RadarrClient({/* empty config */}) {
    // Seed profiles
    profiles_.push_back({1, "Any", 1, {}});
    profiles_.push_back({2, "1080p Standard", 7, {}});

    // Seed library with a few legal-film examples
    Movie m;
    m.radarr_id = next_id_++;
    m.tmdb_id = 20529;
    m.title = "Sita Sings the Blues";
    m.year = 2008;
    m.runtime_minutes = 82;
    m.rating = 6.9;
    m.monitored = true;
    m.has_file = true;
    m.file_quality = "Bluray-1080p";
    library_.push_back(m);

    // Seed a simulated queue entry
    QueueItem q;
    q.id = 1;
    q.movie_id = m.radarr_id;
    q.title = "Sita Sings the Blues (2008)";
    q.size_bytes = 1'500'000'000;
    q.sizeleft_bytes = 500'000'000;
    q.progress = 0.67;
    q.download_rate_bps = 1'500'000;
    q.peers = 42;
    q.state = "downloading";
    queue_.push_back(q);
}

bool RadarrMockClient::is_reachable() { return true; }

std::optional<SystemStatus> RadarrMockClient::get_status() {
    SystemStatus s;
    s.version = "mock-5.14.0";
    s.startup_completed = true;
    return s;
}

std::vector<MovieSearchHit> RadarrMockClient::lookup(const std::string& query) {
    // Return one fake hit shaped like the query for demo purposes
    MovieSearchHit h;
    h.tmdb_id = 999;
    h.title = query + " (mock)";
    h.year = 2024;
    h.runtime_minutes = 100;
    h.rating = 7.5;
    h.overview = "Mock result. Use real RadarrClient for actual data.";
    return {h};
}

std::vector<Movie> RadarrMockClient::get_library() { return library_; }

std::optional<std::vector<Movie>> RadarrMockClient::get_library_checked() {
    return library_;
}

std::optional<Movie> RadarrMockClient::get_movie(int radarr_id) {
    for (const auto& m : library_) if (m.radarr_id == radarr_id) return m;
    return std::nullopt;
}

bool RadarrMockClient::add_movie(int tmdb_id, int /*qp*/, bool monitor) {
    Movie m;
    m.radarr_id = next_id_++;
    m.tmdb_id = tmdb_id;
    m.title = "Mock Movie " + std::to_string(tmdb_id);
    m.monitored = monitor;
    library_.push_back(m);
    return true;
}

bool RadarrMockClient::remove_movie(int radarr_id, bool /*del*/) {
    auto it = std::remove_if(library_.begin(), library_.end(),
                             [&](const Movie& m) { return m.radarr_id == radarr_id; });
    bool removed = (it != library_.end());
    library_.erase(it, library_.end());
    return removed;
}

bool RadarrMockClient::trigger_search(int /*id*/) { return true; }
std::vector<QueueItem> RadarrMockClient::get_queue() { return queue_; }
bool RadarrMockClient::cancel_queue_item(int id) {
    auto it = std::remove_if(queue_.begin(), queue_.end(),
                             [&](const QueueItem& q) { return q.id == id; });
    bool removed = (it != queue_.end());
    queue_.erase(it, queue_.end());
    return removed;
}
std::vector<std::string>
RadarrMockClient::get_movie_download_hashes(int /*movie_id*/) {
    // No history in the mock — DetailScreen tests that exercise the
    // remove flow don't need any historical hashes; the live HTTP
    // client's behavior is tested separately via the test_cli harness.
    return {};
}
std::vector<QualityProfile> RadarrMockClient::get_quality_profiles() { return profiles_; }
std::vector<RootFolder> RadarrMockClient::get_root_folders() {
    RootFolder rf; rf.id = 1; rf.path = "/library/Movies"; rf.free_space_bytes = 500'000'000'000;
    return {rf};
}

}  // namespace media_browser
