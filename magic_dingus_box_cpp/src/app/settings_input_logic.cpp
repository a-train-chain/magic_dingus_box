#include "settings_input_logic.h"

namespace app {

void MenuButtonHold::press(Clock::time_point now) {
    button_held = true;
    volume_changed_while_held = false;
    press_time = now;
}

MenuButtonHold::Release MenuButtonHold::release(Clock::time_point now) {
    button_held = false;

    auto hold_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - press_time).count();

    // Only toggle menu if we didn't change volume AND it was a short press
    if (!volume_changed_while_held && hold_duration < kMenuHoldThreshold.count()) {
        return Release::ToggleSettings;
    } else if (volume_changed_while_held) {
        // Volume was changed, save settings now
        return Release::SaveVolume;
    }
    return Release::Nothing;
}

bool MenuButtonHold::slider_due(Clock::time_point now) const {
    if (!button_held) return false;
    auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - press_time).count();
    return duration > kMenuHoldThreshold.count();
}

int volume_after_hold_step(int volume, int delta, bool vertical_axis) {
    // Invert delta for vertical axis (Up = -1 -> Volume Up)
    int vol_change = (vertical_axis ? -delta : delta) * kHoldVolumeStepPercent;
    volume += vol_change;

    // Clamp volume
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    return volume;
}

GameListSelect classify_game_list_select(int game_idx, int game_count) {
    // Check if "Back" button is selected (last item)
    if (game_idx == game_count) return GameListSelect::Back;
    if (game_idx >= 0 && game_idx < game_count) return GameListSelect::Launch;
    return GameListSelect::Invalid;
}

GameBrowserSelect classify_game_browser_select(int selected, int playlist_count) {
    // Check if "Back" button is selected (last item)
    if (selected == playlist_count) return GameBrowserSelect::ExitBrowser;
    if (selected >= 0 && selected < playlist_count) return GameBrowserSelect::EnterPlaylist;
    return GameBrowserSelect::Nothing;
}

}  // namespace app
