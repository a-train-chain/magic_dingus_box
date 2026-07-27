#pragma once

#include "app_state.h"
#include "../utils/result.h"
#include <string>

namespace app {

class SettingsPersistence {
public:
    // Save current settings to file
    // Returns Result with error message on failure
    static utils::Result<> save_settings(const AppState& state);

    // Load settings from file into state
    // Returns Result with error message on failure
    static utils::Result<> load_settings(AppState& state);

    // Read ONLY display.mode from settings.json, without touching
    // AppState. The DRM mode has to be chosen before the display is
    // initialised — hundreds of lines before load_settings() runs — and
    // MODERN_TV now boots 1920x1080 while CRT_NATIVE must stay at 720p
    // (no composite out on Pi 5; CRT rigs go through an HDMI->composite
    // converter that CRT_MAX_HEIGHT protects). Returns true for
    // CRT_NATIVE, which is also the safe default when the file is
    // missing or unreadable: an unprovisioned box must never come up
    // driving a converter at a resolution it may not accept.
    static bool peek_is_crt_native();

private:
    static std::string get_settings_path();

};

} // namespace app
