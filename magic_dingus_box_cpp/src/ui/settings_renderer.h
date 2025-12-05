#pragma once

#include <cstdint>
#include <vector>
#include "settings_menu.h"
#include "theme.h"

namespace app {
struct Playlist;
}

namespace ui {

class FontManager;

class SettingsMenuRenderer {
public:
    SettingsMenuRenderer(uint32_t screen_width, uint32_t screen_height);
    
    void render(
        Renderer* main_renderer,
        SettingsMenuManager* menu,
        Theme* theme,
        FontManager* title_font_manager,
        FontManager* body_font_manager,
        const std::vector<app::Playlist>& game_playlists = {}
    );
    
    void set_bezel_mode(bool bezel) { bezel_mode_ = bezel; }

private:
    uint32_t screen_width_;
    uint32_t screen_height_;
    uint32_t menu_width_;
    bool bezel_mode_;
    
    void render_menu_items(
        Renderer* renderer,
        const std::vector<MenuItem>& items,
        int selected_index,
        int scroll_offset,
        float menu_x,
        float start_y,
        Theme* theme,
        FontManager* title_font_manager,
        FontManager* body_font_manager,
        const ui::Color& section_color
    );
    
    ui::Color get_section_color(SettingsMenuManager* menu, Theme* theme);
    std::string get_submenu_title(SettingsMenuManager* menu);
};

} // namespace ui

