#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "media_browser/prowlarr/prowlarr_client.h"
#include "media_browser/radarr/radarr_types.h"
#include "media_browser/tmdb_client.h"
#include "media_browser/ui/mb_screen.h"
#include "media_browser/ui/release_picker_screen.h"

namespace media_browser { class RadarrClient; }
namespace media_browser { class TmdbClient; }
namespace media_browser { class ProwlarrClient; }
namespace media_browser { class QbittorrentClient; }
namespace media_browser { class DownloadWatchdog; }

namespace media_browser::ui {

// Task 20: the Detail screen — real implementation.
//
// Layout:
//   - Top 35%: fanart/backdrop (dim + slightly-darkened colored poster-tint
//     placeholder until real image loading lands). Title + year + rating
//     overlaid at the bottom-left with the accent color. Runtime right-aligned.
//     Back hint in the chrome header (e.g. "BTN2 back") anchored top-right.
//   - Middle 40%: overview/synopsis text, wrapped at ~60% of screen width,
//     centered. Truncated to ~6 lines with trailing "...".
//   - Bottom 25%: context-sensitive action button row. Buttons depend on
//     whether the movie is in the library and whether the file is present:
//       - Not in library:              [Add to Library]
//       - In library, no file:         [Search Again] [Remove]
//       - In library, file present:    [Play]         [Remove]
//
// Interaction:
//   - LEFT/RIGHT (or ROTATE):      step between action buttons
//   - SELECT / ROTARY_CLICK:       activate focused button
//   - BTN2 (PLAY_PAUSE):           returns to origin_ (the screen that
//                                  opened this Detail — Browse / Library /
//                                  Search / Queue). BTN4 short-press is a
//                                  no-op in v1.6.x.
//
// Action behavior:
//   - [Add to Library]: picks HD-1080p quality profile (fallback to the
//     first profile) and calls radarr_.add_movie(tmdb_id, qp_id, true). On
//     success refreshes library lookup and re-derives button state so the
//     row flips to Search-Again / Remove immediately.
//   - [Search Again]: radarr_.trigger_search(radarr_id). Brief banner.
//   - [Remove]: two-stage confirmation. First SELECT changes button label
//     to "Confirm Remove" for 2 seconds. Second SELECT within that window
//     calls radarr_.remove_movie(radarr_id, false) and transitions back to
//     Screen::Library. Third-party press, or the 2s expiry, cancels.
//   - [Play]: transitions to Screen::Playback with the resolved host path
//     and title forwarded via get_play_target(); the dispatcher hands those
//     to PlaybackScreen::set_movie before its enter() loads the file.
//
// Error / loading states:
//   - Loading: "Loading..." centered while enter() is in flight.
//   - tmdb_id == 0 (shouldn't happen if the dispatcher handoff is correct):
//     centered "No movie selected" message, no actions.
//   - TMDB fetch failure: centered "Couldn't fetch movie info from TMDB"
//     with a [Retry] button. Radarr being unreachable does NOT enter the
//     error state — DetailScreen still renders TMDB metadata and only
//     surfaces a banner warning that library actions may fail.
class DetailScreen : public MbScreen {
public:
    // prowlarr is optional — pass nullptr when Prowlarr is unconfigured
    // or in unit tests. The screen still renders and library actions
    // still work; only the AVAILABILITY readout is suppressed.
    //
    // qbit is optional — pass nullptr in tests / dev machines without
    // a qBittorrent instance. When provided, the Confirm Remove flow
    // also purges any torrents qBit has for this movie (whether
    // active or finished+seeding) so the disk doesn't accumulate
    // orphan files. Without it, the existing "cancel queue items"
    // path still runs, but won't catch finished torrents.
    DetailScreen(RadarrClient& radarr, TmdbClient& tmdb,
                 ProwlarrClient* prowlarr = nullptr,
                 QbittorrentClient* qbit = nullptr);
    ~DetailScreen();

    // Set the tmdb_id of the movie the detail screen should display. The
    // dispatcher in main.cpp calls this just before transitioning to this
    // screen, using the selected_tmdb_id() of whichever source screen (Browse
    // / Search) produced the selection. No-op if the id matches the current
    // one — avoids clobbering in-flight state on self-transitions.
    void set_tmdb_id(int tmdb_id) {
        if (tmdb_id != tmdb_id_) {
            tmdb_id_ = tmdb_id;
            needs_refresh_ = true;
        }
    }
    int tmdb_id() const { return tmdb_id_; }

    // Remember which screen we came from so BTN2 (back) returns there.
    // Set by the dispatcher in main.cpp on every transition into Detail,
    // using the dispatcher's current_mb_screen at the moment of transition.
    // Library → Detail → BTN2 should return to Library, Search → Detail →
    // BTN2 should return to Search (preserving query + results), etc.
    void set_origin(Screen s) { origin_ = s; }
    Screen origin() const { return origin_; }

    // Carrier struct for the Detail->Playback handoff. Populated by
    // get_play_target() and consumed by main.cpp dispatcher to call
    // PlaybackScreen::set_movie[_meta] before the transition lands.
    struct PlayTarget {
        std::string host_path;  // empty if no playable file
        std::string title;
        // Rich metadata for PlaybackScreen's overlay (similar-films panel).
        // Populated from tmdb_detail_ when available; zero/empty otherwise.
        int tmdb_id = 0;
        int year = 0;
        int runtime_min = 0;
        std::string synopsis;         // overview from TMDB
        std::string genres;           // pre-formatted "Action · Drama · Crime"
        std::string poster_url;       // full URL from TmdbMovieDetail::poster_path
        std::vector<std::string> cast;       // top cast names from TmdbMovieDetail
        std::string director;                // primary director name
    };

    // Returns the host-resolved file path + display title (+ TMDB overlay
    // metadata) for the currently loaded movie, or {empty, empty, 0, ...}
    // if no playable file exists.
    PlayTarget get_play_target() const;

    // Detail->ReleasePicker handoff. The dispatcher in main.cpp registers
    // a callback that fires the picker's load_async() (the worker thread
    // hits Radarr; rows arrive via the picker's update() drain on a
    // future frame). Set by main.cpp once at construction; if unregistered
    // (e.g. unit tests that don't include the picker screen),
    // do_pick_source() returns Screen::Detail without transitioning.
    //
    // Signature was previously (tmdb_id, title, candidates) — Detail
    // synchronously fetched + parsed releases before invoking the
    // callback, blocking the UI thread for 14-25s. The async refactor
    // moved the fetch into the picker itself, so the callback now just
    // forwards keys (tmdb_id for watchdog matching, radarr_movie_id for
    // the Radarr release endpoint, title for the Loading-state header).
    using PickerCallback = std::function<void(
        int tmdb_id,
        int radarr_movie_id,
        std::string title)>;
    void set_picker_callback(PickerCallback cb) {
        picker_callback_ = std::move(cb);
    }

    // Optional download-stall watchdog. Set by main.cpp at startup; if
    // null the screen skips watchdog registration (e.g. unit-test builds
    // that don't construct one). do_add_to_library() registers a watch
    // on success so the watchdog can detect zero-progress / Radarr-failed
    // downloads later.
    void set_watchdog(::media_browser::DownloadWatchdog* w) { watchdog_ = w; }

    // Optional VPN health probe. main.cpp wires this to a lambda reading
    // state.media_browser_vpn_healthy; tests / builds without VPN can
    // leave it null and the Detail screen treats VPN as healthy. When
    // VPN is reported down, render() shows a yellow banner above the
    // action button row so the user understands why a torrent grab will
    // sit at 0% (Radarr will still accept the add but qBit can't reach
    // peers until the tunnel comes back).
    void set_vpn_health_provider(std::function<bool()> fn) {
        vpn_healthy_provider_ = std::move(fn);
    }

    void enter() override;
    Screen handle_input(const std::vector<platform::InputEvent>& events) override;
    void update() override;
    void render(::ui::Renderer& r, int screen_w, int screen_h) override;

private:
    // What the Detail screen is currently showing. Drives which action
    // buttons are rendered and how SELECT resolves.
    enum class Mode {
        Loading,                // Fetching movie + profiles.
        Error,                  // Lookup failed — show Retry.
        NoTmdb,                 // tmdb_id == 0; no movie selected.
        NotInLibrary,           // [Add to Library]
        InLibraryNoFile,        // [Search Again] [Remove]
        InLibraryWithFile,      // [Play] [Remove]
    };

    // Abstract button id — one enum covers all modes. Only a subset is
    // present in the button row at a time, determined by the current Mode.
    enum class Action {
        AddToLibrary,
        SearchAgain,
        Remove,
        ConfirmRemove,   // Transient — Remove's second stage.
        Play,
        Retry,
        MoreInfo,        // Placeholder — future sub-screen with full trivia.
        PickSource,      // Manual release-picker (only when in library).
    };

    struct Button {
        Action action;
        std::string label;
    };

    // Rebuild buttons_ based on mode_ and remove_pending_. Resets focus_ if
    // it falls outside the new range.
    void rebuild_buttons();

    // TMDB-first metadata fetch. Async dispatch: spawns a worker that
    // does the slow TMDB + Radarr HTTP calls off the render thread, then
    // returns immediately. Results are drained by apply_pending_detail()
    // in update() on a future tick. While the worker is in flight, mode_
    // stays Mode::Loading and buttons_ is empty so SELECT no-ops cleanly.
    //
    // The TMDB call alone takes 6+ seconds over the VPN tunnel; before
    // this was async, every poster tap froze the entire kiosk UI for
    // 7-15 seconds. Mirrors BrowseScreen's async pattern (commit 8849b77).
    void fetch();

    // Bundled output of run_fetch(). Each `*_ok` flag distinguishes
    // "fetch failed" from "no result yet" so apply_pending_detail() can
    // decide whether to enter Mode::Error or fall back gracefully on a
    // best-effort field (Radarr unreachable still shows TMDB metadata).
    struct DetailFetchResult {
        std::optional<TmdbMovieDetail> detail;
        std::vector<Movie>             library;
        std::vector<QualityProfile>    profiles;
        // Captured from radarr_.last_error() at the time get_library()
        // returned an empty list. Mirrors the sync path's reachability
        // heuristic (empty + clean error == empty library, not failure).
        std::string                    radarr_library_error;
        bool detail_ok   = false;
        bool library_ok  = false;
        bool profiles_ok = false;
    };

    // Worker entry — runs the synchronous TMDB + Radarr calls off-thread.
    // Captures gen at spawn; if a newer fetch starts before this one
    // returns, the result is silently discarded. `done` is set true as
    // the worker's final act so reap_finished_workers() can join it
    // without blocking.
    void run_fetch(uint64_t gen, int tmdb_id,
                   std::shared_ptr<std::atomic<bool>> done);

    // Drain a completed worker's result into live state (movie_,
    // tmdb_detail_, profiles_, mode_, buttons_) and kick off the
    // Prowlarr availability search. Cheap atomic load most frames; only
    // takes the lock when a result is ready to consume.
    void apply_pending_detail();

    // --- Quiet library re-poll (download→import→ready liveness) ----------
    // While a movie is in-library-but-no-file (Mode::InLibraryNoFile), the
    // user may be watching Detail as its download finishes + imports. The
    // full fetch() path can't be reused for this — it sets Mode::Loading
    // and re-hits TMDB (~6s), blanking the screen. Instead a SEPARATE
    // lightweight Radarr-only poll runs on a ~9s cadence: it re-reads the
    // movie's has_file (and the queue's tracked_download_state for the
    // "importing" sub-state) WITHOUT ever touching mode_=Loading or
    // resetting tmdb_detail_. When has_file flips true it swaps in the
    // fresh Movie record (crucially, its now-populated file_container_path)
    // and transitions to Mode::InLibraryWithFile so Play appears live.
    void maybe_repoll_library();                     // called each frame from update()
    void run_library_poll(uint64_t gen, int radarr_id);  // worker body
    void apply_library_poll();                       // drain on render thread

    // True readiness predicate — the single source of truth for whether
    // "Play" should be offered AND will succeed. hasFile alone is NOT
    // enough: the file must map to a resolvable host path that actually
    // exists on the mounted SSD. rebuild_buttons() and do_play() both
    // consult this so the button is never offered when it would fail.
    bool play_ready() const;

    // Helpers that run on SELECT. Each returns the next Screen (often the
    // current one = Screen::Detail).
    Screen on_activate();
    Screen do_add_to_library();
    Screen do_search_again();
    Screen do_remove_stage1();
    Screen do_remove_confirm();
    // Async Confirm-Remove. The 4-step cleanup chains 3+ sequential HTTP
    // calls; on the render thread a hung Radarr blew past WatchdogSec=10
    // and systemd killed the kiosk. One worker at a time (do_remove_confirm
    // gates on remove_in_flight_); the outcome fields are written by the
    // worker BEFORE the release store to remove_done_, and read on the
    // render thread only after the acquire exchange in
    // drain_remove_result() — that pair is the ordering.
    Screen drain_remove_result();          // render thread, every frame
    void run_remove(int radarr_id);        // worker body
    std::thread remove_worker_;
    std::atomic<bool> remove_in_flight_{false};
    std::atomic<bool> remove_done_{false};
    bool remove_ok_ = false;
    std::string remove_error_;
    int remove_radarr_id_ = -1;            // render-thread only

    // Async Add-to-Library — same shape as the remove pipeline above.
    // Only radarr_.add_movie() runs on the worker (the profile pick and
    // disk preflight are local); the toast, watchdog registration,
    // fetch() refresh, and Queue navigation happen in the drain on the
    // render thread, gated on the tmdb_id still being the one on screen.
    Screen drain_add_result();
    std::thread add_worker_;
    std::atomic<bool> add_in_flight_{false};
    std::atomic<bool> add_done_{false};
    bool add_ok_ = false;
    int add_tmdb_id_ = -1;                 // render-thread only
    Screen do_play();
    Screen do_retry();
    Screen do_more_info();
    Screen do_pick_source();

    // Brief toast messages (e.g. "Search triggered", "Added to library").
    void show_banner(std::string text);

    // Pick the HD-1080p quality profile id. Falls back to the first profile
    // if none matches, or 0 if no profiles are available.
    int pick_quality_profile_id() const;

    RadarrClient& radarr_;
    TmdbClient& tmdb_;
    ProwlarrClient* prowlarr_ = nullptr;
    QbittorrentClient* qbit_ = nullptr;
    int tmdb_id_ = 0;
    bool needs_refresh_ = false;

    // Screen to return to on BTN2 (back). Default Browse preserves
    // legacy behavior for any callers that forget to set_origin().
    Screen origin_ = Screen::Browse;

    Mode mode_ = Mode::Loading;
    // TMDB-sourced metadata — populated by tmdb_.get_movie() and used as the
    // primary source for title / year / overview / artwork. This was Radarr
    // SkyHook before; switched to TMDB direct because SkyHook (api.radarr.video)
    // has flaky transient 503s that bricked Detail even when Radarr itself
    // was healthy.
    std::optional<TmdbMovieDetail> tmdb_detail_;
    // If the movie is in the library we also have the full Movie record
    // (with radarr_id, has_file, etc) — used for library-mutating actions
    // (Play / Search Again / Remove).
    std::optional<Movie> movie_;
    std::vector<QualityProfile> profiles_;

    // True while a download for THIS movie has finished in qBit and Radarr
    // is copying/importing it into the library (tracked_download_state ==
    // importing/importPending). Drives the "Downloaded — importing" banner
    // variant on the InLibraryNoFile screen so the user sees the file is
    // moments away, not stuck "monitored, awaiting release". Reset on every
    // state exit (new movie, remove, has_file flip).
    bool import_in_progress_ = false;

    std::vector<Button> buttons_;
    int focus_ = 0;

    // Remove-button confirmation state. remove_pending_ is true between the
    // first SELECT on [Remove] and either the second SELECT (confirmation)
    // or the 2s expiry.
    bool remove_pending_ = false;
    std::chrono::steady_clock::time_point remove_pending_at_{};
    static constexpr int kRemovePendingMs = 2000;

    // Transient status banner (e.g. "Search triggered"). Cleared after a
    // short delay.
    std::string banner_;
    std::chrono::steady_clock::time_point banner_at_{};
    static constexpr int kBannerMs = 2000;

    // --- Async TMDB + Radarr fetch state ---------------------------------
    // The TMDB get_movie() call takes 6+ seconds when egressing through
    // the VPN, plus another ~1s for radarr_.get_library() and (first
    // time) ~1s for get_quality_profiles(). Run synchronously this
    // froze the entire kiosk UI for 7-15s every time the user tapped a
    // poster. Worker thread does the HTTP off the render thread; render()
    // shows Mode::Loading until apply_pending_detail() drains the result
    // on a future update() tick.
    //
    // Generation counter pattern (same as BrowseScreen / ProwlarrClient):
    // each call to fetch() bumps tmdb_current_gen_; the worker captures
    // gen at spawn time and only publishes its result if tmdb_current_gen_
    // still matches. This lets a rapid screen-swap or retry pre-empt a
    // stale worker without blocking the UI on join. Older workers run to
    // completion in the background and silently drop their results.
    std::atomic<uint64_t> tmdb_current_gen_{0};
    std::mutex            tmdb_result_mtx_;
    DetailFetchResult     tmdb_pending_;
    std::atomic<bool>     tmdb_result_ready_{false};
    // TMDB fetch worker threads. On a long-lived kiosk (up/days) every
    // navigation/retry/add spawns one; left unreaped they accumulate OS
    // thread handles until the process hits its thread ceiling and
    // std::thread's ctor throws (uncaught → terminate). Each worker
    // carries its OWN done-flag (set as the worker's final act) so the
    // reaper can join ONLY threads that have actually finished — a
    // non-blocking sweep that never stalls the render thread on a
    // still-running (e.g. mid-CURL, possibly-stale) worker. Order of
    // completion isn't push order (a later fetch can finish before an
    // earlier stalled one), so we test each entry's flag individually
    // rather than assuming front-is-oldest-is-done. The destructor joins
    // whatever remains.
    struct FetchWorker {
        std::thread                    thread;
        std::shared_ptr<std::atomic<bool>> done;  // shared with the worker fn
    };
    std::vector<FetchWorker> tmdb_workers_;
    void reap_finished_workers();

    // --- Quiet library-poll async state (separate from the TMDB fetch) ---
    // Deliberately a DISTINCT mutex + ready-flag + generation from the
    // tmdb_* set so a poll publish can never race/clobber the main fetch
    // drain. The poll uses its own single reused thread handle
    // (lib_poll_worker_, below), NOT tmdb_workers_.
    struct LibraryPollResult {
        std::optional<Movie> movie;      // fresh record (has_file + path)
        bool import_active = false;      // tracked_download_state importing
        bool ok = false;                 // Radarr reachable this poll
    };
    std::atomic<uint64_t> lib_poll_gen_{0};
    std::mutex            lib_poll_mtx_;
    LibraryPollResult     lib_poll_pending_;
    std::atomic<bool>     lib_poll_ready_{false};
    std::atomic<bool>     lib_poll_inflight_{false};
    std::chrono::steady_clock::time_point last_library_poll_at_{};
    static constexpr int  kLibraryPollMs = 9000;  // ~9s cadence while awaiting file
    // SINGLE reused thread handle for the poll — NOT tmdb_workers_. A
    // download can run for tens of minutes, firing a poll every 9s; pushing
    // each into tmdb_workers_ (only cleared at destruction) would accumulate
    // hundreds of joinable thread objects. Instead we reuse one handle,
    // joining the prior (already-finished, inflight guarded) worker before
    // reassigning. Joined once more in the destructor.
    std::thread           lib_poll_worker_;

    // Callback set by main.cpp to forward (movie title + candidate rows)
    // into ReleasePickerScreen::set_candidates() before the dispatcher
    // transitions us to Screen::ReleasePicker. Unset in unit-test builds
    // that don't link the picker screen — do_pick_source() degrades to a
    // no-op + toast in that case.
    PickerCallback picker_callback_;

    // See set_watchdog(). Optional; null when not wired (tests, headless).
    ::media_browser::DownloadWatchdog* watchdog_ = nullptr;

    // See set_vpn_health_provider(). Default null = treat VPN as healthy
    // (so unit tests don't have to wire the AppState atomic).
    std::function<bool()> vpn_healthy_provider_;
};

}  // namespace media_browser::ui
