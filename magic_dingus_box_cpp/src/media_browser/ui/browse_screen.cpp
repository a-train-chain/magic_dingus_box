#include "media_browser/ui/browse_screen.h"
#include "ui/renderer.h"

namespace media_browser::ui {

BrowseScreen::BrowseScreen(RadarrClient& radarr) : radarr_(radarr) {
    (void)radarr_;  // Unused in stub; Task 18 wires in library fetch.
}

Screen BrowseScreen::handle_input(const std::vector<platform::InputEvent>& events) {
    for (const auto& e : events) {
        // BTN4 / Menu returns to the kiosk main menu.
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            return Screen::Exit;
        }
    }
    return Screen::Browse;  // Stay on this screen.
}

void BrowseScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    (void)screen_w;
    (void)screen_h;
    // Task 18 replaces this with the real poster grid + category rail.
    r.render_media_browser_screen_stub("Browse");
}

}  // namespace media_browser::ui
