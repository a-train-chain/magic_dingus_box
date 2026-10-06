#pragma once

// Pure decisions behind the kiosk's Settings-menu input dispatch
// (app/settings_input.h, kiosk-only). Moved out of main.cpp's event loop
// so they can be unit-tested on the Mac (tests/app/test_settings_input_logic.cpp).

#include <chrono>

namespace app {

// The BTN4 ("Menu", InputAction::SETTINGS_MENU) hold. A short press toggles
// the Settings menu; holding it turns the rotary / vertical axis into a
// master-volume control, with the slider shown once the hold passes
// kMenuHoldThreshold (or at once on the first volume change).
inline constexpr std::chrono::milliseconds kMenuHoldThreshold{300};
// Master volume change per rotary detent / axis step while BTN4 is held.
inline constexpr int kHoldVolumeStepPercent = 5;

struct MenuButtonHold {
    using Clock = std::chrono::steady_clock;

    bool button_held = false;
    bool volume_changed_while_held = false;
    Clock::time_point press_time;

    // What a BTN4 release asks for.
    enum class Release {
        ToggleSettings,  // short press with no volume change
        SaveVolume,      // the hold changed the volume: persist it now
        Nothing,         // a long press that changed nothing
    };

    void press(Clock::time_point now);
    // Ends the hold. ToggleSettings needs BOTH no volume change and a hold
    // shorter than kMenuHoldThreshold; any volume change saves, however
    // long the hold.
    Release release(Clock::time_point now);
    // The volume slider is due: held for MORE than kMenuHoldThreshold.
    bool slider_due(Clock::time_point now) const;
};

// Master volume after one held-BTN4 step: ROTATE turns up for delta > 0;
// ROTATE_VERTICAL is inverted (up = delta -1 = louder). kHoldVolumeStepPercent
// per step, clamped to [0, 100].
int volume_after_hold_step(int volume, int delta, bool vertical_axis);

// SELECT inside an open game list. The row after the last game is "Back".
enum class GameListSelect { Back, Launch, Invalid };
GameListSelect classify_game_list_select(int game_idx, int game_count);

// SELECT on the game browser's playlist list. The row after the last
// playlist is "Back" (exit the browser); out-of-range rows do nothing.
enum class GameBrowserSelect { ExitBrowser, EnterPlaylist, Nothing };
GameBrowserSelect classify_game_browser_select(int selected, int playlist_count);

}  // namespace app
