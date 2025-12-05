#include "settings_menu.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>  // For std::max

#include "../app/app_state.h"
#include "../app/settings_persistence.h"

namespace ui {

SettingsMenuManager::SettingsMenuManager(app::AppState* state)
    : app_state_(state)
    , active_(false)
    , selected_index_(0)
    , animation_duration_(0.3f)
    , is_opening_(false)
    , is_closing_(false)
    , scroll_offset_(0)
    , current_submenu_(MenuSection::BACK)
    , game_browser_active_(false)
    , game_browser_selected_(0)
    , viewing_games_in_playlist_(false)
    , current_game_playlist_index_(0)
    , selected_game_in_playlist_(0)
{
    // Main menu items
    menu_items_ = {
        MenuItem("Video Games", MenuSection::VIDEO_GAMES, "Emulated games"),
        MenuItem("Display", MenuSection::DISPLAY, "Screen settings"),
        MenuItem("Audio", MenuSection::AUDIO, "Volume"),
        MenuItem("System", MenuSection::SYSTEM, "Settings"),
        MenuItem("Info", MenuSection::INFO, "Stats"),
        MenuItem("Back", MenuSection::BACK)
    };
}

// ... (keep existing methods until build_display_submenu)

std::vector<MenuItem> SettingsMenuManager::build_display_submenu() {
    if (!app_state_) {
        return {
            MenuItem("Error: No AppState", MenuSection::BACK),
            MenuItem("Back", MenuSection::BACK)
        };
    }

    auto& settings = app_state_->display_settings;

    return {
        MenuItem("Mode: CRT Native", MenuSection::TOGGLE_DISPLAY_MODE, "Cycle modes"),
        
        MenuItem("Scanlines: " + intensity_to_label(settings.scanline_intensity), 
                 MenuSection::CYCLE_SCANLINES, "CRT lines", 
                 [&]() { 
                     settings.cycle_setting(settings.scanline_intensity); 
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_); 
                 }),
                 
        MenuItem("Color Warmth: " + intensity_to_label(settings.warmth_intensity), 
                 MenuSection::CYCLE_WARMTH, "Temperature",
                 [&]() { 
                     settings.cycle_setting(settings.warmth_intensity); 
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_); 
                 }),
                 
        MenuItem("Phosphor Glow: " + intensity_to_label(settings.glow_intensity), 
                 MenuSection::CYCLE_GLOW, "Radial glow",
                 [&]() { 
                     settings.cycle_setting(settings.glow_intensity); 
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_); 
                 }),
                 
        MenuItem("RGB Mask: " + intensity_to_label(settings.rgb_mask_intensity), 
                 MenuSection::CYCLE_PHOSPHOR_MASK, "RGB stripes",
                 [&]() { 
                     settings.cycle_setting(settings.rgb_mask_intensity); 
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_); 
                 }),
                 
        MenuItem("Screen Bloom: " + intensity_to_label(settings.bloom_intensity), 
                 MenuSection::CYCLE_BLOOM, "Bright glow",
                 [&]() { 
                     settings.cycle_setting(settings.bloom_intensity); 
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_); 
                 }),
                 
        MenuItem("Interlacing: " + intensity_to_label(settings.interlacing_intensity), 
                 MenuSection::CYCLE_INTERLACING, "Video lines",
                 [&]() { 
                     settings.cycle_setting(settings.interlacing_intensity); 
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_); 
                 }),
                 
        MenuItem("Flicker: " + intensity_to_label(settings.flicker_intensity), 
                 MenuSection::CYCLE_FLICKER, "Subtle pulse",
                 [&]() { 
                     settings.cycle_setting(settings.flicker_intensity); 
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_); 
                 }),
                 
        MenuItem("Back", MenuSection::BACK)
    };
}

void SettingsMenuManager::toggle() {
    if (active_ || is_opening_) {
        close();
    } else {
        open();
    }
}

void SettingsMenuManager::open() {
    if (!active_ && !is_opening_) {
        active_ = true;
        is_opening_ = true;
        is_closing_ = false;
        animation_start_ = std::chrono::steady_clock::now() - std::chrono::milliseconds(50);
        selected_index_ = 0;
        scroll_offset_ = 0;
        current_submenu_ = MenuSection::BACK;
    }
}

void SettingsMenuManager::close() {
    if (active_ || is_opening_) {
        is_closing_ = true;
        is_opening_ = false;
        animation_start_ = std::chrono::steady_clock::now();
    }
}

float SettingsMenuManager::get_animation_progress() const {
    if (!active_ && !is_closing_) {
        return 0.0f;
    }
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<float>(now - animation_start_).count();
    
    if (is_closing_) {
        float progress = 1.0f - (elapsed / animation_duration_);
        if (progress <= 0.0f) {
            const_cast<SettingsMenuManager*>(this)->active_ = false;
            const_cast<SettingsMenuManager*>(this)->is_closing_ = false;
            return 0.0f;
        }
        return progress;
    } else {
        float progress = elapsed / animation_duration_;
        if (progress >= 1.0f) {
            const_cast<SettingsMenuManager*>(this)->is_opening_ = false;
            return 1.0f;
        }
        return progress;
    }
}

void SettingsMenuManager::navigate(int delta, int game_playlists_count, int games_in_current_playlist) {
    if (!active_) return;
    
    // Handle game browser navigation
    if (game_browser_active_) {
        if (viewing_games_in_playlist_) {
            // Navigating games within a playlist
            // Range is [0, games_in_current_playlist], where games_in_current_playlist is "Back"
            selected_game_in_playlist_ += delta;
            if (selected_game_in_playlist_ < 0) {
                selected_game_in_playlist_ = 0;
            } else if (selected_game_in_playlist_ > games_in_current_playlist) {
                selected_game_in_playlist_ = games_in_current_playlist;
            }
        } else {
            // Navigating game playlists
            // Range is [0, game_playlists_count], where game_playlists_count is "Back"
            game_browser_selected_ += delta;
            if (game_browser_selected_ < 0) {
                game_browser_selected_ = 0;
            } else if (game_browser_selected_ > game_playlists_count) {
                game_browser_selected_ = game_playlists_count;
            }
        }
        return;
    }
    
    // Normal menu navigation
    const std::vector<MenuItem>& items = current_submenu_ == MenuSection::BACK ? menu_items_ : submenu_items_;
    int max_items = static_cast<int>(items.size());
    
    // Safety check: ensure items vector is not empty
    if (max_items == 0) {
        return;
    }
    
    selected_index_ += delta;
    if (selected_index_ < 0) {
        selected_index_ = 0;
    } else if (selected_index_ >= max_items) {
        selected_index_ = max_items - 1;
    }
    
    // Scrolling logic
    const int max_visible_items = 7;
    if (selected_index_ < scroll_offset_) {
        scroll_offset_ = selected_index_;
    } else if (selected_index_ >= scroll_offset_ + max_visible_items) {
        scroll_offset_ = selected_index_ - max_visible_items + 1;
    }
    
    // Ensure scroll_offset is valid
    if (scroll_offset_ < 0) {
        scroll_offset_ = 0;
    }
    if (scroll_offset_ >= max_items) {
        scroll_offset_ = std::max(0, max_items - 1);
    }
}

MenuSection SettingsMenuManager::select_current() {
    if (!active_) {
        return MenuSection::BACK;
    }
    
    const std::vector<MenuItem>& items = current_submenu_ == MenuSection::BACK ? menu_items_ : submenu_items_;
    if (selected_index_ >= 0 && selected_index_ < static_cast<int>(items.size())) {
        const MenuItem& item = items[selected_index_];
        if (item.action) {
            item.action();
        }
        return item.section;
    }
    return MenuSection::BACK;
}

void SettingsMenuManager::enter_submenu(MenuSection section) {
    current_submenu_ = section;
    selected_index_ = 0;
    scroll_offset_ = 0;
    
    if (section == MenuSection::VIDEO_GAMES) {
        submenu_items_ = build_games_submenu();
    } else if (section == MenuSection::DISPLAY) {
        submenu_items_ = build_display_submenu();
    } else if (section == MenuSection::AUDIO) {
        submenu_items_ = build_audio_submenu();
    } else if (section == MenuSection::SYSTEM) {
        submenu_items_ = build_system_submenu();
    } else if (section == MenuSection::INFO) {
        submenu_items_ = build_info_submenu();
    }
}

void SettingsMenuManager::exit_submenu() {
    current_submenu_ = MenuSection::BACK;
    selected_index_ = 0;
    scroll_offset_ = 0;
    submenu_items_.clear();
}

void SettingsMenuManager::rebuild_current_submenu() {
    if (current_submenu_ == MenuSection::BACK) {
        return;
    }
    
    int old_index = selected_index_;
    enter_submenu(current_submenu_);
    selected_index_ = std::min(old_index, static_cast<int>(submenu_items_.size() - 1));
}

void SettingsMenuManager::enter_game_browser() {
    game_browser_active_ = true;
    game_browser_selected_ = 0;
}

void SettingsMenuManager::exit_game_browser() {
    game_browser_active_ = false;
    game_browser_selected_ = 0;
    viewing_games_in_playlist_ = false;
    current_game_playlist_index_ = 0;
    selected_game_in_playlist_ = 0;
}

void SettingsMenuManager::enter_game_list(int playlist_index) {
    viewing_games_in_playlist_ = true;
    current_game_playlist_index_ = playlist_index;
    selected_game_in_playlist_ = 0;
}

void SettingsMenuManager::exit_game_list() {
    viewing_games_in_playlist_ = false;
    selected_game_in_playlist_ = 0;
}

std::vector<MenuItem> SettingsMenuManager::build_games_submenu() {
    return {
        MenuItem("Browse Games", MenuSection::BROWSE_GAMES, "Game libraries"),
        MenuItem("Download Cores", MenuSection::DOWNLOAD_CORES, "RetroArch cores"),
        MenuItem("Emulators", MenuSection::BACK, "RetroArch"),
        MenuItem("Controllers", MenuSection::BACK, "Button map"),
        MenuItem("Back", MenuSection::BACK)
    };
}



std::vector<MenuItem> SettingsMenuManager::build_audio_submenu() {
    return {
        MenuItem("Menu Vol: 75%", MenuSection::BACK, "Browsing"),
        MenuItem("Video Vol: 100%", MenuSection::BACK, "Playback"),
        MenuItem("Fade: 1.0s", MenuSection::BACK, "Transitions"),
        MenuItem("Back", MenuSection::BACK)
    };
}

std::vector<MenuItem> SettingsMenuManager::build_system_submenu() {
    if (!app_state_) {
        return {
            MenuItem("Error: No AppState", MenuSection::BACK),
            MenuItem("Back", MenuSection::BACK)
        };
    }

    std::string loop_status = app_state_->playlist_loop ? "ON" : "OFF";
    std::string shuffle_status = app_state_->shuffle ? "ON" : "OFF";
    
    return {
        MenuItem("Playlist Loop: " + loop_status, 
                 MenuSection::TOGGLE_PLAYLIST_LOOP, "Auto-restart",
                 [&]() { 
                     app_state_->playlist_loop = !app_state_->playlist_loop; 
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_); 
                 }),
        MenuItem("Shuffle: " + shuffle_status, 
                 MenuSection::TOGGLE_SHUFFLE, "Random order",
                 [&]() { 
                     app_state_->shuffle = !app_state_->shuffle; 
                     rebuild_current_submenu();
                     app::SettingsPersistence::save_settings(*app_state_); 
                 }),
        MenuItem("Back", MenuSection::BACK)
    };
}

std::vector<MenuItem> SettingsMenuManager::build_info_submenu() {
    return {
        MenuItem("Version: 1.0.0", MenuSection::BACK, "Magic Dingus Box"),
        MenuItem("Platform: Pi", MenuSection::BACK, "Hardware"),
        MenuItem("Uptime: 2h 34m", MenuSection::BACK, "Runtime"),
        MenuItem("Back", MenuSection::BACK)
    };
}

std::string SettingsMenuManager::intensity_to_label(float intensity) {
    if (intensity <= 0.0f) {
        return "OFF";
    } else if (intensity <= 0.35f) {
        return "Low (" + std::to_string(static_cast<int>(intensity * 100)) + "%)";
    } else if (intensity <= 0.6f) {
        return "Medium (" + std::to_string(static_cast<int>(intensity * 100)) + "%)";
    } else {
        return "High (" + std::to_string(static_cast<int>(intensity * 100)) + "%)";
    }
}

bool SettingsMenuManager::is_game_browser_back_selected() const {
    if (!game_browser_active_) return false;
    
    // This relies on the caller providing the correct count context, 
    // but since we don't have the counts stored here, we can't strictly verify against max.
    // However, the navigation logic ensures we don't go out of bounds.
    // The "Back" button is always the last item.
    // We'll need to rely on the renderer and input handler to know what the max index is.
    // Actually, we can't easily implement this without storing the counts.
    // Let's change the approach: we'll just check if the index matches the "Back" button index
    // which is handled by the caller (renderer/main) knowing the count.
    // But wait, we need to know if we are selecting "Back" for rendering.
    
    // Alternative: We don't strictly need this helper if we pass the counts to the renderer.
    // But for consistency, let's implement it by checking if we are at the end.
    // Since we don't store the counts, we can't implement this perfectly here without adding state.
    // Let's skip implementing logic here and handle it in renderer/main where counts are known.
    // OR, better: Update the class to store the counts when entering/navigating.
    // For now, let's just return false and handle the check externally where counts are available.
    // Actually, let's implement it properly by adding the counts to the class state if needed, 
    // but for minimal impact, let's just return false and I'll handle the logic in renderer/main 
    // by comparing index == count.
    return false; 
}

} // namespace ui

