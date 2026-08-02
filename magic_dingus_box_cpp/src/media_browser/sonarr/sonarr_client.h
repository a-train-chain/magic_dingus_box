#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "media_browser/sonarr/sonarr_types.h"

namespace media_browser {

// Outcome of add_series. `settled` is the part callers must not ignore: see
// SonarrClient::add_series for why a "successful" add can still hand back a
// season list that does not reflect the requested monitoring. [[nodiscard]]
// because silently dropping this return value is exactly how a caller ends
// up trusting an unsettled `series` it never looked at.
struct [[nodiscard]] AddSeriesResult {
    bool ok = false;       // the series is in Sonarr and `series` identifies it
    bool settled = false;  // the outcome we asked for was observed; seasons[] is authoritative
    // When !settled, seasons[] is left EMPTY rather than holding a
    // pending/mid-refresh snapshot that looks plausible but is not — an
    // empty list is an obvious "re-fetch me", a stale one is not.
    Series series;
};

// Settle predicate for add_series' post-POST poll loop, exposed so its exact
// shape can be table-tested directly against hand-built Series/Season
// values rather than only indirectly through fixture-driven HTTP stubs
// (fixture+stub coverage is exactly why the specials-monitored false
// positive this predicate now guards against had no test until it was
// found in review). NOT part of the client's public operational API —
// callers should use add_series(), not this directly; it is only meaningful
// for a record add_series itself just created, never for an existing
// library record (see add_series()'s doc comment for why, and
// record_refreshed() below for the predicate that IS meaningful there).
bool add_settled(const Series& s, bool monitor);

// Settle predicate for add_series' idempotent (already-in-library) path.
// "Does this record match the outcome I just requested?" (add_settled) is
// meaningless for a record Sonarr never applied addOptions to — its
// monitored-season shape can be the user's own permanent choice (every
// season monitored), an earlier set_season_monitored() call mid-way through
// a season pass, or coincidentally the shape a fresh add would produce, and
// add_settled would read the first two as unsettled FOREVER, since the
// shape never changes on its own. The only question that means anything for
// an EXISTING record is whether Sonarr has ever actually refreshed it at
// all — i.e. whether episodes are known (any season shows
// episode_count > 0), independent of what is or is not monitored.
bool record_refreshed(const Series& s);

// HTTP client for Sonarr v4 (which still serves its API under /api/v3 — there
// is no /api/v4 namespace). Mirrors RadarrClient: every public method is
// virtual so SonarrMockClient can replace them wholesale, and the four http_*
// helpers are protected virtuals so unit tests can stub transport without a
// network.
class SonarrClient {
public:
    struct Config {
        std::string base_url = "http://localhost:8989";
        std::string api_key;
        // Kept short (like Radarr's) so a stalled Sonarr cannot freeze the
        // render thread. The kiosk systemd unit has WatchdogSec=10; any
        // synchronous call reachable from render must fit inside it.
        int timeout_secs = 5;
        // Queue page size. Sonarr's PagingResource default is far smaller, and
        // its queue is per-EPISODE, so season packs make >100 records genuinely
        // reachable — get_queue() pages until it has them all. Overridable so
        // tests can force the multi-page path without a 100-record fixture.
        int queue_page_size = 100;
        // Budget for add_series' post-POST settle poll (see add_series).
        // 8s comfortably covers a SkyHook episode fetch; the caller gets a
        // provisional result rather than a hang if it does not.
        int add_settle_timeout_ms = 8000;
        int add_settle_poll_ms = 500;
        // Sonarr's root folder is /data/library/tv inside the container; the
        // host sees /mnt/ssd/library/tv. Both normalized to end in '/' by the
        // constructor so "/data/library/tv2/..." cannot false-match.
        std::string container_library_prefix = "/data/library/tv/";
        std::string host_library_prefix      = "/mnt/ssd/library/tv/";
    };

    explicit SonarrClient(Config config);
    virtual ~SonarrClient();

    SonarrClient(const SonarrClient&) = delete;
    SonarrClient& operator=(const SonarrClient&) = delete;

    // Service health
    virtual bool is_reachable();
    virtual std::optional<SystemStatus> get_status();

    // Series discovery.
    //
    // Resolves a TMDB id through Sonarr's own delegation path
    // (term=tmdb:<id>, which SkyHook maps to TVDB server-side) — live-proven
    // against the box, so the kiosk needs no TVDB mapping table. Some shows
    // have no mapping and come back empty; pass the TMDB title as
    // `title_fallback` to retry as a free-text search.
    virtual std::vector<SeriesSearchHit> lookup_by_tmdb(
        int tmdb_id, const std::string& title_fallback = "");
    virtual std::vector<SeriesSearchHit> lookup(const std::string& query);

    // Library. get_library_checked() is the primary shape: nullopt on HTTP
    // failure vs a possibly-empty vector on success. The bare wrapper exists
    // for callers that genuinely do not care — do NOT use it to decide
    // "library is empty", which is the bug the Radarr equivalent had to fix.
    virtual std::optional<std::vector<Series>> get_library_checked();
    virtual std::vector<Series> get_library();
    virtual std::optional<Series> get_series(int sonarr_id);
    // GET /api/v3/series?tvdbId=<id> — Sonarr filters server-side. Used to
    // detect an already-added series before POSTing (which would 400 on
    // seriesExistsValidator).
    //
    // CHECKED shape on purpose, like get_library_checked: nullopt means the
    // REQUEST FAILED, an engaged-but-empty vector means Sonarr answered "not
    // in the library". Callers must not collapse the two — this probe gates a
    // mutation, and Sonarr shares Gluetun's netns, so a tunnel blip that read
    // as "not present" would POST a duplicate and surface Sonarr's 400
    // validation text instead of the real network fault.
    virtual std::optional<std::vector<Series>> find_series_by_tvdb(int tvdb_id);

    // Library management.
    //
    // Adds a series with addOptions.monitor = "firstSeason" (the spec's
    // season-at-a-time default) when `monitor` is true, or "none" when it is
    // false — NEVER "firstSeason" for an unmonitored add, because Sonarr
    // honours addOptions.monitor independently of series.monitored and would
    // leave a fully monitored season 1 armed underneath.
    //
    // *** Sonarr applies addOptions ASYNCHRONOUSLY. ***
    // The POST returns the STORED resource (RestController.Created serializes
    // GetResourceById), but at that moment AddSeriesService has only inserted
    // the row and published SeriesAddedEvent, which queues a
    // RefreshSeriesCommand. Not until RefreshSeriesService has fetched the
    // episode list from SkyHook does EpisodeMonitoredService apply the monitor
    // enum. Until then the POST response and early GETs show every season
    // monitored:true — so a single immediate re-GET is no better than
    // trusting the POST body.
    //
    // *** The settle signal is the OUTCOME, not an incidental marker. ***
    // A live probe (2026-08-01: added Breaking Bad, polled GET /series/{id}
    // 40x at 0.7s) found addOptions ABSENT from the POST response and every
    // GET — Sonarr's SeriesResource never serializes it back, so "addOptions
    // disappeared" is not an observable signal at all. Worse, the same probe
    // showed statistics.totalEpisodeCount > 0 well before the monitor enum
    // was applied: Sonarr writes episodes and applies monitoring in SEPARATE
    // steps, so "episodes exist" alone is also not sufficient — trusting it
    // would settle mid-refresh and report the whole series monitored, which
    // is the exact failure this method exists to prevent. The only signal
    // that cannot false-positive during that race window is checking what we
    // actually asked for: settled once episodes are known to exist AND the
    // monitored-season set matches the request (exactly one non-special
    // season monitored AND specials NOT monitored, for "firstSeason" — never
    // hardcoded to season number 1, since a show's first aired season is not
    // always numbered 1; none at all, specials included, for "none"). The
    // specials clause matters on its own: a show with exactly ONE regular
    // season plus specials has a mid-refresh all-monitored state of
    // {specials monitored, S1 monitored} where the non-special count is
    // already 1, so a predicate that checked only that count would settle
    // one poll early and report specials monitored when firstSeason will
    // unmonitor them (live-verified in Phase 2a: firstSeason left "S2-5 AND
    // specials" unmonitored; caught in review before shipping).
    //
    // Two shapes accepted as safe-by-design rather than specially handled:
    // a series SkyHook has no episodes for yet (announced/upcoming) can
    // never satisfy the episode-presence clause, and a series whose ONLY
    // season is season 0 (specials-only — rare but real) can never satisfy
    // monitored_non_special == 1. Both burn the full timeout budget on
    // every add — accepted and safe, since the result is a correctly
    // labeled settled=false rather than a wrong answer, not a hang.
    //
    // BOUNDED-POLLS GET /api/v3/series/{id} against that predicate, capped by
    // Config::add_settle_timeout_ms. On timeout it returns ok=true,
    // settled=false with seasons[] cleared — the add really did happen, but
    // the caller MUST re-fetch rather than trust a pending/mid-refresh
    // season list.
    //
    // *** WORKER THREAD ONLY. *** This sleeps between polls. Worst case is
    // NOT just add_settle_timeout_ms: a slow final GET can still be in
    // flight when the deadline is crossed, so the true ceiling is roughly
    // add_settle_timeout_ms + add_settle_poll_ms + cfg_.timeout_secs
    // (~13.5s with the defaults: 8s + 0.5s + 5s). cfg_.timeout_secs is 5 and
    // the kiosk unit's WatchdogSec is 10; calling this from the render
    // thread risks a watchdog kill well before the settle budget is even
    // exhausted.
    //
    // Idempotent: when the tvdbId is already in the library the existing
    // record is returned instead of POSTing (which would 400 on
    // seriesExistsValidator) — settled is computed by record_refreshed(),
    // NOT add_settled() (see record_refreshed()'s doc comment for why an
    // existing record needs a different question), and never assumed true.
    // ok=false on any failure; see last_error().
    [[nodiscard]] virtual AddSeriesResult add_series(
        int tmdb_id,
        int quality_profile_id,
        bool monitor = true,
        const std::string& title_fallback = "");

    // Flips one season's monitored flag. Sonarr's PUT replaces the whole
    // resource, so this GETs the current record, edits one season, and PUTs
    // it back untouched otherwise. false when the series or season is not
    // found, or the PUT failed.
    virtual bool set_season_monitored(int sonarr_id, int season_number, bool monitored);

    // POST /api/v3/command. Command names are the C# class name minus
    // "Command". Always pass a seriesId — MissingEpisodeSearch without one
    // sweeps the entire library.
    virtual bool trigger_season_search(int sonarr_id, int season_number);
    virtual bool trigger_series_search(int sonarr_id);

    // DELETE /api/v3/series/{id}. Never sets addImportListExclusion — the
    // user is deleting a download, not blacklisting the show.
    virtual bool remove_series(int sonarr_id, bool delete_files = false);

    // Queue / downloads.
    //
    // Sonarr's queue is per EPISODE: a season pack yields N records sharing a
    // single downloadId. These are returned RAW AND UNGROUPED on purpose —
    // Phase 2c groups by download_id for display, and it needs to see the
    // sibling rows to do that (and to know that cancelling any one of them
    // takes the whole download with it).
    //
    // Pages internally (Config::queue_page_size per request) until the queue is
    // exhausted — per-episode records make >100 genuinely reachable.
    virtual std::vector<SonarrQueueItem> get_queue();

    // Removes the download from the client. NOTE: this acts on the WHOLE
    // download, not one episode — every sibling queue id 404s afterwards.
    virtual bool cancel_queue_item(int queue_id);

    // Distinct downloadIds from this series' history, lowercased for direct
    // comparison with QbittorrentClient (which stores hashes lowercase).
    // Feeds the orphan-proof remove: the queue only knows in-progress
    // downloads, so finished-and-seeding torrents would otherwise survive a
    // series deletion.
    virtual std::vector<std::string> get_series_download_hashes(int sonarr_id);

    // Profiles / storage. Resolve the quality profile BY NAME at the call
    // site ("Any" on this box, id 1 — the id is not portable).
    virtual std::vector<QualityProfile> get_quality_profiles();
    virtual std::vector<RootFolder> get_root_folders();
    // Quality definitions — the MB/min table behind the TV disk estimate.
    // Empty on any failure; pick_preferred_mb_per_min falls back to 70.
    virtual std::vector<QualityDefinition> get_quality_definitions();

    // Container path -> host path. Unrecognized paths pass through unchanged
    // with a warn-level log.
    std::string resolve_host_path(const std::string& container_path) const;

    // Trailing-slash normalization, exposed so main.cpp can normalize
    // env-supplied overrides at the same boundary the constructor uses.
    static std::string normalize_prefix(std::string s);

    // URL builders, exposed for unit tests.
    static std::string build_lookup_path_tmdb(int tmdb_id);
    static std::string build_lookup_path_term(const std::string& term);

    // Returns a COPY under the error mutex — screens read this on the render
    // thread while worker threads write it (the exact data race the Radarr
    // client had to fix).
    std::string last_error() const {
        std::lock_guard<std::mutex> lk(err_mtx_);
        return last_error_;
    }

protected:
    void set_error(std::string msg) {
        std::lock_guard<std::mutex> lk(err_mtx_);
        last_error_ = std::move(msg);
    }
    // Virtual for stubbing in tests (see test_sonarr_client.cpp).
    virtual std::string http_get(const std::string& path);
    virtual std::string http_post(const std::string& path, const std::string& body);
    virtual std::string http_put(const std::string& path, const std::string& body);
    virtual std::string http_delete(const std::string& path);

    Config cfg_;

private:
    mutable std::mutex err_mtx_;
    std::string last_error_;  // guarded by err_mtx_
};

}  // namespace media_browser
