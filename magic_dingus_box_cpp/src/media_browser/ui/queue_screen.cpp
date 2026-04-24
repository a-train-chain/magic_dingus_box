#include "media_browser/ui/queue_screen.h"
#include "ui/renderer.h"

namespace media_browser::ui {

QueueScreen::QueueScreen(RadarrClient& radarr) : radarr_(radarr) {
    (void)radarr_;  // Unused in stub; Task 21 wires in Radarr.get_queue().
}

Screen QueueScreen::handle_input(const std::vector<platform::InputEvent>& events) {
    for (const auto& e : events) {
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            return Screen::Exit;
        }
    }
    return Screen::Queue;
}

void QueueScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    (void)screen_w;
    (void)screen_h;
    // Task 21 replaces this with the live download queue UI.
    r.render_media_browser_screen_stub("Queue");
}

}  // namespace media_browser::ui
