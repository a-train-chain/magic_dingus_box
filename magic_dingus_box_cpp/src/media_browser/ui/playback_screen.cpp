#include "media_browser/ui/playback_screen.h"

#include <spdlog/spdlog.h>

#include "app/app_state.h"
#include "app/controller.h"
#include "platform/input_manager.h"
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

    // Empty playlist_dir: load_file_with_resolution treats this as "use the
    // path as-is" (no relative resolution). The path is already host-absolute
    // since DetailScreen ran it through RadarrClient::resolve_host_path.
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

    controller_.play();

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

void PlaybackScreen::render(::ui::Renderer& /*r*/,
                            int /*screen_w*/, int /*screen_h*/) {
    // Implemented in Task 6.
}

}  // namespace media_browser::ui
