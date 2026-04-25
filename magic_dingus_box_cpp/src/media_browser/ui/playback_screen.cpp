#include "media_browser/ui/playback_screen.h"

#include <spdlog/spdlog.h>

#include "app/app_state.h"
#include "app/controller.h"
#include "platform/input_manager.h"
#include "ui/renderer.h"
#include "ui/theme.h"
#include "ui/toast.h"
#include "utils/result.h"

namespace media_browser::ui {

PlaybackScreen::PlaybackScreen(app::Controller& controller, app::AppState& state)
    : controller_(controller), state_(state) {}

void PlaybackScreen::set_movie(std::string host_path, std::string title) {
    movie_path_ = std::move(host_path);
    movie_title_ = std::move(title);
}

void PlaybackScreen::enter() {
    exit_pending_ = false;
    deferred_toast_.clear();
    // Tracks state.video_active across update() ticks for end-of-stream
    // edge detection. Starts false; update() needs to see false→true (play
    // started) before a later true→false transition reads as natural EOS.
    was_video_active_ = false;

    if (movie_path_.empty()) {
        deferred_toast_ = "No movie file path";
        exit_pending_ = true;
        return;
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

    spdlog::info("[playback] playing '{}' (path='{}')",
                 movie_title_, movie_path_);
}

void PlaybackScreen::leave() {
    // Idempotent — safe to call from any exit path. Catches the case where
    // the user long-presses BTN4 and the dispatcher hard-exits to MainMenu.
    controller_.stop();
    state_.show_seek_bar = false;

    if (!deferred_toast_.empty()) {
        ::ui::Toast::show(deferred_toast_);
        deferred_toast_.clear();
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
        // BTN4 short-press → return to Detail. The dispatcher's long-press
        // handler (held >500ms) intercepts before we see it, so reaching
        // here means it's a short press. We don't call controller_.stop()
        // here because leave() will (idempotently); having a single stop
        // point keeps lifecycle reasoning simple.
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            return Screen::Detail;
        }

        if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed) {
            controller_.toggle_pause();
            continue;
        }

        // ±10s with PREV/NEXT — same as main UI when video is playing.
        if (e.action == platform::InputAction::NEXT && e.pressed) {
            controller_.seek(10.0);
            state_.show_seek_bar = true;
            state_.seek_bar_timer = 1.5;
            continue;
        }
        if (e.action == platform::InputAction::PREV && e.pressed) {
            controller_.seek(-10.0);
            state_.show_seek_bar = true;
            state_.seek_bar_timer = 1.5;
            continue;
        }

        // ±5s with C-stick.
        if (e.action == platform::InputAction::SEEK_RIGHT) {
            controller_.seek(5.0);
            state_.show_seek_bar = true;
            state_.seek_bar_timer = 1.5;
            continue;
        }
        if (e.action == platform::InputAction::SEEK_LEFT) {
            controller_.seek(-5.0);
            state_.show_seek_bar = true;
            state_.seek_bar_timer = 1.5;
            continue;
        }

        // Velocity-curve rotary seek — exact same formula as main.cpp:1758.
        if (e.action == platform::InputAction::ROTATE && e.delta != 0) {
            double velocity = static_cast<double>(e.velocity);
            double seek_seconds = 5.0 + 25.0 * (velocity * velocity);
            controller_.seek(seek_seconds * e.delta);
            state_.show_seek_bar = true;
            state_.seek_bar_timer = 1.5;
            continue;
        }
    }

    return Screen::Playback;
}

void PlaybackScreen::update() {
    // Edge-detect natural end-of-stream: state.video_active flips true→false
    // when the GStreamer pipeline reaches EOS. We only treat it as
    // end-of-stream if WE didn't trigger the stop (exit_pending stays false
    // until this branch fires).
    bool video_active_now = state_.video_active;
    if (was_video_active_ && !video_active_now && !exit_pending_) {
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
    // last 500 ms. Matches Detail's "FEATURE PRESENTATION" header style.
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
        const float kPaddingX = 32.0f;
        const float kBaselineY = 38.0f;
        r.mb_draw_title_text(heading, kPaddingX, kBaselineY,
                             th.font_heading_size, th.accent2, alpha);
        // Underline beneath, just like Detail's header rule.
        const float rule_y = 58.0f;
        r.mb_draw_line(kPaddingX, rule_y, w - kPaddingX, rule_y,
                       2.0f, th.accent2, 0.95f * alpha);
    }

    // Seek bar — fades in/out via state.seek_bar_timer (set in handle_input
    // on every seek event, decremented elsewhere). The kiosk's main render
    // path normally draws this when state.video_active && state.show_seek_bar,
    // but its render gate skips MediaBrowser mode, so we render an
    // equivalent strip here using the same Media Browser primitives the
    // rest of the HUD uses. Position mirrors the main UI's seek bar:
    // a thin track ~80% of screen width, centered, sat above the bottom hint.
    if (state_.show_seek_bar && state_.seek_bar_timer > 0.0) {
        const float kBarHpad      = 80.0f;          // gutter on each side
        const float kBarH         = 6.0f;           // total bar thickness
        const float kBarBottomPad = 96.0f;          // distance above screen bottom
        float bar_w = w - 2.0f * kBarHpad;
        if (bar_w < 200.0f) bar_w = 200.0f;
        float bar_x = (w - bar_w) * 0.5f;
        float bar_y = h - kBarBottomPad;
        // Decay alpha during the last 0.5 s of the timer so it fades out.
        float fade_alpha = 1.0f;
        if (state_.seek_bar_timer < 0.5) {
            fade_alpha = static_cast<float>(state_.seek_bar_timer) / 0.5f;
            if (fade_alpha < 0.0f) fade_alpha = 0.0f;
        }
        // Track: dim outline at low alpha — same idiom as Detail's section
        // dividers / Settings sliders.
        r.mb_stroke_rect(bar_x, bar_y, bar_w, kBarH,
                         1.0f, th.dim, 0.5f * fade_alpha);
        // Fill: position-proportional, in accent (gold). Reads
        // state.position / state.duration which the controller updates each
        // frame. Guards against duration <= 0 (transient at startup) by
        // showing 0% rather than NaN-ing.
        double duration = state_.duration;
        double position = state_.position;
        double pct = (duration > 0.0)
                       ? (position / duration)
                       : 0.0;
        if (pct < 0.0) pct = 0.0;
        if (pct > 1.0) pct = 1.0;
        float fill_w = static_cast<float>(pct) * bar_w;
        if (fill_w > 0.0f) {
            r.mb_fill_rect(bar_x, bar_y, fill_w, kBarH,
                           th.accent, 0.95f * fade_alpha);
        }
    }

    // Pause indicator — bottom-center, persistent until unpause.
    if (controller_.is_paused()) {
        std::string label = "PAUSED";
        int sz = th.font_medium_size;
        int baseline = r.mb_text_baseline(sz);
        int tw = r.mb_text_width(label, sz);
        float tx = (w - static_cast<float>(tw)) / 2.0f;
        float ty = h - 60.0f - static_cast<float>(sz)
                 + static_cast<float>(baseline);
        r.mb_draw_text(label, tx, ty, sz, th.dim, 0.85f);
    }

    // Persistent control hint — bottom-right, mirrors Media Browser footer.
    {
        std::string hint = "BTN4: stop   PLAY: pause   ROTATE: seek";
        int sz = th.font_small_size;
        int baseline = r.mb_text_baseline(sz);
        int tw = r.mb_text_width(hint, sz);
        float tx = w - 32.0f - static_cast<float>(tw);
        float ty = h - 12.0f - static_cast<float>(sz)
                 + static_cast<float>(baseline);
        r.mb_draw_text(hint, tx, ty, sz, th.dim, 0.7f);
    }
}

}  // namespace media_browser::ui
