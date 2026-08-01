#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <json/json.h>
#include "media_browser/sonarr/sonarr_types.h"

namespace media_browser {

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

    // Profiles / storage. Resolve the quality profile BY NAME at the call
    // site ("Any" on this box, id 1 — the id is not portable).
    virtual std::vector<QualityProfile> get_quality_profiles();
    virtual std::vector<RootFolder> get_root_folders();

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
