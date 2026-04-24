#include "media_browser/ui/library_screen.h"
#include "ui/renderer.h"

namespace media_browser::ui {

LibraryScreen::LibraryScreen(RadarrClient& radarr) : radarr_(radarr) {
    (void)radarr_;  // Unused in stub; Task 22 wires in Radarr.get_library().
}

Screen LibraryScreen::handle_input(const std::vector<platform::InputEvent>& events) {
    for (const auto& e : events) {
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            return Screen::Exit;
        }
    }
    return Screen::Library;
}

void LibraryScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    (void)screen_w;
    (void)screen_h;
    // Task 22 replaces this with the downloaded-movies library grid.
    r.render_media_browser_screen_stub("Library");
}

}  // namespace media_browser::ui
