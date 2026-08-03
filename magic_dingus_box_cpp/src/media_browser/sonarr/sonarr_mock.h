#pragma once

#include "media_browser/sonarr/sonarr_client.h"

namespace media_browser {

// In-memory Sonarr for dev machines with no API key and for tests. Seeded
// with one series in the exact post-firstSeason-add state (S1 monitored,
// specials and later seasons not) so mock-mode UI exercises the real state
// machine rather than an idealized one.
//
// Unlike RadarrMockClient — which leaves get_history/grab_release falling
// through to the real HTTP path against an empty config — this mock overrides
// EVERY public virtual. A dev machine must never emit a live request.
class SonarrMockClient : public SonarrClient {
public:
    SonarrMockClient();

    bool is_reachable() override;
    std::optional<SystemStatus> get_status() override;
    std::vector<SeriesSearchHit> lookup_by_tmdb(
        int tmdb_id, const std::string& title_fallback = "") override;
    std::vector<SeriesSearchHit> lookup(const std::string& query) override;
    // Always returns nullopt — see the .cpp for why the checked shape must
    // report "unreachable" even though this mock has a seeded library.
    std::optional<std::vector<Series>> get_library_checked() override;
    std::vector<Series> get_library() override;
    std::optional<Series> get_series(int sonarr_id) override;
    std::optional<std::vector<Series>> find_series_by_tvdb(int tvdb_id) override;
    // Defaults MUST mirror SonarrClient's — dropping them here compiles fine
    // through a SonarrClient& but breaks mock.add_series(id, profile) (and
    // friends) called through a SonarrMockClient&, since default arguments
    // are resolved at the STATIC type, not virtually.
    AddSeriesResult add_series(int tmdb_id, int quality_profile_id,
                               bool monitor = true,
                               const std::string& title_fallback = "") override;
    bool set_season_monitored(int sonarr_id, int season_number, bool monitored) override;
    bool trigger_season_search(int sonarr_id, int season_number) override;
    bool trigger_series_search(int sonarr_id) override;
    // false, unlike the two triggers above: this mock stands in for a box
    // with NO Sonarr configured (main.cpp's keyless fallback), and Quick
    // Start treats the return as "did a real search start?" — a keyless box
    // must answer no, not pretend. Callers are best-effort by contract, so
    // false costs one info log and nothing else.
    bool trigger_episode_search(int episode_id) override;
    bool remove_series(int sonarr_id, bool delete_files = false) override;
    // Always nullopt, same honesty rule as get_library_checked above: the
    // checked contract is reachability, this mock stands in for a box with NO
    // Sonarr configured (main.cpp's keyless fallback), and keyless boxes must
    // not fabricate data — an engaged episode list here would hand
    // SeriesDetail a fake picker on a box whose owner has no Sonarr at all.
    std::optional<std::vector<EpisodeInfo>> get_episodes_checked(int) override {
        return std::nullopt;
    }
    std::vector<QualityProfile> get_quality_profiles() override;
    std::vector<RootFolder> get_root_folders() override;
    std::vector<QualityDefinition> get_quality_definitions() override;
    // Engaged copy of get_queue()'s value — same relationship as
    // get_series_download_hashes_checked's mock override just below (this
    // mock has no reachability lie to defend against, unlike
    // get_library_checked). Contract completeness only: main.cpp gates the
    // pointer QueueScreen holds on sonarr_key.empty(), so a SonarrMockClient
    // is never actually reachable from that screen.
    std::optional<std::vector<SonarrQueueItem>> get_queue_checked() override;
    std::vector<SonarrQueueItem> get_queue() override;
    bool cancel_queue_item(int queue_id) override;
    // Engaged, not nullopt: unlike get_library_checked, this mock has no
    // specific reason to answer "unreachable" here, and it delegates to the
    // same fixture-consistent computation as the raw variant just below.
    std::optional<std::vector<std::string>>
    get_series_download_hashes_checked(int sonarr_id) override;
    std::vector<std::string> get_series_download_hashes(int sonarr_id) override;

protected:
    std::vector<Series> library_;
    std::vector<QualityProfile> profiles_;
    std::vector<SonarrQueueItem> queue_;
    int next_id_ = 1;
};

}  // namespace media_browser
