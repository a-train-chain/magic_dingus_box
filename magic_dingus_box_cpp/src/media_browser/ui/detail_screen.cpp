#include "media_browser/ui/detail_screen.h"
#include "ui/renderer.h"

namespace media_browser::ui {

DetailScreen::DetailScreen(RadarrClient& radarr) : radarr_(radarr) {
    (void)radarr_;  // Unused in stub; Task 20 wires in movie metadata fetch.
}

Screen DetailScreen::handle_input(const std::vector<platform::InputEvent>& events) {
    for (const auto& e : events) {
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            return Screen::Exit;
        }
    }
    return Screen::Detail;
}

void DetailScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    (void)screen_w;
    (void)screen_h;
    // Task 20 replaces this with the poster + synopsis + action buttons.
    r.render_media_browser_screen_stub("Detail");
}

}  // namespace media_browser::ui
