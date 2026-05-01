#pragma once

#include <chrono>
#include <string>
#include <vector>

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
//                                       returns Screen::Detail on BTN4 or
//                                       on natural end-of-stream.
//   update()                         <- edge-detects end-of-stream,
//                                       decays title marquee.
//   render(r, w, h)                  <- draws HUD + playback overlay (when open).
//   leave()                          <- idempotent stop(); surfaces any
//                                       deferred toast; cancels prefetch.
class PlaybackScreen : public MbScreen {
public:
    // qbit pointer is optional. When provided, enter()/leave()
    // pause/resume all torrents to free disk IO for smooth playback —
    // necessary on USB-flash media because concurrent torrent writes
    // contend with GStreamer's reads and cause scrubbing freezes.
    // When null, playback simply runs without managing qBit state
    // (e.g., unit tests, devs running without the Docker stack).
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

    // Caller (main.cpp dispatcher, on Detail->Playback) sets these BEFORE
    // returning Screen::Playback. Last setter wins.
    void set_movie(std::string host_path, std::string title);

    // Optional — sets rich TMDB metadata for the overlay's header and
    // similar-films pre-fetch. Call after set_movie() and before the
    // screen's enter(). When not called (local files, no TMDB binding),
    // tmdb_id defaults to 0 and the overlay renders without similar films.
    void set_movie_meta(PlaybackOverlayMovieMeta meta);

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

    std::string movie_title_;
    std::string movie_path_;       // host-side path

    bool was_video_active_ = false;
    bool exit_pending_ = false;
    std::string deferred_toast_;

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
