#pragma once

#include "app_state.h"
#include <string>

namespace app {

class SettingsPersistence {
public:
    // Save current settings to file
    static bool save_settings(const AppState& state);
    
    // Load settings from file into state
    static bool load_settings(AppState& state);
    
private:
    static std::string get_settings_path();
};

} // namespace app
