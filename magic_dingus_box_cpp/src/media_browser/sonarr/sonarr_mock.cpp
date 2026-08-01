#include "media_browser/sonarr/sonarr_mock.h"

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

}  // namespace media_browser
