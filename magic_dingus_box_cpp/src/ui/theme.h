#pragma once

#include <cstdint>
#include <string>

namespace ui {

struct Color {
    uint8_t r, g, b, a;
    
    Color() : r(0), g(0), b(0), a(255) {}
    Color(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) : r(r), g(g), b(b), a(a) {}
};

class Theme {
public:
    Theme();
    
    // Colors (matching Python Theme class)
    Color bg;           // Background: #1F191F
    Color fg;           // Text: #F2E4D9
    Color highlight1;   // Green: #66DD7A
    Color highlight2;   // Red/orange: #EA3A27
    Color highlight3;   // Gold: #F5BF42
    Color action;       // Steel blue: #5884B1
    Color accent;       // Alias for highlight3
    Color accent2;      // Alias for action
    Color dim;          // Dim text: ~60% of fg
    
    // Font sizes (matching Python Theme)
    int font_title_size;
    int font_heading_size;
    int font_large_size;
    int font_medium_size;
    int font_small_size;
    
    // Layout constants (matching Python UIRenderer)
    int margin_x;
    int margin_x_bezel;
    int title_y;
    int header_y;
    int playlist_item_height;
    int footer_y;
    
    // Get font path (will be set by font_manager)
    std::string get_font_path() const { return font_path_; }
    void set_font_path(const std::string& path) { font_path_ = path; }

private:
    std::string font_path_;
};

} // namespace ui

