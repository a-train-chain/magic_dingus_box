#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "media_browser/ui/mb_screen.h"

// Forward declarations to keep this header light.
namespace app {
class Controller;
struct AppState;
}
namespace ui { class Renderer; }

namespace media_browser::ui {

// Plays an ad-hoc movie file through the existing kiosk GStreamer pipeline.
// Constructed once in main.cpp; DetailScreen sets the movie via set_movie()
// before transitioning into Screen::Playback.
//
// Lifecycle:
//   set_movie(host_path, title)      <- caller sets target before transition
//   enter()                          <- load + play, arm title marquee
//   handle_input(events) -> Screen   <- maps inputs to Controller methods,
//                                       returns Screen::Detail on BTN4 or
//                                       on natural end-of-stream.
//   update()                         <- edge-detects end-of-stream,
//                                       decays title marquee.
//   render(r, w, h)                  <- draws HUD only (video frame is
//                                       drawn by the main render loop's
//                                       state.video_active path).
//   leave()                          <- idempotent stop(); surfaces any
//                                       deferred toast.
class PlaybackScreen : public MbScreen {
public:
    PlaybackScreen(app::Controller& controller, app::AppState& state);

    // Caller (main.cpp dispatcher, on Detail->Playback) sets these BEFORE
    // returning Screen::Playback. Last setter wins.
    void set_movie(std::string host_path, std::string title);

    void enter() override;
    void leave() override;
    Screen handle_input(const std::vector<platform::InputEvent>& events) override;
    void update() override;
    void render(::ui::Renderer& r, int screen_w, int screen_h) override;

private:
    app::Controller& controller_;
    app::AppState&   state_;

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
};

}  // namespace media_browser::ui
