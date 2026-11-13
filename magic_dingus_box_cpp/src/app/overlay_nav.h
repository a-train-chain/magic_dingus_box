#pragma once

// The list overlay's cursor: which playlist row is highlighted, where the
// scroll window over the rows starts, and whether the cursor has moved off
// the list onto the Picture (crop) control.
//
// The control sits one stop below the last playlist row — it is drawn in
// the now-playing block, not in the list — and is shown while a playlist
// video whose display aspect is not 4:3 plays under the visible overlay
// (picture_control_visible below; main.cpp stores it in
// AppState::picture_control_available). Turning down from the last row moves
// onto it, turning up from it returns to the last row, and a further turn
// down does nothing. While the cursor is on it, `selected` stays the last
// row, so leaving it lands there. Design:
// docs/superpowers/specs/2026-11-12-video-crop-toggle-design.md, "Overlay".
//
// Without the control this is main.cpp's old inline ROTATE handler: one row
// per turn whatever the turn's size, clamped at both ends, the window moved
// only as far as keeps the cursor on screen and never past the list's end.
//
// Pure: no I/O, unit-tested on the Mac (tests/app/test_overlay_nav.cpp).

#include <optional>

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
// is there for this cursor (main.cpp: cursor_may_use_picture).
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

// ── Is the control there? ────────────────────────────────────────────────
// What main.cpp knows about the playing video, once per frame.
struct PictureFacts {
    // A main-menu playlist item owns the shared pipeline
    // (PlaylistPlayback::owns_pipeline): not the intro, a game, a playlist
    // switch or the Media Browser.
    bool owns_pipeline = false;
    // The loaded video has a media-info key (Controller::current_media_key
    // is not empty; it is cleared by a failed load).
    bool has_key = false;
    // Its display aspect: the one the web admin recorded, else the last
    // decoded frame's. nullopt = not known yet.
    std::optional<double> aspect;
    // AppState::video_active.
    bool video_active = false;
    // The overlay is up over the video (AppState::ui_visible_when_playing —
    // the now-playing block it lives in draws only then).
    bool overlay_visible = false;
};

// The cursor may stay on the control: the item still owns the pipeline and
// is not known to be 4:3. Deliberately blind to video_active and to an
// unknown aspect: loading the next item (auto-advance, NEXT/PREV) resets
// the player and video_active drops for the frames until the new stream
// prerolls, and the focus must ride through that. It returns to the last
// row only when the NEW video turns out to have no control — known 4:3, a
// failed load (no key), or the pipeline handed to something else.
bool picture_focus_may_stay(const PictureFacts& f);

// The control is drawn, and a turn may move onto it: the focus could stay,
// a video is actually playing, its aspect is known and not 4:3, and the
// overlay is up.
bool picture_control_visible(const PictureFacts& f);

// What rotate_overlay / clamp_overlay should be told between main.cpp's
// per-frame decisions: the control is shown, or the cursor is already on
// it. main.cpp's per-frame clamp (with picture_focus_may_stay) has already
// dropped a focus that may not stay, so a focus still set is one that may —
// even in a load gap, where the control is momentarily not drawn.
bool cursor_may_use_picture(const AppState& state);

}  // namespace app
