#include "overlay_nav.h"

#include "../video/picture_placement.h"

#include <algorithm>

namespace app {

OverlayCursor rotate_overlay(OverlayCursor c, int delta, int rows, int max_visible,
                             bool picture_available) {
    if (delta > 0) {
        // Down: the next row; from the last row, the Picture control when it
        // is offered; on the control, nothing (clamped).
        if (c.on_picture) {
            // already as far down as it goes
        } else if (c.selected < rows - 1) {
            c.selected++;
        } else if (picture_available && rows > 0) {
            c.on_picture = true;
        }
    } else if (delta < 0) {
        // Up: off the control onto the last row (where `selected` already
        // is), else the previous row, clamped at the first.
        if (c.on_picture) {
            c.on_picture = false;
        } else if (c.selected > 0) {
            c.selected--;
        }
    }
    return clamp_overlay(c, rows, max_visible, picture_available);
}

OverlayCursor clamp_overlay(OverlayCursor c, int rows, int max_visible,
                            bool picture_available) {
    if (c.on_picture) {
        // On the control, or falling back from it: the last playlist row.
        if (rows > 0) c.selected = rows - 1;
        if (rows <= 0 || !picture_available) c.on_picture = false;
    }

    // Keep the cursor's row on screen, and the window inside the list.
    const int visible = std::max(1, max_visible);
    if (c.selected >= c.scroll + visible) c.scroll = c.selected - visible + 1;
    if (c.selected < c.scroll) c.scroll = c.selected;
    const int max_scroll = std::max(0, rows - visible);
    if (c.scroll > max_scroll) c.scroll = max_scroll;
    if (c.scroll < 0) c.scroll = 0;
    return c;
}

OverlayCursor overlay_cursor(const AppState& state) {
    OverlayCursor c;
    c.selected = state.selected_index;
    c.scroll = state.playlist_scroll_offset;
    c.on_picture = state.picture_focus;
    return c;
}

void set_overlay_cursor(AppState& state, const OverlayCursor& c) {
    state.selected_index = c.selected;
    state.playlist_scroll_offset = c.scroll;
    state.picture_focus = c.on_picture;
}

bool picture_focus_may_stay(const PictureFacts& f) {
    const bool known_four_by_three =
        f.aspect.has_value() && video::is_four_by_three(*f.aspect);
    return f.owns_pipeline && f.has_key && !known_four_by_three;
}

bool picture_control_visible(const PictureFacts& f) {
    return picture_focus_may_stay(f) && f.video_active && f.overlay_visible &&
           f.aspect.has_value();
}

bool cursor_may_use_picture(const AppState& state) {
    return state.picture_control_available || state.picture_focus;
}

}  // namespace app
