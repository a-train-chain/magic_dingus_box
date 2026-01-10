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

private:
    static std::string get_settings_path();
};

} // namespace app
