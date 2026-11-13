#pragma once

// The Channel Guide's cursor: which channel row is highlighted, where the
// grid's 5-row window starts, and whether the cursor has moved off the grid
// onto the Picture (crop) control.
//
// The control sits one stop ABOVE channel 00 — it is drawn in the
// now-playing panel, which is above the grid in layout A — and is there
// while a non-4:3 playlist video plays under the visible guide (main.cpp's
// AppState::picture_control_available, the same rule as the List style's
// control, app/overlay_nav.h). Turning up from row 0 moves onto it, turning
// down from it returns to row 0, and a further turn up does nothing. While
// the cursor is on it, `row` stays 0. Design:
// docs/superpowers/specs/2026-11-12-channel-guide-design.md, "Controls".
//
// One row per turn whatever the turn's size, clamped at both ends (channel
// up/down wraps — PlaylistPlayback::on_channel_step — the knob does not);
// the window moves only as far as keeps the cursor row on screen and never
// past the last channel.
//
// Pure: no I/O and no AppState, so app_state.h can hold a GuideCursor.
// Unit-tested on the Mac (tests/app/test_guide_nav.cpp).

namespace app {

struct GuideCursor {
    int row = 0;     // channel row (= UI playlist row; 00 = Master Shuffle)
    int scroll = 0;  // first channel row in the grid's window
    bool on_picture = false;
};

// Channel rows on screen at once, at 960x720 and at 640x480 alike.
constexpr int kGuideVisibleRows = 5;

// The cursor as the guide opens: on the tuned channel (row 0 when nothing
// is tuned or `tuned` is past the list), that row centred in the window as
// far as the list allows.
GuideCursor guide_open(int tuned, int rows);

// One turn of the encoder (or D-pad / keyboard / phone up-down): `delta` > 0
// is down, < 0 is up, 0 moves nothing (only the window is repaired). `rows`
// is the number of channels; `picture_available` whether the Picture control
// is there for this cursor.
GuideCursor guide_rotate(GuideCursor c, int delta, int rows, bool picture_available);

// Repairs a cursor after something other than a turn changed its world: the
// control went away (the cursor falls back to row 0) or the channel list
// changed under it (a playlist reload). Then the window is put back around
// the cursor and inside the list. A consistent cursor comes back unchanged.
GuideCursor guide_clamp(GuideCursor c, int rows, bool picture_available);

// The cursor follows a channel change (channel up/down while the guide is
// shown): onto `channel`'s row, off the Picture control, the window moved
// only as far as keeps the row on screen.
GuideCursor guide_follow(GuideCursor c, int channel, int rows);

// What a press (encoder press / SELECT) does with the guide open.
enum class GuidePress {
    Tune,           // another channel's row: tune it (and close the guide)
    Close,          // the tuned channel's row: close the guide
    TogglePicture,  // the Picture control: flip crop; the guide stays open
    None,           // a channel with no schedule: nothing
};

// `tuned` is the channel on screen — or the one a pending tune is loading —
// (-1 none); `row_tunable` whether the cursor's row has a schedule.
GuidePress guide_press(const GuideCursor& c, int tuned, bool row_tunable);

}  // namespace app
