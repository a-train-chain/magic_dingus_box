#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <vector>
#include <optional>
#include <json/json.h>
#include "media_browser/radarr/radarr_types.h"

namespace media_browser {

class RadarrClient {
public:
    struct Config {
        std::string base_url = "http://localhost:7878";
        std::string api_key;
        // HTTP timeout for all Radarr requests. Kept short so a stalled
        // Radarr instance doesn't freeze the kiosk main render thread while
        // get_queue() / is_reachable() are in-flight.
        int timeout_secs = 5;
        // Movie adds begin with a safe metadata GET through Gluetun. Let that
        // GET span one normal healthcheck/reconnect cycle without retrying the
        // POST that mutates the Radarr library. Tests set the delay to zero.
        int metadata_lookup_retry_window_ms = 45000;
        int metadata_lookup_retry_delay_ms = 1000;
        // Path translation for movie files: Radarr returns container-internal
        // paths and the kiosk runs on the host. The docker-compose repoints
        // Radarr's root folder to /data/library (with /downloads hardlink
        // support), so movieFile.path is "/data/library/...". Default the
        // container prefix to match; resolve_host_path also falls back to
        // the legacy "/library/" mount (the compose maps both to the same
        // host dir). Both prefixes are normalized to end in '/' by the
        // constructor (defense against /library2 false-matches).
        std::string container_library_prefix = "/data/library/";
        std::string host_library_prefix      = "/mnt/ssd/library/";
    };

    explicit RadarrClient(Config config);
    virtual ~RadarrClient();

    RadarrClient(const RadarrClient&) = delete;
    RadarrClient& operator=(const RadarrClient&) = delete;

    // Service health
    virtual bool is_reachable();
    virtual std::optional<SystemStatus> get_status();

    // Movie discovery
    virtual std::vector<MovieSearchHit> lookup(const std::string& query);
    virtual std::vector<Movie> get_library();
    // Library fetch with an explicit success signal (spec 1c): nullopt on
    // HTTP failure, a possibly-empty vector on success. get_library() keeps
    // its old bare-vector shape as a wrapper — an empty library and a failed
    // fetch were previously indistinguishable, which would have made For You
    // show "add movies to your library" on a full box whenever the GET
    // failed after a successful ping.
    virtual std::optional<std::vector<Movie>> get_library_checked();
    virtual std::optional<Movie> get_movie(int radarr_id);

    // Library management
    virtual bool add_movie(int tmdb_id, int quality_profile_id, bool monitor = true);
    virtual bool remove_movie(int radarr_id, bool delete_files = false);
    virtual bool trigger_search(int radarr_id);

    // Queue / downloads. The checked shape distinguishes a successful empty
    // queue from an HTTP/transport failure; reachability-sensitive callers
    // must use it instead of consulting the shared last_error() afterward.
    virtual std::optional<std::vector<QueueItem>> get_queue_checked();
    virtual std::vector<QueueItem> get_queue();
    virtual bool cancel_queue_item(int queue_id);

    // Which movies Radarr is actively searching indexers for right now
    // (running MoviesSearch / MissingMoviesSearch commands). Powers the
    // "Searching indexers now…" state in the awaiting-release list.
    virtual ActiveSearches get_active_searches();

    // Grab a specific release picked by the user. The `release` JSON
    // must be an object previously returned from /api/v3/release?movieId=X
    // (or constructed with the same shape — at minimum guid + indexerId).
    virtual bool grab_release(const Json::Value& release);

    // Fetch /api/v3/release?movieId=X — the list of releases Radarr would
    // consider for an interactive search of this movie. Each entry has
    // guid, indexerId, title, seeders, size, quality, customFormatScore, etc.
    virtual std::vector<Json::Value> get_releases_for_movie(int radarr_movie_id);

    // Returns the distinct downloadId values (qBit info_hash) from
    // every "grabbed" event in this movie's Radarr history. Used by
    // DetailScreen::do_remove_confirm() to find torrents that may
    // still be seeding in qBittorrent for a movie the user is
    // deleting — the active-queue cancel path only catches downloads
    // currently in progress; finished+seeding torrents don't appear
    // there. Empty vector on error or when the movie has no history.
    // Hashes are returned in lowercase for direct comparison with
    // QbittorrentClient (which normalizes to lowercase internally).
    virtual std::vector<std::string> get_movie_download_hashes(int movie_id);

    struct HistoryEvent {
        int         id = 0;
        int         movie_id = 0;
        std::string event_type;       // grabbed, downloadFailed, downloadFolderImported, etc.
        std::string source_title;     // release title
        std::string date_iso;         // ISO 8601
    };

    // Recent history events for one movie. Returns most-recent-first.
    virtual std::vector<HistoryEvent> get_history(int radarr_movie_id, int page_size = 20);

    // Profiles
    virtual std::vector<QualityProfile> get_quality_profiles();
    virtual std::vector<RootFolder> get_root_folders();

    // Path translation for the movie file Radarr reports. Returns the host
    // path that GStreamer can open, given a container-internal path.
    // Unrecognized paths pass through unchanged with a warn-level log.
    std::string resolve_host_path(const std::string& container_path) const;

    // Trailing-slash normalization for path prefixes — exposed so main.cpp
    // can normalize env-var-supplied overrides at the same boundary the
    // constructor uses internally. Empty input passes through unchanged
    // (treated as "use default"); otherwise we ensure exactly one '/' at
    // the end. Defense against /library2/foo falsely matching /library.
    static std::string normalize_prefix(std::string s);

    // Diagnostics. Returns a COPY under the error mutex: screens read
    // this on the render thread while their workers run client calls
    // that write it — the old by-reference accessor was a data race on
    // std::string (torn reads, use-after-free of the old buffer).
    std::string last_error() const {
        std::lock_guard<std::mutex> lk(err_mtx_);
        return last_error_;
    }

protected:
    struct HttpGetResult {
        std::string body;
        std::string error;
    };

    void set_error(std::string msg) {
        std::lock_guard<std::mutex> lk(err_mtx_);
        last_error_ = std::move(msg);
    }
    // Virtual for mocking (see radarr_mock.h)
    virtual std::string http_get(const std::string& path);
    // Body and failure cause travel together so a concurrent Radarr call
    // cannot overwrite the cause between the request and its caller's read.
    virtual HttpGetResult http_get_result(const std::string& path,
                                          int timeout_secs);
    virtual std::string http_post(const std::string& path, const std::string& body);
    // Returns the HTTP status code; 0 is the ONE reserved "no answer" value
    // (transport failure — no status line). Callers branch on the code
    // IN-BAND (code > 0 && code < 400) instead of reading last_error()
    // afterwards: that read is a cross-thread split on the shared error
    // string once any background poll runs Radarr HTTP concurrently
    // (DetailScreen's library poll does). last_error side effects are
    // unchanged — the message is still set for diagnostics. Ported from
    // SonarrClient's identical fix.
    virtual long http_delete(const std::string& path);

    // Long-timeout GET for endpoints that do synchronous work upstream
    // (notably /api/v3/release?movieId=X, which kicks off an interactive
    // search across every Prowlarr-synced indexer and can take 10-30s).
    // Other endpoints use http_get() with the default cfg_.timeout_secs
    // (5s) so a stalled Radarr doesn't freeze the UI on routine calls.
    virtual std::string http_get_long(const std::string& path, int timeout_secs);

    // Time boundary for the safe metadata-GET retry loop. Virtualizing the
    // clock and wait keeps the production deadline deterministic in tests.
    virtual std::chrono::steady_clock::time_point metadata_retry_now() const;
    virtual void wait_for_metadata_retry(std::chrono::milliseconds delay);

    Config cfg_;

private:
    mutable std::mutex err_mtx_;
    std::string last_error_;  // guarded by err_mtx_ — set_error()/last_error()
};

}  // namespace media_browser
