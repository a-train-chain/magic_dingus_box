#include "media_browser/ui/playback_screen.h"

#include "app/app_state.h"
#include "app/controller.h"
#include "platform/input_manager.h"

namespace media_browser::ui {

PlaybackScreen::PlaybackScreen(app::Controller& controller, app::AppState& state)
    : controller_(controller), state_(state) {}

void PlaybackScreen::set_movie(std::string host_path, std::string title) {
    movie_path_ = std::move(host_path);
    movie_title_ = std::move(title);
}

void PlaybackScreen::enter() {
    // Implemented in Task 5.
}

void PlaybackScreen::leave() {
    // Implemented in Task 5.
}

Screen PlaybackScreen::handle_input(
        const std::vector<platform::InputEvent>& /*events*/) {
    // Implemented in Task 5.
    return Screen::Playback;
}

void PlaybackScreen::update() {
    // Implemented in Task 5.
}

void PlaybackScreen::render(::ui::Renderer& /*r*/,
                            int /*screen_w*/, int /*screen_h*/) {
    // Implemented in Task 6.
}

}  // namespace media_browser::ui
