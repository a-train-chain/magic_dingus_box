#include "media_browser/ui/playback_screen.h"

#include <spdlog/spdlog.h>

#include "app/app_state.h"
#include "app/controller.h"
#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/tmdb_client.h"
#include "media_browser/ui/mb_chrome.h"
#include "platform/input_manager.h"
#include "ui/renderer.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "utils/result.h"

namespace media_browser::ui {

PlaybackScreen::PlaybackScreen(app::Controller& controller, app::AppState& state,
                                ::media_browser::TmdbClient& tmdb,
                                QbittorrentClient* qbit)
    : controller_(controller), state_(state), tmdb_(tmdb), qbit_(qbit) {}

void PlaybackScreen::set_movie(std::string host_path, std::string title) {
    movie_path_ = std::move(host_path);
    movie_title_ = std::move(title);
    // Reset overlay meta so a stale entry from a previous movie doesn't leak.
    overlay_meta_ = {};
    overlay_meta_.title = movie_title_;
}

void PlaybackScreen::set_movie_meta(PlaybackOverlayMovieMeta meta) {
    overlay_meta_ = std::move(meta);
}

void PlaybackScreen::enter() {
    exit_pending_ = false;
    deferred_toast_.clear();
    qbit_was_paused_by_us_ = false;
    // Tracks state.video_active across update() ticks for end-of-stream
    // edge detection. Starts false; update() needs to see false→true (play
    // started) before a later true→false transition reads as natural EOS.
    was_video_active_ = false;

    if (movie_path_.empty()) {
        deferred_toast_ = "No movie file path";
        exit_pending_ = true;
        return;
    }

    // Pause torrents BEFORE loading the movie so the GStreamer
    // demuxer's initial reads don't have to fight qBit's piece-write
    // bursts for disk bandwidth. On USB-flash media (the typical Pi
    // setup), concurrent random read+write tanks throughput to
    // single-digit MB/s and makes scrubbing feel frozen. Pausing
    // gives the playback reader exclusive disk access.
    //
    // Best-effort: a qBit failure here doesn't abort playback — we
    // just log and continue with whatever performance the disk can
    // give us. Same rationale as the controller_.load_file fallback
    // path below.
    if (qbit_ != nullptr) {
        if (qbit_->pause_all()) {
            qbit_was_paused_by_us_ = true;
        } else {
            spdlog::warn("[playback] qbit pause_all failed; "
                         "playback may stutter on USB-flash media");
        }
    }

    // Empty playlist_dir disables the playlist-dir-relative resolution
    // strategy in path_resolver. The path is already host-absolute (passed
    // through RadarrClient::resolve_host_path by DetailScreen), so the
    // resolver's absolute-path branch returns it unchanged.
    auto load_result = controller_.load_file_with_resolution(
        movie_path_, /*playlist_dir=*/"", /*start=*/0.0, /*end=*/0.0,
        /*loop=*/false);

    // Result<> exposes operator bool — false on failure. .error() carries
    // the message.
    if (!load_result) {
        deferred_toast_ = "Playback failed: " + load_result.error();
        spdlog::error("[playback] load failed for '{}': {}",
                      movie_path_, load_result.error());
        exit_pending_ = true;
        return;
    }

    // load_file_with_resolution -> GstPlayer::load_file -> play() internally,
    // so the pipeline is already transitioning to PLAYING here. Calling
    // controller_.play() again would issue a second set_state(PLAYING) on
    // the same pipeline back-to-back, which trips the state machine and
    // logs spurious "state change to PLAYING failed" errors. One play() per
    // load is the contract.

    // Title marquee for 3 seconds on entry.
    title_marquee_until_ = std::chrono::steady_clock::now()
                          + std::chrono::seconds(3);

    // Initial warmup: suppress EOS detection for ~1 second so an
    // immediate user-initiated scrub right after pressing Play doesn't
    // get its post-seek state-flicker mistaken for end-of-stream. The
    // pipeline takes a moment to reach steady PLAYING state after load,
    // and seeks issued during that window cause extra-long flicker.
    eos_suppress_frames_ = 60;

    spdlog::info("[playback] playing '{}' (path='{}')",
                 movie_title_, movie_path_);

    // Start pre-fetching similar films in the background so they are ready
    // by the time the user presses the rotary to open the overlay.
    // Idempotent for the same tmdb_id; no-op when tmdb_id == 0.
    // The fetch runs on a detached std::thread inside PlaybackOverlay; the
    // overlay's destructor joins it, so lifetime is safe.
    overlay_.start_prefetch(tmdb_, overlay_meta_);
}

void PlaybackScreen::leave() {
    // Idempotent — safe to call from any exit path. Catches the case where
    // the user long-presses BTN4 and the dispatcher hard-exits to MainMenu.
    controller_.stop();

    // Cancel any in-flight similar-films prefetch so its thread doesn't
    // outlive the screen's use of tmdb_ after we've left. The thread checks
    // cancel_requested_ and exits early; join happens in PlaybackOverlay's
    // destructor (or on the next start_prefetch call).
    overlay_.cancel_prefetch();
    overlay_.close();

    // Force state.video_active false immediately. controller_.stop() tears
    // down the pipeline, but state.video_active is normally only updated
    // by Controller::update_state(), which runs every other frame in the
    // main loop. Without this explicit reset, the next render tick sees
    // stale video_active=true and runs gst_renderer.render() over a
    // torn-down pipeline, painting a stale texture fragment (the
    // upper-right green rectangle the user reports). Same pattern as
    // main.cpp's main-UI exit path.
    state_.video_active = false;
    state_.show_seek_bar = false;

    if (!deferred_toast_.empty()) {
        ::ui::Toast::show(deferred_toast_);
        deferred_toast_.clear();
    }

    // Resume torrents only if WE paused them — never resume a torrent
    // the operator had manually stopped before entering playback. The
    // qbit_was_paused_by_us_ flag is the consent record.
    if (qbit_ != nullptr && qbit_was_paused_by_us_) {
        if (!qbit_->resume_all()) {
            spdlog::warn("[playback] qbit resume_all failed; "
                         "operator may need to manually resume from web UI");
        }
        qbit_was_paused_by_us_ = false;
    }

    spdlog::info("[playback] left playback screen");
}

Screen PlaybackScreen::handle_input(
        const std::vector<platform::InputEvent>& events) {
    // First: if natural end-of-stream or load failure already armed an exit,
    // honor it. This fires every frame even with empty events because the
    // dispatcher always calls handle_input.
    if (exit_pending_) {
        return Screen::Detail;
    }

    for (const auto& e : events) {
        // BTN4 short-press:
        //   - When overlay is open → close the overlay (stay in Playback).
        //   - When overlay is closed → return to Detail (existing behavior).
        // The dispatcher's long-press handler (held >500ms) intercepts before
        // we see it, so reaching here means it's a short press. We don't call
        // controller_.stop() because leave() will (idempotently).
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            if (overlay_.is_open()) {
                overlay_.close();
                return Screen::Playback;
            }
            return Screen::Detail;
        }

        // Rotary press (SELECT):
        //   - When overlay is closed → open it.
        //   - When overlay is open → no-op for now; Task 9 will wire quick-add here.
        // Note: only on key-down (pressed == true); ignore key-up.
        if (e.action == platform::InputAction::SELECT && e.pressed) {
            if (!overlay_.is_open()) {
                overlay_.open();
            }
            // TODO Task 9: when overlay_.is_open(), call quick-add on
            // overlay_.focused_film() via Radarr.
            return Screen::Playback;
        }

        // BTN2 (PLAY_PAUSE, red) = toggle play/pause during MB playback.
        // main.cpp's global exit-modal intercept exempts Screen::Playback
        // so this handler stays reachable while a movie is playing.
        // NOTE: BTN2 is intentionally NOT gated by overlay_.is_open() —
        // the user explicitly asked for BTN2 = pause/play to work even
        // when the overlay is open.
        if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed) {
            controller_.toggle_pause();
            continue;
        }

        // Every seek bumps the EOS-suppression counter. A FLUSH seek
        // briefly transitions the pipeline through PAUSED, which makes
        // state.video_active flicker false — without suppression the
        // EOS edge detector in update() would misread that as end-of-
        // stream and bail to Detail. 30 frames = ~0.5 s at 60 fps,
        // plenty of margin for the pipeline to resettle.
        constexpr int kSeekSuppressFrames = 30;

        // Reusable timer value. Matches the main UI's 1.5s convention so
        // the bar's hide-after-scrub feel is identical to the playlist
        // scrub experience the user wants to mirror.
        constexpr double kSeekBarVisibleSec = 1.5;

        // ±10s with PREV/NEXT — same as main UI when video is playing.
        if (e.action == platform::InputAction::NEXT && e.pressed) {
            controller_.seek(10.0);
            state_.show_seek_bar = true;
            state_.seek_bar_timer = kSeekBarVisibleSec;
            eos_suppress_frames_ = kSeekSuppressFrames;
            continue;
        }
        if (e.action == platform::InputAction::PREV && e.pressed) {
            controller_.seek(-10.0);
            state_.show_seek_bar = true;
            state_.seek_bar_timer = kSeekBarVisibleSec;
            eos_suppress_frames_ = kSeekSuppressFrames;
            continue;
        }

        // ±5s with C-stick.
        if (e.action == platform::InputAction::SEEK_RIGHT) {
            controller_.seek(5.0);
            state_.show_seek_bar = true;
            state_.seek_bar_timer = kSeekBarVisibleSec;
            eos_suppress_frames_ = kSeekSuppressFrames;
            continue;
        }
        if (e.action == platform::InputAction::SEEK_LEFT) {
            controller_.seek(-5.0);
            state_.show_seek_bar = true;
            state_.seek_bar_timer = kSeekBarVisibleSec;
            eos_suppress_frames_ = kSeekSuppressFrames;
            continue;
        }

        // Rotary twist:
        //   - When overlay is open → scroll the similar-films carousel.
        //   - When overlay is closed → velocity-curve seek (existing behavior).
        //
        // Velocity-curve seek: same shape as the main UI's playlist scrub
        // (main.cpp:1903), but with the max-seek constant scaled up because
        // movies are ~10x longer than the typical playlist video. The
        // playlist formula gives 5s slow → 30s fast (a visible 4% jump on a
        // 12-min video); on a 2hr movie that same 30s is only 0.4% of the
        // runtime, which feels like nothing. Bumping the curve to 5s slow
        // → 120s (~2 min) fast restores the same proportional travel: about
        // 1-2% of total runtime per fast click, suitable for chapter-
        // skipping while preserving precise small scrubs at slow velocities
        // (the velocity² factor means low-velocity ticks still produce
        // ~5-10s seeks, same as the playlist).
        if (e.action == platform::InputAction::ROTATE && e.delta != 0) {
            if (overlay_.is_open()) {
                overlay_.on_rotate(e.delta);
                continue;
            }
            double velocity = static_cast<double>(e.velocity);
            double seek_seconds = 5.0 + 115.0 * (velocity * velocity);
            controller_.seek(seek_seconds * e.delta);
            state_.show_seek_bar = true;
            state_.seek_bar_timer = kSeekBarVisibleSec;
            eos_suppress_frames_ = kSeekSuppressFrames;
            continue;
        }
    }

    return Screen::Playback;
}

void PlaybackScreen::update() {
    // Decay the post-seek / post-enter EOS suppression counter. While
    // it's positive, we ignore the state.video_active true→false
    // transition that GStreamer's FLUSH-seek state-machine produces;
    // without this guard, the edge below would mistake "pipeline
    // briefly went PAUSED for the seek" as "movie ended."
    if (eos_suppress_frames_ > 0) {
        --eos_suppress_frames_;
    }

    // Edge-detect natural end-of-stream: state.video_active flips
    // true→false when the GStreamer pipeline reaches EOS. We only
    // treat it as end-of-stream if WE didn't trigger the stop AND the
    // suppression counter has fully decayed (no recent seek that could
    // be causing the flicker).
    bool video_active_now = state_.video_active;
    if (was_video_active_ && !video_active_now && !exit_pending_
        && eos_suppress_frames_ <= 0) {
        exit_pending_ = true;
        spdlog::info("[playback] natural end-of-stream detected");
    }
    was_video_active_ = video_active_now;
}

void PlaybackScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    const ::ui::Theme& th = r.mb_theme();
    const float w = static_cast<float>(screen_w);
    const float h = static_cast<float>(screen_h);

    // Title marquee — top-left, 3 seconds with linear fade-out over the
    // last 500 ms. Sits BELOW the 40 px wood-frame top band (which would
    // otherwise hide the heading), with breathing room above the inset
    // movie. Pre-v1.6.x this was at y=38 / rule y=58 — those put both
    // baseline and rule UNDER the wood frame.
    //   y_baseline = 40 (frame inner edge) + ~30 (heading height + pad) = 70
    //   y_rule     = baseline + 20
    // kPaddingX matches mb_chrome::kSafeInset_px so the heading lines up
    // horizontally with every other Marquee screen's title.
    auto now = std::chrono::steady_clock::now();
    if (now < title_marquee_until_) {
        auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            title_marquee_until_ - now).count();
        float alpha = 1.0f;
        if (remaining_ms < 500) {
            alpha = static_cast<float>(remaining_ms) / 500.0f;
        }
        std::string heading = "NOW PLAYING";
        if (!movie_title_.empty()) {
            heading += " — " + movie_title_;
        }
        const float kPaddingX  = 60.0f;   // matches chrome::kSafeInset_px
        const float kBaselineY = 70.0f;   // below the 40 px wood top band
        r.mb_draw_title_text(heading, kPaddingX, kBaselineY,
                             th.font_heading_size, th.dim, alpha);
        // Underline beneath, just like Detail's header rule.
        const float rule_y = 90.0f;       // 20 px under heading baseline
        r.mb_draw_line(kPaddingX, rule_y, w - kPaddingX, rule_y,
                       2.0f, th.dim, 0.95f * alpha);
    }

    // Match the main UI's playlist seek bar exactly — same colors,
    // same geometry (80% screen width, 4px track, 10px playhead, time
    // labels above), same fade behavior. Single source of truth lives
    // in Renderer::render_seek_bar; mb_render_seek_bar is a thin
    // wrapper. video_active is no longer gated inside; the decision to
    // show is purely show_seek_bar + timer, both of which the
    // PlaybackScreen handle_input path already manages correctly.
    r.mb_render_seek_bar(state_);

    // Persistent control hint — full chrome footer (six-input icon row),
    // matching every other Marquee screen. Replaces the previous bottom-
    // right text-only hint. Sits above the 40 px wood-frame bottom band
    // (the chrome helper anchors at screen_h - kFrameInset_px - kPad3).
    // When the overlay is open the footer is hidden (the overlay renders its
    // own hint row) so the two panels don't clash visually.
    if (!overlay_.is_open()) {
        namespace mc = ::media_browser::ui::chrome;
        mc::draw_footer_hints(r, screen_w, screen_h, {
            {mc::HintIcon::Btn1Yellow,  "\xE2\x88\x92" "10s"},  // −10s
            {mc::HintIcon::Btn2Red,     "Pause"},
            {mc::HintIcon::Btn3Green,   "+10s"},
            {mc::HintIcon::Btn4Black,   "Stop"},
            {mc::HintIcon::RotaryNav,   "Seek"},
            {mc::HintIcon::RotaryPress, "Info"},
        });
    }

    // Overlay: bottom-1/3 panel with movie meta + similar-films carousel.
    // Drawn LAST so it sits above all other HUD elements.
    overlay_.render(r, screen_w, screen_h);

    (void)w;
    (void)h;
}

}  // namespace media_browser::ui
