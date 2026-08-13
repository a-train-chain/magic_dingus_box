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
    // Single-episode search: {"name":"EpisodeSearch","episodeIds":[id]}.
    // Quick Start's fast lane — a ~2 GB single-episode release lands in
    // minutes while the season pack fills in behind it. The episode id
    // comes from get_episodes_checked, never guessed.
    virtual bool trigger_episode_search(int episode_id);

    // DELETE /api/v3/series/{id}. Never sets addImportListExclusion — the
    // user is deleting a download, not blacklisting the show.
    virtual bool remove_series(int sonarr_id, bool delete_files = false);

    // Episodes of one library series, via
    // GET /api/v3/episode?seriesId=<id>&includeEpisodeFile=true — the Phase 3
    // episode picker's fetch. Same checked contract as get_library_checked,
    // INCLUDING its accepted misclassification: nullopt ONLY when transport
    // failed (http_get returned ""); any non-empty body goes through the
    // parser, so a malformed/unparseable body reads as engaged-EMPTY, not
    // nullopt — "Sonarr answered garbage" collapses into "no episodes"
    // exactly as it does for the library fetch, and keeping the two checked
    // variants' discipline identical beats a private, subtly different one.
    //
    // Returned sorted by (season_number, episode_number) — next_up assumes
    // it. Season 0 (specials) is NOT filtered here: the client stays
    // policy-free, the UI excludes specials to match merge_season_rows.
    virtual std::optional<std::vector<EpisodeInfo>> get_episodes_checked(
        int sonarr_id);

    // Parser behind get_episodes_checked, public static for tests (house
    // pattern, like the URL builders below). Tolerant: non-array/unparseable
    // bodies yield empty; every field read is guarded, absent → empty/0.
    static std::vector<EpisodeInfo> parse_episode_list(const std::string& json);

    // Queue / downloads. get_queue_checked() is the primary shape: nullopt on
    // HTTP failure vs a possibly-empty vector on success — same relationship
    // as get_library_checked() / get_series_download_hashes_checked(). The
    // bare get_queue() wrapper below exists for callers that genuinely do not
    // care — do NOT use it to decide "no downloads are active", which is
    // exactly the bug the Queue screen's TV section had to be fixed for: a
    // routine Gluetun netns re-link can make a live season pack's fetch fail,
    // and {} is indistinguishable from a genuinely empty queue.
    //
    // Sonarr's queue is per EPISODE: a season pack yields N records sharing a
    // single downloadId. These are returned RAW AND UNGROUPED on purpose —
    // Phase 2c groups by download_id for display, and it needs to see the
    // sibling rows to do that (and to know that cancelling any one of them
    // takes the whole download with it).
    //
    // Pages internally (Config::queue_page_size per request) until the queue is
    // exhausted — per-episode records make >100 genuinely reachable. A
    // transport/HTTP failure on ANY page — the first or a later one — is
    // nullopt: see get_queue_checked's .cpp comment for why a mid-paging
    // failure cannot be treated as a truncated success the way an earlier
    // version of this pager did.
    virtual std::optional<std::vector<SonarrQueueItem>> get_queue_checked();

    // Bare wrapper — nullopt collapses to {}, same relationship as
    // get_library() has to get_library_checked(). Do NOT use this to decide
    // whether a Sonarr outage occurred; see get_queue_checked().
    virtual std::vector<SonarrQueueItem> get_queue();

    // Removes the download from the client. NOTE: this acts on the WHOLE
    // download, not one episode — every sibling queue id 404s afterwards.
    // blocklist=false: a user-initiated cancel must not poison the release
    // for a later retry. Forwards to the 2-arg overload below.
    virtual bool cancel_queue_item(int queue_id);
    // blocklist=true carries the stall-reaper's full semantics: blocklist
    // the release AND skip Sonarr's immediate auto-redownload, via
    // removeFromClient + blocklist + skipRedownload all set true.
    virtual bool cancel_queue_item(int queue_id, bool blocklist);

    // Distinct downloadIds from this series' history, lowercased for direct
    // comparison with QbittorrentClient (which stores hashes lowercase).
    // Feeds the orphan-proof remove: the queue only knows in-progress
    // downloads, so finished-and-seeding torrents would otherwise survive a
    // series deletion.
    //
    // CHECKED shape on purpose, like get_library_checked / find_series_by_tvdb:
    // nullopt means the service did not answer (transport/HTTP failure), an
    // engaged-but-empty vector means Sonarr answered and this series
    // genuinely has no download history. The orphan-proof remove worker
    // branches on exactly that distinction — engaged proceeds, nullopt
    // aborts before anything is deleted — and it must read the answer from
    // THIS single return rather than a follow-up last_error() call: Task 8
    // adds a ~9 s background re-poll that shares this client and its one
    // last_error_ member, so a decision split across two calls could
    // observe an error the POLL thread set (or cleared) in between, not
    // this call's own outcome.
    virtual std::optional<std::vector<std::string>>
    get_series_download_hashes_checked(int sonarr_id);

    // Bare wrapper — nullopt collapses to {}, same relationship as
    // get_library() has to get_library_checked(). Do NOT use this to decide
    // whether a failure occurred; see get_series_download_hashes_checked.
    virtual std::vector<std::string> get_series_download_hashes(int sonarr_id);

    // --- season-delete surface --------------------------------------------
    //
    // GET /api/v3/history/series?seriesId=<id>&seasonNumber=<n>. The
    // seasonNumber param is REQUIRED server-side scoping, not an optional
    // refinement: individual history records carry no season field at all
    // (probe P1), so there is no client-side way to filter a whole-series
    // fetch down to one season. CHECKED shape, same discipline as
    // get_series_download_hashes_checked: nullopt is transport/HTTP
    // failure, an engaged-but-empty SeasonHistory is Sonarr genuinely
    // answering "no history for this season".
    virtual std::optional<SeasonHistory> get_season_history_checked(
        int sonarr_id, int season_number);

    // POST /api/v3/history/failed/{id}, empty body. Marks one history
    // (grab) record as failed, which blocklists its release (probe P2,
    // live-verified).
    //
    // THIS CALL RE-GRABS unless suppression is in force. Hardware, twice
    // (2026-08-13, magicpi5, Sonarr 4.0.19.2979): the mark-failed publishes
    // Sonarr's DownloadFailedEvent, whose redownload handler pushes an
    // EXPLICIT EpisodeSearch by episode id — and an explicit search does
    // not consult monitored flags at all. Measured with the episode, the
    // season AND the series all monitored=false: `EpisodeSearch` appeared
    // in /api/v3/command within 5 s every time. There is NO per-request
    // opt-out: `?skipRedownload=true` binds on the queue DELETE endpoint
    // (a bad value there 400s with a named validation error) but is
    // silently IGNORED here — the action body ran and returned its own
    // 404, proving the parameter was never bound. Callers that must not
    // re-grab have to hold an AutoRedownloadGuard (below) across the call.
    //
    // Sonarr answers this endpoint with HTTP 200 and a
    // genuinely EMPTY body on success, so http_post's body-only return
    // cannot tell that apart from its own "" on transport/HTTP failure.
    // Uses http_post_status (in-band verdict via HTTP status code) instead
    // of reading last_error() — last_error_ is ONE member shared with the
    // ~9s background series re-poll (SeriesDetailScreen runs poll_worker_
    // concurrently with mut_worker_ against the same client instance, and
    // spawn_mutation does not wait on poll_inflight_), so a poll's error
    // landing mid-window could fail a real success, or a poll's entry-clear
    // could make a real failure read as success — the latter would make
    // Task 6 believe a release was blocklisted when it wasn't and proceed
    // to delete files.
    virtual bool mark_history_failed(int history_id);

    // GET /api/v3/episodefile?seriesId=<id>. CHECKED shape, same discipline
    // as get_episodes_checked: nullopt is transport/HTTP failure, an
    // engaged-but-empty vector is "this series has no episode files".
    virtual std::optional<std::vector<EpisodeFileInfo>>
    get_episode_files_checked(int sonarr_id);

    // DELETE /api/v3/episodefile/bulk, body {"episodeFileIds":[...]}.
    // Sonarr 500s on an empty/null id list rather than no-op'ing (probe-
    // verified: task-1-report.md), so an empty `ids` short-circuits to
    // true with NO HTTP call — "nothing to delete" is success, not a
    // request the server would reject.
    virtual bool delete_episode_files(const std::vector<int>& ids);

    // PUT /api/v3/episode/monitor, body {"episodeIds":[...],"monitored":b}.
    // episode.monitored is INDEPENDENT of the season container's monitored
    // flag (probe P3, live-verified), and SeasonSearch skips unmonitored
    // episodes — which is why every flow that monitors a season in order to
    // download it must re-monitor the season's EPISODES first, and why
    // stage (a) of the season delete unmonitors them.
    //
    // CORRECTION 2026-08-13 (hardware): P3 also concluded that
    // autoRedownloadFailed keys off this per-episode flag, so unmonitoring
    // the episodes would stop the auto re-search after mark_history_failed.
    // That half is DISPROVEN — see mark_history_failed's comment above. The
    // redownload is an EXPLICIT EpisodeSearch by id, which bypasses
    // monitoring entirely; suppression needs AutoRedownloadGuard. This call
    // is still required, just not for that reason.
    //
    // Same empty-list short-circuit as
    // delete_episode_files, and for the same reason: nothing to change is
    // success, not a request worth a round-trip.
    //
    // Verdict comes from http_put_status (the HTTP status code), NOT from
    // whether a body came back: this endpoint's success-body shape is
    // UNVERIFIED, and a 2xx with an empty body would read as failure under
    // http_put's body-only return — aborting stage (a) of every season
    // delete and warning on every "Download Season N". Same in-band
    // discipline as mark_history_failed.
    virtual bool set_episodes_monitored(const std::vector<int>& ids,
                                        bool monitored);

    // GET /api/v3/config/downloadclient. CHECKED, and STRICTLY so: nullopt
    // covers transport/HTTP failure AND an unparseable-or-incomplete body,
    // because the only consumer needs a faithful document to restore from
    // and a guess would be worse than an abort (see
    // SonarrParsers::parse_download_client_config).
    virtual std::optional<DownloadClientConfig> get_download_client_config();

    // PUT /api/v3/config/downloadclient/{cfg.id} with `cfg.raw` round-tripped
    // and autoRedownloadFailed overwritten by `enabled` — the whole document,
    // because this PUT REPLACES the resource rather than patching it.
    //
    // Verdict is the HTTP STATUS via http_put_status, and the accept band is
    // any 2xx, NOT 200: the live box answers this endpoint **202 Accepted**
    // (measured 2026-08-13). A `code == 200` check would have failed every
    // real call while passing every test written against a 200-returning
    // fake — so the 202 case is pinned in test_sonarr_client.cpp.
    virtual bool set_auto_redownload_failed(const DownloadClientConfig& cfg,
                                            bool enabled);

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
    // Status-code-returning POST — same reason http_delete_body exists
    // beside http_delete: some endpoints answer success with an empty body,
    // which collides with http_post's own "" return on transport/HTTP
    // failure. mark_history_failed is the current caller (POST
    // /history/failed/{id} answers HTTP 200 with a genuinely empty body).
    // Same contract as http_delete: 0 = transport failure, callers branch
    // on `code > 0 && code < 400`; last_error side effects unchanged.
    virtual long http_post_status(const std::string& path, const std::string& body);
    virtual std::string http_put(const std::string& path, const std::string& body);
    // Status-code-returning PUT — http_post_status' reason, PUT verb.
    // set_episodes_monitored is the caller: PUT /api/v3/episode/monitor's
    // success-body shape is UNVERIFIED, and http_put's body-only return
    // reads a 2xx-with-empty-body as failure — which would abort stage (a)
    // of EVERY season delete and make "Download Season N" warn every time.
    // Same contract as http_post_status: 0 = transport failure, callers
    // branch on `code > 0 && code < 400`; last_error side effects unchanged.
    virtual long http_put_status(const std::string& path, const std::string& body);
    // Returns the HTTP STATUS CODE, not the body — 0 means the request never
    // got an answer (transport failure). DELETE endpoints answer with an empty
    // body on success, so the body cannot distinguish success from failure and
    // the old "did last_error() stay empty?" read is a cross-thread split read
    // now that Task 8's background re-poll shares this client's one
    // last_error_ member. Callers branch on `code > 0 && code < 400`.
    // last_error side effects are unchanged: still set on transport failure
    // and on HTTP >= 400, so the UI keeps its message.
    virtual long http_delete(const std::string& path);
    // Body-carrying DELETE — http_delete above takes no body, and
    // /api/v3/episodefile/bulk requires one (its id list). Same status-code
    // contract as http_delete (0 = transport failure, callers branch on
    // `code > 0 && code < 400`), same curl setup plus
    // CURLOPT_CUSTOMREQUEST "DELETE" + POSTFIELDS.
    virtual long http_delete_body(const std::string& path, const std::string& body);

    Config cfg_;

private:
    mutable std::mutex err_mtx_;
    std::string last_error_;  // guarded by err_mtx_
};

// RAII suppression of Sonarr's auto-redownload-on-failed, for the duration of
// a destructive operation that marks history records failed.
//
// WHY THIS EXISTS. Deleting a season blocklists its release and leaves
// re-download MANUAL — the owner presses a button when they want it back.
// The original design met that contract by unmonitoring the season and its
// episodes (probe P3). Hardware disproved it on 2026-08-13: Sonarr's
// redownload handler pushes an EXPLICIT EpisodeSearch by episode id, and an
// explicit search bypasses monitored=false. Live, the delete completed and
// Sonarr grabbed a replacement 5 s and 4 s later; the user was told to press
// a button to download a season whose row already read `downloading`. It
// converged only after 3 deletes and 8 blocklist rows, once picking a
// 24.65 GB pack for a 7-minute-per-episode show.
//
// WHY A GLOBAL FLAG AND NOT A PARAMETER. The per-request opt-out was probed
// first and does not exist for this endpoint: `skipRedownload` binds on the
// queue DELETE (a non-boolean value 400s with a named validation error) but
// is silently ignored on POST /history/failed/{id} (the action body ran and
// returned its own 404). The only working lever is the GLOBAL config field
// `autoRedownloadFailed`, verified live: with it false, marking a record
// failed produced NO EpisodeSearch in 20 s, with the episode unmonitored and
// again with it monitored — it is the master switch, and it takes effect on
// the very next request (no config cache to wait out).
//
// THE COST, AND THE OBLIGATION. The flag is global to SONARR (a separate
// config from Radarr's — movies are unaffected): while a guard is held, NO
// failed SERIES download gets an automatic retry. That window
// is a few seconds and is the price of the owner's manual-redownload
// contract. Leaving the flag off, however, is a SILENT GLOBAL REGRESSION
// with no UI anywhere in the kiosk to reveal it — so restore is not
// best-effort. It runs on every exit path (explicit call, abort, exception),
// retries, logs at error, and reports failure so the caller can tell the
// owner in words.
class AutoRedownloadGuard {
public:
    // Reads the current config and, if auto-redownload is ON, switches it
    // off. Never throws for a Sonarr failure — check armed().
    explicit AutoRedownloadGuard(SonarrClient& client);
    // Backstop only: calls restore() and swallows everything. A destructor
    // that throws during stack unwinding is std::terminate, and this one
    // runs inside a mutation worker thread.
    ~AutoRedownloadGuard();

    AutoRedownloadGuard(const AutoRedownloadGuard&) = delete;
    AutoRedownloadGuard& operator=(const AutoRedownloadGuard&) = delete;
    AutoRedownloadGuard(AutoRedownloadGuard&&) = delete;
    AutoRedownloadGuard& operator=(AutoRedownloadGuard&&) = delete;

    // TRUE = suppression is in force and it is safe to proceed. FALSE = we
    // could not read the config, or the PUT that disables the flag was
    // refused. A false here MUST abort the caller before anything
    // destructive: proceeding would delete the files AND re-grab, which is
    // exactly the shipped defect.
    //
    // NOTE armed() is also true when the owner already had the flag off. In
    // that case the guard PUTs nothing at all and has nothing to undo —
    // deliberately, so a crash mid-operation cannot leave the box in a state
    // this code invented.
    [[nodiscard]] bool armed() const { return armed_; }

    // Puts the ORIGINAL value back. Idempotent — the destructor calls it
    // too, and a second call is a no-op rather than a second PUT. Call it
    // explicitly when you need the verdict in time to put it in a message;
    // otherwise let the destructor do it.
    bool restore();

    // TRUE only after restore() has run and exhausted its retries. The
    // caller owes the owner a visible warning when this is set: nothing
    // else in the kiosk surfaces the state of this Sonarr flag.
    [[nodiscard]] bool restore_failed() const { return restore_failed_; }

private:
    SonarrClient& client_;
    DownloadClientConfig original_{};
    bool armed_ = false;
    bool changed_ = false;        // we PUT a change, so there is one to undo
    bool restored_ = false;       // restore() already completed
    bool restore_failed_ = false;
};

}  // namespace media_browser
