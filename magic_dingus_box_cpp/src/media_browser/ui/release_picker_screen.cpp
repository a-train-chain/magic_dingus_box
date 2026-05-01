#include "media_browser/ui/release_picker_screen.h"
#include "media_browser/radarr/radarr_client.h"

namespace media_browser::ui {

ReleasePickerScreen::ReleasePickerScreen(::media_browser::RadarrClient& radarr)
    : radarr_(radarr) {}

void ReleasePickerScreen::set_candidates(std::string movie_title,
                                         std::vector<ReleaseCandidate> rows) {
    movie_title_ = std::move(movie_title);
    rows_       = std::move(rows);
    focus_      = 0;
    scroll_top_ = 0;
    // Sort + flag wiring lands in Task 11.
}

Screen ReleasePickerScreen::handle_input(
    const std::vector<platform::InputEvent>& /*events*/) {
    // Skeleton — Task 11 wires DPad navigation + SELECT/BACK handling.
    // For now any input returns to Detail so the screen never traps the user.
    return Screen::Detail;
}

void ReleasePickerScreen::render(::ui::Renderer& /*r*/, int /*w*/, int /*h*/) {
    // Skeleton — Task 11 implements the row layout.
}

void ReleasePickerScreen::sort_candidates(
    std::vector<ReleaseCandidate>& /*rows*/) {
    // Implemented in Task 11.
}

void ReleasePickerScreen::flag_auto_pick_and_threshold(
    std::vector<ReleaseCandidate>& /*rows*/, int /*min_format_score*/) {
    // Implemented in Task 11.
}

}  // namespace media_browser::ui
