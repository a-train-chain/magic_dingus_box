#pragma once

#include "media_browser/ui/mb_screen.h"

namespace media_browser { class RadarrClient; }

namespace media_browser::ui {

// Task 17 stub. Task 19 will replace render() with the search entry UI
// (virtual keyboard + Radarr lookup results).
class SearchScreen : public MbScreen {
public:
    explicit SearchScreen(RadarrClient& radarr);

    Screen handle_input(const std::vector<platform::InputEvent>& events) override;
    void render(::ui::Renderer& r, int screen_w, int screen_h) override;

private:
    RadarrClient& radarr_;
};

}  // namespace media_browser::ui
