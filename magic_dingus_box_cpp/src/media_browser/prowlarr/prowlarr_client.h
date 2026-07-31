#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace media_browser {

// Aggregate seeder/release stats for a Prowlarr search query. The kiosk
// only needs roll-up numbers — a full release list would be too dense to
// display on a 1280x720 UI and isn't actionable here (the user can't
// pick a specific release; Radarr does that automatically once the
// movie is added to the library).
struct ReleaseSummary {
    int total_releases = 0;   // Number of distinct releases returned.
    int best_seeders = 0;     // Highest seeder count among results.
    int total_seeders = 0;    // Sum across all results — rough "depth"
                              // signal complementing best_seeders.
};

// Minimal Prowlarr client focused on the kiosk's one need: pre-add
// availability check on the Detail screen. We deliberately don't model
// indexers, categories, or release picking here — Radarr handles all of
// that once the movie is added.
//
// Threading: search_async() runs on a detached background thread so the
// UI thread never blocks on a 3-15s indexer round-trip. Results are
// posted to a slot the caller polls each frame via take_result().
class ProwlarrClient {
public:
    struct Config {
        // Default matches the docker-compose service name port. The
        // kiosk runs on the host where Prowlarr's port is forwarded to
        // localhost:9696.
        std::string base_url = "http://localhost:9696";

        // Loaded from MDB_PROWLARR_API_KEY env var by the caller. Empty
        // = client is disabled (search_async returns immediately with
        // an "unconfigured" result so the UI can show a setup hint
        // instead of a perpetual spinner).
        std::string api_key;

        // Default timeout for UI-thread calls (list_indexers,
        // set_indexer_enabled). Kept short so a stalled Prowlarr
        // doesn't trip systemd's WatchdogSec on Settings -> Sources
        // entry or indexer toggles. The toggle path is also gated on
        // vpn_healthy in mb_settings_screen, so 5s is plenty.
        int timeout_secs = 5;
        // Worker-thread search uses search_timeout_secs (much longer)
        // because Prowlarr fans out to all indexers in parallel and a
        // single Cloudflare-protected one (Byparr solving a challenge)
        // can push the overall response past 10s. 30s leaves room
        // without keeping the spinner up forever if all indexers are
        // genuinely down. search_async runs on a worker thread so the
        // UI never blocks.
        int search_timeout_secs = 30;
    };

    explicit ProwlarrClient(Config cfg);
    ~ProwlarrClient();

    ProwlarrClient(const ProwlarrClient&) = delete;
    ProwlarrClient& operator=(const ProwlarrClient&) = delete;

    // Returns true if an api_key was configured at construction. UI uses
    // this to decide whether to show "Sources: not configured (set
    // MDB_PROWLARR_API_KEY)" vs running an actual search.
    bool is_configured() const { return !cfg_.api_key.empty(); }

    // State of an in-flight or recently-completed search. The UI polls
    // get_state() every render and switches its label accordingly.
    enum class State {
        Idle,        // No search running, no result to display.
        Searching,   // Background thread is querying Prowlarr.
        Ready,       // Result available via take_result() or peek_result().
        Failed,      // Network/parse error. peek_error() carries detail.
    };

    State state() const { return state_.load(); }

    struct ReleaseRecord {
        std::string title;
        std::string indexer;
        std::string guid;
        std::string download_url;     // magnet: or http URL
        std::string protocol;          // "torrent" or "usenet"
        int         seeders   = 0;
        int         leechers  = 0;
        long long   size_bytes = 0;
        long long   age_seconds = 0;   // since publish, if Prowlarr provides
    };

    struct IndexerStats {
        std::string name;
        int   result_count = 0;
        int   results_above_seed_threshold = 0;
        int   last_response_ms = 0;       // 0 if not measured per-indexer
        std::string last_error;            // empty if healthy
    };

    // Lightweight projection of the /api/v1/indexer entity. The real
    // payload is hundreds of fields per indexer (Cardigann definition,
    // capability matrix, per-cap field schemas) — the Settings panel only
    // needs the three cells visible on a row plus the id for PUT routing.
    struct IndexerInfo {
        int         id = 0;
        std::string name;
        bool        enabled = false;
    };

    // Static parsers (separated for unit testing — don't need a live HTTP client).
    static std::vector<ReleaseRecord> parse_search_response(const std::string& json_body);
    static std::vector<IndexerStats>  aggregate_indexer_stats(
        const std::vector<ReleaseRecord>& records, int seed_threshold);
    static std::vector<IndexerInfo>   parse_indexer_list(const std::string& json_body);

    // Live accessors (populated after search_async completes).
    std::vector<ReleaseRecord> get_last_releases() const;
    std::vector<IndexerStats>  get_last_indexer_stats() const;

    // Live GET /api/v1/indexer. Synchronous — called once on Settings
    // entry. Empty vector on failure (caller can fall back to a "not
    // reachable" message; peek_error() has the curl/HTTP detail).
    std::vector<IndexerInfo> list_indexers();

    // Live PUT /api/v1/indexer/<id> with the `enable` field flipped.
    // Returns true on 2xx. Fetches the full entity first because Prowlarr
    // requires the entire object on PUT (not a partial patch) — sending
    // just `{enable: true}` clears every other field on the indexer.
    bool set_indexer_enabled(int id, bool enabled);

    // Kick off a background search. Title is required; year is optional
    // (pass 0 to skip). Cancels any in-flight search via abort flag.
    // Idempotent if called repeatedly with the same title+year while
    // a search is in flight (no-op).
    void search_async(const std::string& title, int year);

    // Returns the result if state == Ready, else nullopt. Non-destructive
    // — the result remains available until the next search_async().
    std::optional<ReleaseSummary> peek_result() const;

    // Latest error string when state == Failed. Copy under err_mtx_ —
    // the render thread reads while search workers write; the old
    // by-reference accessor was a data race on std::string.
    std::string peek_error() const {
        std::lock_guard<std::mutex> lk(err_mtx_);
        return last_error_;
    }

    // Stop any in-flight search and join the worker thread. Called by
    // the destructor; exposed so callers can force-cancel before another
    // operation that depends on Prowlarr being idle.
    void cancel();

protected:
    // Virtual for unit tests. Default implementation does a real curl
    // GET with cfg_.timeout_secs (5s — UI-thread budget). Returns the
    // response body, or empty string on transport failure (with
    // last_error_ populated).
    virtual std::string http_get(const std::string& path);

    // Long-timeout variant for the worker-thread search call. Same
    // implementation as http_get but with an explicit timeout (search
    // uses cfg_.search_timeout_secs = 30s because Prowlarr fans out
    // to all indexers in parallel and a single Cloudflare-protected
    // one can push the overall response past 10s).
    virtual std::string http_get_long(const std::string& path, int timeout_secs);

    // Virtual for unit tests. Default implementation does a real curl
    // PUT with a JSON body and the X-Api-Key header. Returns the
    // response body, or empty string on transport failure (with
    // last_error_ populated).
    virtual std::string http_put(const std::string& path,
                                 const std::string& body);

    Config cfg_;

private:
    // Worker entry — runs on background thread spawned by search_async.
    // gen is captured at spawn time and compared to current_gen_ before
    // publishing results, so an abandoned worker (search_async called
    // again before this one finishes) silently drops its result rather
    // than overwriting the new search's state.
    void run_search(uint64_t gen, std::string title, int year);

    std::atomic<State>    state_{State::Idle};
    std::atomic<bool>     abort_{false};

    // Generation counter. search_async bumps it before spawning a new
    // worker; the worker checks current_gen_ before writing results
    // and aborts if it changed. This decouples "start a new search"
    // from "wait for the old one to finish," so search_async never
    // blocks the UI thread on the worker's join — the previous worker
    // just runs to completion and its result is discarded. The atomic
    // load/store is enough; no mutex needed because the comparison is
    // a single 64-bit read/write.
    std::atomic<uint64_t> current_gen_{0};

    // Result + error are written by the worker, read by the UI thread.
    // Mutex protects the strings; ReleaseSummary fits in a single load
    // but we keep all the assignment + state flip under the same lock
    // so a concurrent peek_result() never reads a half-initialized
    // ReleaseSummary.
    mutable std::mutex result_mtx_;
    std::optional<ReleaseSummary> result_;
    // Error string has its own mutex (set_error never touches
    // result_mtx_, so callers already holding result_mtx_ can safely
    // call set_error with no lock-ordering hazard).
    mutable std::mutex err_mtx_;
    std::string last_error_;  // guarded by err_mtx_ — set_error()/peek_error()

    void set_error(std::string msg) {
        std::lock_guard<std::mutex> lk(err_mtx_);
        last_error_ = std::move(msg);
    }

    // Per-release records + per-indexer stats from the most recent
    // search. Populated alongside `result_` on search completion. Kept
    // under a separate mutex from `result_mtx_` so the larger Sources
    // panel reads don't contend with the lightweight Detail-screen
    // ReleaseSummary peek that runs every render frame.
    mutable std::mutex                    last_results_mu_;
    std::vector<ReleaseRecord>            last_releases_;       // guarded by last_results_mu_
    std::vector<IndexerStats>             last_indexer_stats_;  // guarded by last_results_mu_

    // All worker threads spawned during this client's lifetime, in
    // creation order. The most recent worker writes the result; older
    // workers (those whose generation no longer matches current_gen_)
    // run to completion in the background and silently discard their
    // results. We keep them all in this vector so the destructor can
    // join every one — without this, an exit while a worker was
    // mid-CURL would leave the worker holding references to a freed
    // ProwlarrClient instance, producing an at-exit segfault that
    // masks unrelated bugs in the kiosk's shutdown logs.
    //
    // Cap on size: realistic max is one new thread per ~12s timeout
    // window, so even an actively-browsing user accumulates only a
    // handful before earlier workers complete. cancel() opportunistically
    // joins finished threads to keep this vector small.
    // Search workers with per-worker completion flags. The flag is each
    // worker's LAST act, so a join on a done==true worker is instant —
    // which is what lets search_async reap without ever blocking the
    // render thread (the old force-join of the oldest worker held the
    // UI for up to the full 30s search timeout when Prowlarr hung).
    struct SearchWorker {
        std::shared_ptr<std::atomic<bool>> done;
        std::thread thread;
    };
    std::vector<SearchWorker> workers_;
};

}  // namespace media_browser
