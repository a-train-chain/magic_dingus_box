#include "theme.h"

namespace ui {

Theme::Theme()
    : bg(31, 25, 31)              // #1F191F
    , fg(242, 228, 217)           // #F2E4D9
    , highlight1(102, 221, 122)    // #66DD7A
    , highlight2(234, 58, 39)      // #EA3A27
    , highlight3(245, 191, 66)     // #F5BF42
    , action(88, 132, 177)        // #5884B1
    , accent(highlight3)           // Alias
    , accent2(action)              // Alias
    , dim(150, 140, 135)           // ~60% of fg
    , font_title_size(32)
    , font_heading_size(22)
    , font_large_size(24)
    , font_medium_size(18)
    , font_small_size(14)
    , margin_x(16)
    , margin_x_bezel(60)
    , title_y(8)
    , header_y(8 + 32 + 24)  // title_y + title_height + spacing
    , playlist_item_height(36)
    , footer_y(0)  // Will be set based on screen height
{
}

} // namespace ui

