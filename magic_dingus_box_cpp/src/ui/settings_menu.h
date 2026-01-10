#pragma once

#include <string>
#include <vector>
#include <functional>
#include <chrono>

namespace app {
    struct AppState;
}

namespace ui {

enum class MenuSection {
    VIDEO_GAMES,
    DISPLAY,
    AUDIO,
    SYSTEM,
    WIFI,
    WIFI_NETWORKS,
    INFO,
    BACK,
    BROWSE_GAMES,
    TOGGLE_PLAYLIST_LOOP,
    TOGGLE_SHUFFLE,
    TOGGLE_DISPLAY_MODE,
    TOGGLE_BEZEL,
    CHANGE_RESOLUTION,
    CYCLE_BEZEL_STYLE,
    CYCLE_BEZEL,
    CYCLE_SCANLINES,
    CYCLE_WARMTH,
    CYCLE_BLOOM,
    CYCLE_GLOW,
    CYCLE_PHOSPHOR_MASK,
    CYCLE_INTERLACING,
    CYCLE_FLICKER,
    DOWNLOAD_CORES
};

struct MenuItem {
    std::string label;
    MenuSection section;
    std::string sublabel;
    std::function<void()> action;
    
    MenuItem(const std::string& l, MenuSection s = MenuSection::BACK, 
             const std::string& sub = "", std::function<void()> a = nullptr)
        : label(l), section(s), sublabel(sub), action(a) {}
};

class SettingsMenuManager {
public:
    SettingsMenuManager(app::AppState* state = nullptr);
    
    void set_app_state(app::AppState* state) { app_state_ = state; }
    
    void update();
    void toggle();
    void open();
    void close();
    
    bool is_active() const { return active_; }
    bool is_opening() const { return is_opening_; }
    bool is_closing() const { return is_closing_; }
    
    float get_animation_progress() const;
    
    void navigate(int delta, int game_playlists_count = 0, int games_in_current_playlist = 0);
    MenuSection select_current();
    
    void enter_submenu(MenuSection section);
    void exit_submenu();
    void rebuild_current_submenu();
    
    void enter_game_browser();
    void exit_game_browser();
    void enter_game_list(int playlist_index);
    void exit_game_list();
    
    // State accessors
    int get_selected_index() const { return selected_index_; }
    int get_scroll_offset() const { return scroll_offset_; }
    MenuSection get_current_submenu() const { return current_submenu_; }
    bool is_game_browser_active() const { return game_browser_active_; }
    bool is_viewing_games_in_playlist() const { return viewing_games_in_playlist_; }
    int get_game_browser_selected() const { return game_browser_selected_; }
    int get_current_game_playlist_index() const { return current_game_playlist_index_; }
    int get_selected_game_in_playlist() const { return selected_game_in_playlist_; }
    
    // Helper to check if "Back" is selected in game browser
    bool is_game_browser_back_selected() const;
    
    const std::vector<MenuItem>& get_menu_items() const { return menu_items_; }
    const std::vector<MenuItem>& get_submenu_items() const { return submenu_items_; }

private:
    app::AppState* app_state_;
    bool active_;
    int selected_index_;
    std::chrono::steady_clock::time_point animation_start_;
    float animation_duration_;
    bool is_opening_;
    bool is_closing_;
    int scroll_offset_;
    // State for async operations
    bool was_scanning_;
    bool was_connecting_;
    
    MenuSection current_submenu_;
    std::vector<MenuItem> menu_items_;
    std::vector<MenuItem> submenu_items_;
    
    // Game browser state
    bool game_browser_active_;
    int game_browser_selected_;
    bool viewing_games_in_playlist_;
    int current_game_playlist_index_;
    int selected_game_in_playlist_;
    
    std::vector<MenuItem> build_games_submenu();
    std::vector<MenuItem> build_display_submenu();
    std::vector<MenuItem> build_audio_submenu();
    std::vector<MenuItem> build_system_submenu();
    std::vector<MenuItem> build_wifi_submenu();
    std::vector<MenuItem> build_wifi_networks_submenu();
    std::vector<MenuItem> build_info_submenu();
    std::string intensity_to_label(float intensity);
};

} // namespace ui

