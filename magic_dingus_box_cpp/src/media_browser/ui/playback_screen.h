#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "app/movie_quiet_mode.h"
#include "media_browser/sonarr/sonarr_types.h"  // EpisodeInfo (stored by value)
#include "media_browser/ui/episode_logic.h"
#include "media_browser/ui/mb_screen.h"
#include "media_browser/ui/playback_overlay.h"
#include "media_browser/ui/still_watching.h"

// Forward declarations to keep this header light.
namespace app {
class Controller;
struct AppState;
}
namespace ui { class Renderer; }
namespace media_browser { class QbittorrentClient; }
namespace media_browser { class RadarrClient; }
namespace media_browser { class TmdbClient; }

namespace media_browser::ui {

// Plays an ad-hoc movie file through the existing kiosk GStreamer pipeline.
// Constructed once in main.cpp; DetailScreen sets the movie via set_movie()
// before transitioning into Screen::Playback.
//
// Lifecycle:
//   set_movie(host_path, title)      <- caller sets target before transition
//   set_movie_meta(meta)             <- caller sets overlay metadata (tmdb_id,
//                                       synopsis, genres, runtime) before
//                                       transitioning. set_movie() must be
//                                       called first (it owns the path).
//   enter()                          <- load + play, arm title marquee,
//                                       kick off similar-films prefetch.
//   handle_input(events) -> Screen   <- maps inputs to Controller methods,
//                                       returns origin_ (default Detail) on
//                                       BTN4 or on natural end-of-stream.
//   update()                         <- edge-detects end-of-stream,
//                                       decays title marquee.
//   render(r, w, h)                  <- draws HUD + playback overlay (when open).
//   leave()                          <- idempotent stop(); surfaces any
//                                       deferred toast; cancels prefetch.
class PlaybackScreen : public MbScreen {
public:
    // Torrent-stack quieting during playback (the contention guard) is
    // delegated to an app::MovieQuietMode set via set_quiet_mode(); when
    // none is set, playback simply runs without managing qBit/containers
    // (unit tests, devs running without the Docker stack).
    //
    // tmdb is used by the PlaybackOverlay to fetch similar films in the
    // background when the user opens the overlay (rotary press).
    //
    // radarr is used by the SELECT handler (rotary press while overlay is
    // open) to quick-add the focused similar film via Radarr.
    PlaybackScreen(app::Controller& controller, app::AppState& state,
                   ::media_browser::TmdbClient& tmdb,
                   ::media_browser::RadarrClient& radarr);

    // The contention guard's executor (owned by main.cpp, outlives this
    // screen). enter() requests a pause in the per-session mode; leave()
    // requests the matching resume only if enter() requested a pause. Both
    // return immediately — the qBit round-trips and the docker stop/start
    // script run on the executor's worker, never on the render thread.
    void set_quiet_mode(app::MovieQuietMode* quiet) { quiet_ = quiet; }

    // Builds the executor's actions: Trickle = qBit alternative speed
    // limits; FullPause = qBit pause_all() + playback_services_pause.sh
    // pause. Resume undoes exactly what the pause reported. `barrier` (may
    // be empty) runs first on the worker before each action — main.cpp
    // uses it to let a pending game quiet-mode transition finish first.
    static app::MovieQuietMode::Actions make_quiet_actions(
        QbittorrentClient* qbit, std::function<void()> barrier);

    // The quick-add workers publish into members — join before they die
    // (a joinable std::thread member at destruction is terminate()). The
    // deferred-add worker may be inside a 90 s service gate: shutting_down_
    // cancels it so the join costs at most one in-flight request.
    ~PlaybackScreen() override {
        shutting_down_.store(true, std::memory_order_release);
        if (quickadd_worker_.joinable()) quickadd_worker_.join();
        if (deferred_add_worker_.joinable()) deferred_add_worker_.join();
    }

    // Caller (main.cpp dispatcher, on Detail->Playback) sets these BEFORE
    // returning Screen::Playback. Last setter wins.
    void set_movie(std::string host_path, std::string title);

    // Optional — sets rich TMDB metadata for the overlay's header and
    // similar-films pre-fetch. Call after set_movie() and before the
    // screen's enter(). When not called (local files, no TMDB binding),
    // tmdb_id defaults to 0 and the overlay renders without similar films.
    void set_movie_meta(PlaybackOverlayMovieMeta meta);

    // Where BTN4 short-press / natural end-of-stream returns to. Set by the
    // dispatcher on EVERY handoff into Playback (Detail today, SeriesDetail
    // in Task 5). Defaults to Screen::Detail — belt-and-braces for any path
    // that forgets to call it (preserves the pre-Task-4 behavior).
    void set_origin(Screen s) { origin_ = s; }

    // One-shot resume offset in seconds. enter() forwards it as the start
    // parameter of Controller::load_file_with_resolution; leave() clears it
    // so a later playback that skips this setter starts from 0.
    void set_start_position(double s) { start_position_ = s; }

    // Which piece of media this playback session's watch state is attributed
    // to. Disengaged = untracked playback (no checkpoints, no watched
    // marking). Cleared in leave() so a stale identity can never attribute a
    // later, unrelated file's positions to the wrong title.
    void set_watch_identity(std::optional<WatchIdentity> id) {
        watch_identity_ = std::move(id);
    }
    std::optional<WatchIdentity> watch_identity() const { return watch_identity_; }

    // Consume-once EOS accessor: returns the engaged watch identity exactly
    // once per EOS latch (eos_reported_ flips on first call; both flags
    // reset together in enter() and in the in-place episode advance).
    // main.cpp polls this every frame and calls WatchStore::mark_watched
    // only on an engaged return — so EOS costs exactly ONE SQLite write,
    // never a per-frame write while a countdown or season-end card idles
    // on screen.
    std::optional<WatchIdentity> take_eos_watched() {
        if (!eos_latched_ || eos_reported_) return std::nullopt;
        eos_reported_ = true;
        return watch_identity_;  // may be disengaged -> caller skips
    }

    // TV episode context — handed by the dispatcher (Task 6) on every
    // SeriesDetail->Playback handoff, BEFORE the transition. host_paths is
    // index-aligned with episodes (empty string when the episode has no
    // file), pre-resolved by SeriesDetail via SonarrClient::resolve_host_path
    // so playback never touches the Sonarr layer. rows/watch are the season
    // rows and the per-series watch map the end-of-episode overlay decides
    // from; the map is this screen's IN-MEMORY copy only — the persistent
    // store write stays in main.cpp's take_eos_watched() drain. Cleared in
    // leave().
    void set_episode_context(std::vector<EpisodeInfo> episodes,
                             std::vector<std::string> host_paths,
                             std::vector<SeasonRow> rows,
                             watch_map watch,
                             std::string series_title);

    // One-shot "Start Season N" intent from the season-end card's primary.
    // Consumed by the dispatcher on the Playback->SeriesDetail transition
    // (Task 6 wires the receiving side). Deliberately NOT cleared in
    // leave(): the dispatcher may drain it before or after leave() runs on
    // that transition, and clearing there would race the consumer. enter()
    // clears it instead, so a never-consumed intent cannot leak into a
    // later session.
    std::optional<int> take_pending_next_season() {
        auto v = pending_next_season_;
        pending_next_season_.reset();
        return v;
    }

    // Called by main.cpp when the phone remote's tap-to-seek lands on the
    // shared pipeline while THIS screen owns playback. Gives the external
    // seek the same treatment the local seek handlers give themselves:
    // EOS-flicker suppression (a FLUSH seek briefly drops video_active,
    // which update()'s edge detector would otherwise misread as "movie
    // ended" and bail out of playback) plus the on-TV scrub-bar flash.
    void notify_external_seek();

    // Pipeline-error probe, wired by main.cpp to the shared GstPlayer's
    // has_error() (the Controller does not expose it). Polled by update()
    // BEFORE the natural-end edge detector: an errored file (corrupt,
    // truncated, undecodable) exits playback with a toast and is never
    // mistaken for end-of-stream — no watched mark, no next-episode
    // countdown. Unset (tests, dev builds) = errors are not detected.
    void set_error_probe(std::function<bool()> probe) {
        error_probe_ = std::move(probe);
    }

    // True once this session was abandoned because of a pipeline error;
    // reset by enter() / the in-place episode advance. main.cpp's exit
    // sites skip the final watch-state flush for such a session, so an
    // error near the end of a file cannot cross the watched threshold via
    // the position write (a natural-end-only decision).
    bool ended_on_error() const { return ended_on_error_; }

    void enter() override;
    void leave() override;
    Screen handle_input(const std::vector<platform::InputEvent>& events) override;
    void update() override;
    void render(::ui::Renderer& r, int screen_w, int screen_h) override;

private:
    app::Controller&              controller_;
    app::AppState&                state_;
    ::media_browser::TmdbClient&  tmdb_;
    ::media_browser::RadarrClient& radarr_;
    // Contention-guard executor (null = no quieting). The consent records
    // (alt-limits engaged by us / torrents paused by us / containers
    // stopped by us) live on its worker — see app::MovieQuietMode::Consent.
    app::MovieQuietMode*          quiet_ = nullptr;
    // True when this session's enter() requested a pause, so leave()
    // requests the resume exactly once and only then.
    bool quiet_requested_ = false;

    std::string movie_title_;
    std::string movie_path_;       // host-side path

    // See set_origin() / set_start_position() / set_watch_identity().
    Screen origin_ = Screen::Detail;
    double start_position_ = 0.0;                  // one-shot; cleared in leave()
    std::optional<WatchIdentity> watch_identity_;  // cleared in leave()

    // EOS latch pair. eos_latched_ is set by update()'s video_active
    // true→false edge; eos_reported_ flips on the first take_eos_watched()
    // so the caller sees the latch exactly once. Both reset together in
    // enter() AND in advance_to_next_episode() — the two session starts.
    // At the edge, movie/no-identity sessions ALSO arm exit_pending_
    // (unchanged behavior); TV sessions latch ONLY and hand control to the
    // end-of-episode overlay, whose own outcomes are the only TV setters
    // of exit_pending_ (missing file / load failure).
    bool eos_latched_ = false;
    bool eos_reported_ = false;

    // ---- TV episode context (Task 5) — see set_episode_context() ----
    std::vector<EpisodeInfo> episodes_;
    std::vector<std::string> episode_host_paths_;  // index-aligned with episodes_
    std::vector<SeasonRow> season_rows_;
    watch_map watch_;              // in-memory copy; store writes live in main.cpp
    std::string series_title_;
    // Cached position of the playing episode in episodes_. A hint, never
    // trusted blindly: begin_end_overlay() re-validates it against the
    // watch identity's (season, episode) and falls back to a linear search
    // — a stale vector (or an identity that advanced past it) takes the
    // movie-style exit instead of indexing garbage.
    int current_index_ = -1;

    // ---- End-of-episode overlay state machine ----
    // kind == None -> no overlay (movies never leave None). Countdown uses
    // the frame-clock timer below (the series_detail confirm-timer idiom);
    // expiry or SELECT triggers the in-place advance, RED/BTN4 exit.
    EndOverlayModel end_overlay_;
    std::chrono::steady_clock::time_point countdown_started_at_{};
    std::optional<int> pending_next_season_;  // see take_pending_next_season()

    // "Still watching?" streak guard (still_watching.h — pure, Mac-tested).
    // Counts episodes started without user interaction in the CURRENT
    // continuous playback session: enter() zeroes it (manual start), the
    // countdown-expiry auto-advance increments it, and every input this
    // screen handles during an episode — pause, seek, rotary, phone remote
    // (same InputActions / notify_external_seek path) — zeroes it again.
    // At kAutoAdvanceStreakLimit, begin_end_overlay() swaps the Countdown
    // for the StillWatching prompt; its 60 s expiry stops playback via the
    // NORMAL exit path (exit_pending_ -> origin_), never a bypass of
    // leave(). countdown_started_at_ doubles as the prompt's frame clock
    // (only one end overlay exists at a time).
    StillWatchingGuard still_watching_;

    // Arms end_overlay_ at a TV EOS edge: locates the finished episode
    // (identity-validated), syncs the in-memory watch map, and resolves
    // the overlay via decide_end_overlay. S0 identities, an empty episode
    // vector, or a no-match all skip the overlay -> movie-style exit.
    void begin_end_overlay();

    // In-place advance to episodes_[end_overlay_.next_index]: stop, swap
    // path/title/meta (preserving overlay meta), re-arm the enter() side
    // effects explicitly (enter() is NOT re-run), reset the EOS latch pair
    // together, and load. Missing file / load failure -> deferred toast +
    // exit_pending_ (the enter() precedent).
    void advance_to_next_episode();

    // Dim scrim + countdown / season-end card. Drawn last (above the HUD).
    void render_end_overlay(::ui::Renderer& r, int screen_w, int screen_h);

    bool was_video_active_ = false;
    bool exit_pending_ = false;
    std::function<bool()> error_probe_;   // see set_error_probe()
    bool ended_on_error_ = false;         // see ended_on_error()
    std::string deferred_toast_;

    // Async quick-add (overlay SELECT). get_quality_profiles + add_movie
    // are two 5s-timeout HTTP calls — run inline they froze the PLAYING
    // MOVIE for up to ~10s. The worker composes the result toast; update()
    // drains it. One at a time. Only used when Radarr is up during the
    // movie (trickle sessions) — a FullPause session stops the container,
    // so its presses go to the deferred queue below instead.
    std::thread quickadd_worker_;
    std::atomic<bool> quickadd_in_flight_{false};
    std::atomic<bool> quickadd_done_{false};
    std::string quickadd_toast_;   // worker → render, ordered by quickadd_done_

    // WORKER thread. The add itself, shared by the immediate and the
    // deferred path: quality-profile pick + add_movie. Returns the outcome
    // phrase ("Added — searching", "Already in library", ...).
    std::string run_quick_add(int tmdb_id);

    // Deferred quick-add (FullPause sessions). FullPause STOPS the Radarr
    // container for the whole movie, so an add fired during playback could
    // only burn its timeouts and say "Couldn't add". Such a press is queued
    // instead ("Will add after the movie"); leave() hands the session's
    // queue to deferred_add_worker_, which waits on a ServiceGate (the
    // MovieQuietMode resume, then Radarr answering) and then adds each
    // film, reporting through the thread-safe ::ui::Toast::post because the
    // outcome lands after this screen stops receiving update() ticks.
    struct DeferredAdd {
        int tmdb_id = 0;
        std::string title;
    };
    bool session_full_pause_ = false;                  // set by enter()
    std::vector<DeferredAdd> session_deferred_adds_;   // render thread
    void start_deferred_adds();                        // render thread (leave)
    void run_deferred_adds();                          // worker
    std::thread deferred_add_worker_;
    std::mutex deferred_add_mtx_;
    std::vector<DeferredAdd> deferred_add_queue_;      // guarded by the mutex
    bool deferred_add_running_ = false;                // guarded by the mutex
    std::atomic<bool> shutting_down_{false};
    // FullPause session lifecycle as the deferred worker sees it. enter()
    // bumps the counter and raises the flag for a FullPause session;
    // leave() lowers the flag BEFORE start_deferred_adds() takes the
    // mutex, and the worker reads both under that mutex — so a batch it
    // requeues because a session is active is always restarted by that
    // session's leave() (see decide_deferred_batch in service_gate.h).
    std::atomic<bool> full_pause_session_active_{false};
    std::atomic<std::uint64_t> full_pause_sessions_started_{0};

    // Frames remaining during which we suppress end-of-stream detection.
    // Counted down by update(). The state.video_active flag flickers
    // false during the brief PAUSED→PLAYING transition that GStreamer
    // performs as part of a FLUSH seek — without this grace counter,
    // the EOS edge detector in update() interprets that flicker as the
    // movie ending and bails to Detail (user perceives it as the
    // playback "crashing" out). Pumped on enter() (initial warmup) and
    // every seek (post-seek settle) — including phone-remote tap-to-seek
    // via notify_external_seek().
    int eos_suppress_frames_ = 0;

    // Post-seek settle margin (~0.5 s at 60 fps) and the scrub bar's
    // hide-after-scrub window (matches the main UI's 1.5 s convention).
    // Shared by the local seek handlers in handle_input() and by
    // notify_external_seek() so the two paths can never drift.
    static constexpr int kSeekSuppressFrames = 30;
    static constexpr double kSeekBarVisibleSec = 1.5;

    // Publishes this session's identity into the AppState fields the
    // StatusWriter serializes for the phone remote (now_playing.title /
    // .subtitle / .kind, and clears the playlist context — MB playback has
    // none). Movie sessions: title + release year + kind "movie". TV
    // sessions (watch identity kind Tv with a series title): series title +
    // "SxEy · <episode>" + kind "tv". Called from enter() and from
    // advance_to_next_episode() — the two session starts; leave() clears.
    void publish_now_playing_status();

    std::chrono::steady_clock::time_point title_marquee_until_{};

    // Overlay metadata — populated by set_movie_meta() before enter().
    // Falls back to title-only (no synopsis, no similar films) when not set.
    PlaybackOverlayMovieMeta overlay_meta_;

    // Bottom-1/3 similar-films overlay (rotary press to open, BTN4 to close).
    PlaybackOverlay overlay_;

    // --- HUD auto-hide state machine ---
    //
    // The scrub bar and footer hints are hidden while the movie plays
    // normally. Any input event (button press, rotary, pause) bumps
    // hud_visible_until_ to now + kHudShowMs. The render path fades the
    // HUD out over kHudFadeMs once the window expires. While paused the
    // HUD is always fully visible regardless of the timer.
    std::chrono::steady_clock::time_point hud_visible_until_{};
    static constexpr int kHudShowMs = 3000;   // visible for 3s after any input
    static constexpr int kHudFadeMs = 300;    // fade tail duration

    // Extend hud_visible_until_ to now + kHudShowMs. Call on every input.
    void bump_hud_visibility();

    // Compute HUD alpha [0..1]. 1.0 when paused; otherwise time-window based.
    // `paused` should be !state_.video_active (or however pause state is checked).
    float hud_alpha(bool paused) const;
};

}  // namespace media_browser::ui
