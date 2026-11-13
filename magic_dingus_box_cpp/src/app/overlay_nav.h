#pragma once

// The list overlay's cursor: which playlist row is highlighted, where the
// scroll window over the rows starts, and whether the cursor has moved off
// the list onto the Picture (crop) control.
//
// The control sits one stop below the last playlist row — it is drawn in
// the now-playing block, not in the list — and exists only while a playlist
// video whose display aspect is not 4:3 plays (main.cpp decides that each
// frame into AppState::picture_control_available). Turning down from the
// last row moves onto it, turning up from it returns to the last row, and a
// further turn down does nothing. While the cursor is on it, `selected`
// stays the last row, so leaving it lands there. Design:
// docs/superpowers/specs/2026-11-12-video-crop-toggle-design.md, "Overlay".
//
// Without the control this is main.cpp's old inline ROTATE handler: one row
// per turn whatever the turn's size, clamped at both ends, the window moved
// only as far as keeps the cursor on screen and never past the list's end.
//
// Pure: no I/O, unit-tested on the Mac (tests/app/test_overlay_nav.cpp).

#include "app_state.h"

namespace app {

struct OverlayCursor {
    int selected = 0;
    int scroll = 0;
    bool on_picture = false;
};

// One turn of the encoder (or D-pad / keyboard / phone up-down): `delta` > 0
// is down, < 0 is up, 0 moves nothing (only the window is repaired).
// `rows` is the number of playlist rows, `max_visible` how many are on
// screen (< 1 counts as 1), `picture_available` whether the Picture control
// is offered.
OverlayCursor rotate_overlay(OverlayCursor c, int delta, int rows, int max_visible,
                             bool picture_available);

// Repairs a cursor after something other than a turn changed its world: the
// control went away (the next video is 4:3, playback stopped) — the cursor
// falls back to the last playlist row — or the rows changed under it (a
// playlist reload). On the control, `selected` is moved to the last row.
// Then the window is put back around the cursor and inside the list. A
// consistent cursor without the control comes back unchanged.
OverlayCursor clamp_overlay(OverlayCursor c, int rows, int max_visible,
                            bool picture_available);

// AppState's copy of the cursor: selected_index, playlist_scroll_offset,
// picture_focus.
OverlayCursor overlay_cursor(const AppState& state);
void set_overlay_cursor(AppState& state, const OverlayCursor& c);

}  // namespace app
