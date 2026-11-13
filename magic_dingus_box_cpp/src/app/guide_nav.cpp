#include "guide_nav.h"

#include <algorithm>

namespace app {

GuideCursor guide_open(int tuned, int rows) {
    GuideCursor c;
    c.row = (tuned >= 0 && tuned < rows) ? tuned : 0;
    // Centre the tuned row (channels both sides of it in view); the clamp
    // keeps the window inside the list.
    c.scroll = c.row - kGuideVisibleRows / 2;
    return guide_clamp(c, rows, false);
}

GuideCursor guide_rotate(GuideCursor c, int delta, int rows, bool picture_available) {
    if (delta > 0) {
        // Down: off the control onto 00 (where `row` already is), else the
        // next row, clamped at the last.
        if (c.on_picture) {
            c.on_picture = false;
        } else if (c.row < rows - 1) {
            c.row++;
        }
    } else if (delta < 0) {
        // Up: the previous row; from 00, the Picture control when it is
        // offered; on the control, nothing (clamped).
        if (c.on_picture) {
            // already as far up as it goes
        } else if (c.row > 0) {
            c.row--;
        } else if (picture_available && rows > 0) {
            c.on_picture = true;
        }
    }
    return guide_clamp(c, rows, picture_available);
}

GuideCursor guide_clamp(GuideCursor c, int rows, bool picture_available) {
    if (c.on_picture) {
        // On the control, or falling back from it: channel 00.
        c.row = 0;
        if (rows <= 0 || !picture_available) c.on_picture = false;
    }
    c.row = std::max(0, std::min(c.row, rows - 1));

    // Keep the cursor row on screen, and the window inside the list.
    if (c.row >= c.scroll + kGuideVisibleRows) c.scroll = c.row - kGuideVisibleRows + 1;
    if (c.row < c.scroll) c.scroll = c.row;
    const int max_scroll = std::max(0, rows - kGuideVisibleRows);
    c.scroll = std::max(0, std::min(c.scroll, max_scroll));
    return c;
}

GuideCursor guide_follow(GuideCursor c, int channel, int rows) {
    c.row = channel;
    c.on_picture = false;
    return guide_clamp(c, rows, false);
}

GuidePress guide_press(const GuideCursor& c, int tuned, bool row_tunable) {
    if (c.on_picture) return GuidePress::TogglePicture;
    if (c.row == tuned) return GuidePress::Close;
    if (!row_tunable) return GuidePress::None;
    return GuidePress::Tune;
}

bool guide_visible(bool overlay_up, bool video_active, bool loading) {
    return overlay_up || (!video_active && !loading);
}

}  // namespace app
