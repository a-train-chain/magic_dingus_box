#pragma once

#include "media_browser/ui/mb_screen.h"

namespace media_browser { class RadarrClient; }

namespace media_browser::ui {

// Task 17 stub. Task 20 will replace render() with the movie detail view
// (poster, synopsis, cast, download / watch actions).
class DetailScreen : public MbScreen {
public:
    explicit DetailScreen(RadarrClient& radarr);

    Screen handle_input(const std::vector<platform::InputEvent>& events) override;
    void render(::ui::Renderer& r, int screen_w, int screen_h) override;

private:
    RadarrClient& radarr_;
};

}  // namespace media_browser::ui
