#include "media_browser/ui/search_screen.h"
#include "ui/renderer.h"

namespace media_browser::ui {

SearchScreen::SearchScreen(RadarrClient& radarr) : radarr_(radarr) {
    (void)radarr_;  // Unused in stub; Task 19 wires in Radarr.lookup().
}

Screen SearchScreen::handle_input(const std::vector<platform::InputEvent>& events) {
    for (const auto& e : events) {
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            return Screen::Exit;
        }
    }
    return Screen::Search;
}

void SearchScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    (void)screen_w;
    (void)screen_h;
    // Task 19 replaces this with a virtual-keyboard-driven search UI.
    r.render_media_browser_screen_stub("Search");
}

}  // namespace media_browser::ui
