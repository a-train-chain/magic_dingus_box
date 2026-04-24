#include "media_browser/ui/mb_settings_screen.h"
#include "ui/renderer.h"

namespace media_browser::ui {

MbSettingsScreen::MbSettingsScreen(RadarrClient& radarr) : radarr_(radarr) {
    (void)radarr_;  // Unused in stub; Task 23 wires in Radarr profile/root folder calls.
}

Screen MbSettingsScreen::handle_input(const std::vector<platform::InputEvent>& events) {
    for (const auto& e : events) {
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            return Screen::Exit;
        }
    }
    return Screen::MovieSettings;
}

void MbSettingsScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    (void)screen_w;
    (void)screen_h;
    // Task 23 replaces this with the Movie-module settings form.
    r.render_media_browser_screen_stub("Movie Settings");
}

}  // namespace media_browser::ui
