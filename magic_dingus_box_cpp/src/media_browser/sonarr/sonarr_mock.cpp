#include "media_browser/sonarr/sonarr_mock.h"

#include <algorithm>

namespace media_browser {

SonarrMockClient::SonarrMockClient() : SonarrClient({/* empty config */}) {
    profiles_.push_back({1, "Any", 1, {}});

    Series s;
    s.sonarr_id       = next_id_++;
    s.tvdb_id         = 81189;
    s.tmdb_id         = 1396;
    s.imdb_id         = "tt0903747";
    s.title           = "Breaking Bad";
    s.overview        = "Mock series. Use a real SonarrClient for actual data.";
    s.year            = 2008;
    s.runtime_minutes = 47;
    s.status          = "ended";
    s.monitored       = true;
    s.path            = "/data/library/tv/Breaking Bad";
    s.added_at        = "2026-08-01T09:00:00Z";
    s.episode_file_count = 7;
    s.size_on_disk_bytes = 8589934592LL;
    // Specials + 5 seasons; only season 1 monitored and downloaded — the
    // shape addOptions.monitor="firstSeason" actually persists.
    s.seasons.push_back({0, false, 5, 0, 0});
    s.seasons.push_back({1, true, 7, 7, 8589934592LL});
    s.seasons.push_back({2, false, 13, 0, 0});
    s.seasons.push_back({3, false, 13, 0, 0});
    s.seasons.push_back({4, false, 13, 0, 0});
    s.seasons.push_back({5, false, 16, 0, 0});
    library_.push_back(std::move(s));

    // A season-2 pack in flight: three episode rows sharing one downloadId,
    // the exact shape Phase 2c's grouping must handle. Lowercase hash — the
    // real client lowercases history hashes and QueueScreen lowercases the
    // queue's before comparing, so the mock never hands out a casing the UI
    // would not see.
    const std::string kMockHash = "a1b2c3d4e5f60718293a4b5c6d7e8f9012345678";
    for (int ep = 1; ep <= 3; ++ep) {
        SonarrQueueItem q;
        q.id             = 100 + ep;
        q.series_id      = library_.front().sonarr_id;
        q.episode_id     = 5000 + ep;
        q.season_number  = 2;
        q.title          = "Breaking.Bad.S02.1080p.WEB-DL.x264-MOCK";
        q.size_bytes     = 12'884'901'888LL;
        q.sizeleft_bytes = 6'442'450'944LL;
        q.progress       = 0.5;
        q.eta_seconds    = 4800;
        q.state          = "downloading";
        q.tracked_download_state = "downloading";
        q.download_id    = kMockHash;
        q.episode.id             = q.episode_id;
        q.episode.series_id      = q.series_id;
        q.episode.season_number  = 2;
        q.episode.episode_number = ep;
        q.episode.title          = "Mock Episode " + std::to_string(ep);
        queue_.push_back(std::move(q));
    }
}

bool SonarrMockClient::is_reachable() { return true; }

std::optional<SystemStatus> SonarrMockClient::get_status() {
    SystemStatus st;
    st.version = "mock-4.0.19.2979";
    st.startup_completed = true;
    return st;
}

std::vector<SeriesSearchHit>
SonarrMockClient::lookup_by_tmdb(int tmdb_id, const std::string& title_fallback) {
    SeriesSearchHit h;
    h.tmdb_id = tmdb_id;
    h.tvdb_id = 81189;
    h.title = title_fallback.empty()
                ? ("Mock Series " + std::to_string(tmdb_id))
                : title_fallback;
    h.year = 2008;
    h.runtime_minutes = 47;
    h.status = "ended";
    h.overview = "Mock result. Use a real SonarrClient for actual data.";
    // Lookup results arrive with every season monitored — mirror that so a
    // caller cannot accidentally depend on the mock being tidier than Sonarr.
    for (int n = 0; n <= 5; ++n) h.seasons.push_back({n, true, 0, 0, 0});
    return {h};
}

std::vector<SeriesSearchHit> SonarrMockClient::lookup(const std::string& query) {
    return lookup_by_tmdb(0, query);
}

std::optional<std::vector<Series>> SonarrMockClient::get_library_checked() {
    return library_;
}

std::vector<Series> SonarrMockClient::get_library() { return library_; }

std::optional<Series> SonarrMockClient::get_series(int sonarr_id) {
    for (const auto& s : library_) if (s.sonarr_id == sonarr_id) return s;
    return std::nullopt;
}

std::optional<std::vector<Series>> SonarrMockClient::find_series_by_tvdb(int tvdb_id) {
    // Always "the request worked" — the mock has no transport to fail.
    std::vector<Series> out;
    for (const auto& s : library_) if (s.tvdb_id == tvdb_id) out.push_back(s);
    return out;
}

AddSeriesResult SonarrMockClient::add_series(int tmdb_id,
                                             int /*quality_profile_id*/,
                                             bool monitor,
                                             const std::string& title_fallback) {
    AddSeriesResult r;
    // The mock has no async refresh, so everything it returns is settled.
    r.settled = true;
    for (const auto& s : library_) {
        if (s.tmdb_id == tmdb_id) {  // idempotent
            r.ok = true;
            r.series = s;
            return r;
        }
    }
    Series s;
    s.sonarr_id       = next_id_++;
    s.tmdb_id         = tmdb_id;
    s.tvdb_id         = 100000 + tmdb_id;
    s.title           = title_fallback.empty()
                          ? ("Mock Series " + std::to_string(tmdb_id))
                          : title_fallback;
    s.monitored       = monitor;
    s.runtime_minutes = 45;
    s.path            = "/data/library/tv/" + s.title;
    // Mirror a real firstSeason add: specials off, season 1 on, rest off.
    s.seasons.push_back({0, false, 3, 0, 0});
    s.seasons.push_back({1, monitor, 10, 0, 0});   // "none" when monitor==false
    s.seasons.push_back({2, false, 10, 0, 0});
    library_.push_back(s);
    r.ok = true;
    r.series = s;
    return r;
}

bool SonarrMockClient::set_season_monitored(int sonarr_id, int season_number,
                                            bool monitored) {
    for (auto& s : library_) {
        if (s.sonarr_id != sonarr_id) continue;
        for (auto& season : s.seasons) {
            if (season.season_number == season_number) {
                season.monitored = monitored;
                return true;
            }
        }
        return false;
    }
    return false;
}

bool SonarrMockClient::trigger_season_search(int /*id*/, int /*season*/) { return true; }
bool SonarrMockClient::trigger_series_search(int /*id*/) { return true; }

bool SonarrMockClient::remove_series(int sonarr_id, bool /*delete_files*/) {
    auto it = std::remove_if(library_.begin(), library_.end(),
                             [&](const Series& s) { return s.sonarr_id == sonarr_id; });
    const bool removed = (it != library_.end());
    library_.erase(it, library_.end());
    return removed;
}

std::vector<QualityProfile> SonarrMockClient::get_quality_profiles() {
    return profiles_;
}

std::vector<RootFolder> SonarrMockClient::get_root_folders() {
    RootFolder rf;
    rf.id = 1;
    rf.path = "/data/library/tv";
    rf.free_space_bytes = 500'000'000'000LL;
    return {rf};
}

std::vector<SonarrQueueItem> SonarrMockClient::get_queue() { return queue_; }

bool SonarrMockClient::cancel_queue_item(int queue_id) {
    // Cancelling one row removes the WHOLE download, exactly like the live
    // DELETE — a mock that removed a single episode row would teach the UI
    // the wrong lesson.
    std::string hash;
    for (const auto& q : queue_) if (q.id == queue_id) hash = q.download_id;
    if (hash.empty()) return false;
    auto it = std::remove_if(queue_.begin(), queue_.end(),
                             [&](const SonarrQueueItem& q) {
                                 return q.download_id == hash;
                             });
    queue_.erase(it, queue_.end());
    return true;
}

std::vector<std::string>
SonarrMockClient::get_series_download_hashes(int sonarr_id) {
    std::vector<std::string> out;
    for (const auto& q : queue_) {
        if (q.series_id != sonarr_id || q.download_id.empty()) continue;
        if (std::find(out.begin(), out.end(), q.download_id) == out.end()) {
            out.push_back(q.download_id);
        }
    }
    return out;
}

}  // namespace media_browser
