#pragma once

#include "media_browser/ui/mb_screen.h"

namespace media_browser { class RadarrClient; }

namespace media_browser::ui {

// Task 17 stub. Task 20 will replace render() with the movie detail view
// (poster, synopsis, cast, download / watch actions).
//
// Task 18 wires in a tmdb_id setter so the dispatcher can hand off the
// poster the user selected on the Browse screen. The full detail fetch
// lives in Task 20.
class DetailScreen : public MbScreen {
public:
    explicit DetailScreen(RadarrClient& radarr);

    // Set the tmdb_id of the movie the detail screen should display. Called
    // by the dispatcher in main.cpp just before transitioning to this
    // screen. No-op if the id matches the current one (so we don't clobber
    // in-flight state on self-transitions).
    void set_tmdb_id(int tmdb_id) { tmdb_id_ = tmdb_id; }
    int tmdb_id() const { return tmdb_id_; }

    Screen handle_input(const std::vector<platform::InputEvent>& events) override;
    void render(::ui::Renderer& r, int screen_w, int screen_h) override;

private:
    RadarrClient& radarr_;
    int tmdb_id_ = 0;
};

}  // namespace media_browser::ui
