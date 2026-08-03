#pragma once

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "media_browser/sonarr/sonarr_types.h"  // EpisodeInfo (stored by value)
#include "media_browser/ui/episode_logic.h"
#include "media_browser/ui/mb_screen.h"
#include "media_browser/ui/playback_overlay.h"

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
    // qbit pointer is optional. When provided, enter()/leave() quiet the
    // torrent stack for the duration of playback — concurrent torrent
    // writes contend with GStreamer's reads and cause scrubbing freezes.
    // HOW they quiet it is per-board (state.platform_profile
    // .trickle_torrents_during_video): Pi 5 engages qBit's alternative
    // speed limits (~1.5 MB/s trickle — downloads keep progressing);
    // Pi 4B / Unknown keep the full pause_all() (USB-flash media has no
    // random-IO headroom to give away). When null, playback simply runs
    // without managing qBit state (e.g., unit tests, devs running
    // without the Docker stack).
    //
    // tmdb is used by the PlaybackOverlay to fetch similar films in the
    // background when the user opens the overlay (rotary press).
    //
    // radarr is used by the SELECT handler (rotary press while overlay is
    // open) to quick-add the focused similar film via Radarr.
    PlaybackScreen(app::Controller& controller, app::AppState& state,
                   ::media_browser::TmdbClient& tmdb,
                   ::media_browser::RadarrClient& radarr,
                   QbittorrentClient* qbit = nullptr);

    // The quick-add worker publishes into members — join before they
    // die (a joinable std::thread member at destruction is terminate()).
    ~PlaybackScreen() override {
        if (quickadd_worker_.joinable()) quickadd_worker_.join();
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
    QbittorrentClient*            qbit_ = nullptr;  // optional; pause/resume during playback

    // Tracks whether enter() asked qBit to pause. leave() only resumes
    // if pause actually succeeded — avoids accidentally starting
    // torrents that the operator manually paused before entering
    // playback (we'd be flipping their state without consent).
    bool qbit_was_paused_by_us_ = false;

    // Trickle-branch mirror of the flag above: set when enter() engaged
    // qBit's alternative speed limits, so leave() clears the cap only if
    // WE set it — an operator who had alt limits on for their own
    // reasons keeps them. Exactly one of these two flags can be set per
    // session (the enter() branch is either/or on the platform profile).
    bool qbit_alt_limited_by_us_ = false;

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
    std::string deferred_toast_;

    // Async quick-add (overlay SELECT). get_quality_profiles + add_movie
    // are two 5s-timeout HTTP calls — run inline they froze the PLAYING
    // MOVIE for up to ~10s (and on a Pi 4B, where the Radarr container is
    // docker-paused during playback, the freeze was guaranteed and the
    // add could never succeed until playback ended). The worker composes
    // the result toast; update() drains it. One at a time.
    std::thread quickadd_worker_;
    std::atomic<bool> quickadd_in_flight_{false};
    std::atomic<bool> quickadd_done_{false};
    std::string quickadd_toast_;   // worker → render, ordered by quickadd_done_

    // Frames remaining during which we suppress end-of-stream detection.
    // Counted down by update(). The state.video_active flag flickers
    // false during the brief PAUSED→PLAYING transition that GStreamer
    // performs as part of a FLUSH seek — without this grace counter,
    // the EOS edge detector in update() interprets that flicker as the
    // movie ending and bails to Detail (user perceives it as the
    // playback "crashing" out). Pumped on enter() (initial warmup) and
    // every seek (post-seek settle).
    int eos_suppress_frames_ = 0;

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
