#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "media_browser/prowlarr/prowlarr_client.h"
#include "media_browser/radarr/radarr_types.h"
#include "media_browser/tmdb_client.h"
#include "media_browser/ui/mb_screen.h"

namespace media_browser { class RadarrClient; }
namespace media_browser { class TmdbClient; }
namespace media_browser { class ProwlarrClient; }
namespace media_browser { class QbittorrentClient; }

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
    // PlaybackScreen::set_movie before the transition lands.
    struct PlayTarget {
        std::string host_path;  // empty if no playable file
        std::string title;
    };

    // Returns the host-resolved file path + display title for the currently
    // loaded movie, or {empty, empty} if no playable file exists.
    PlayTarget get_play_target() const;

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
    // returns, the result is silently discarded.
    void run_fetch(uint64_t gen, int tmdb_id);

    // Drain a completed worker's result into live state (movie_,
    // tmdb_detail_, profiles_, mode_, buttons_) and kick off the
    // Prowlarr availability search. Cheap atomic load most frames; only
    // takes the lock when a result is ready to consume.
    void apply_pending_detail();

    // Helpers that run on SELECT. Each returns the next Screen (often the
    // current one = Screen::Detail).
    Screen on_activate();
    Screen do_add_to_library();
    Screen do_search_again();
    Screen do_remove_stage1();
    Screen do_remove_confirm();
    Screen do_play();
    Screen do_retry();
    Screen do_more_info();

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
    // All worker threads spawned during this screen's lifetime. Joined
    // in the destructor so a worker mid-CURL doesn't outlive the
    // DetailScreen and segfault when it tries to publish.
    std::vector<std::thread> tmdb_workers_;
};

}  // namespace media_browser::ui
