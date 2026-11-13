#pragma once

// The Channel Guide's cursor and input rules.
//
// The cursor: which channel row is highlighted, where the grid's 5-row
// window starts, and whether the cursor has moved off the grid onto the
// Picture (crop) control. The control sits one stop ABOVE channel 00 — it is
// drawn in the now-playing panel, which is above the grid in layout A — and
// is there while a non-4:3 playlist video plays under the visible guide
// (main.cpp's AppState::picture_control_available, the same rule as the
// List style's control, app/overlay_nav.h). Turning up from row 0 moves onto
// it, turning down from it returns to row 0, and a further turn up does
// nothing. While the cursor is on it, `row` stays 0. One row per turn
// whatever the turn's size, clamped at both ends (channel up/down wraps —
// PlaylistPlayback::on_channel_step — the knob does not); the window moves
// only as far as keeps the cursor row on screen and never past the last
// channel. Design: docs/superpowers/specs/2026-11-12-channel-guide-design.md,
// "Controls".
//
// The input rules (guide_input / guide_tune_taken / guide_frame, below):
// main.cpp gathers what it knows (GuideFacts, GuideFrameFacts), these write
// the guide's state in AppState and hand back the calls main.cpp makes.
//
// Pure: no I/O. Unit-tested on the Mac (tests/app/test_guide_nav.cpp).
// app_state.h includes this header (AppState holds a GuideCursor and a
// GuideShownEdge), so AppState is only forward-declared here.

#include <chrono>
#include <optional>
#include <string>

namespace app {

struct AppState;

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

// What a press (encoder press / SELECT) on the cursor means.
enum class GuidePress {
    Tune,           // another channel's row: tune it (and close the guide)
    Close,          // the tuned channel's row: close the guide
    TogglePicture,  // the Picture control: flip crop; the guide stays open
    None,           // a channel with no schedule: nothing
};

// `tuned` is the channel on screen — or the one a pending tune is loading —
// (-1 none); `row_tunable` whether the cursor's row has a schedule.
// guide_input turns Close into a retry while nothing is playing.
GuidePress guide_press(const GuideCursor& c, int tuned, bool row_tunable);

// The frame the guide appears on screen; fed every frame by guide_frame.
struct GuideShownEdge {
    bool was_shown = false;
    bool rose(bool shown) {
        const bool appeared = shown && !was_shown;
        was_shown = shown;
        return appeared;
    }
};

// ── Input rules ──────────────────────────────────────────────────────────
// The guide's state lives in AppState (render thread only):
//   guide_cursor            the cursor above;
//   guide_opened_by_viewer  the viewer owns the guide: SELECT opened it, or
//                           the shown guide got any other input. A tune's
//                           load never hides a guide the viewer owns when it
//                           runs (PlaylistPlayback) — the guide never closes
//                           by itself;
//   guide_viewer_tune       the row of a viewer's Tune press whose load has
//                           not run (-1 none): during that wait the knob and
//                           NEXT/PREV neither take ownership nor move the
//                           cursor off it, so the tune still closes the
//                           guide;
//   guide_appeared          the on-screen edge (guide_frame).

// The guide is on screen, in Channel Guide style: the overlay is up
// (ui_visible_when_playing), or nothing is playing or loading — the menu is
// drawn whenever there is no video — and the Media Browser does not own the
// screen. Not in the load gap of a tune or an item boundary (the playing
// indexes set while video_active is false) with the overlay hidden: nothing
// is drawn there, so SELECT opens the guide rather than pressing an
// invisible row. Input and the renderer use this one definition.
bool guide_visible(const AppState& s);

// The renderer paints the guide this frame, in Channel Guide style — and
// the tuned video is drawn in the guide's window rather than over the whole
// picture for exactly as long: while the guide is on screen (guide_visible —
// the load gap of a tune with the overlay up included, where the List style
// draws nothing), and through a Close's fade-out, when the overlay is
// already hidden but the guide is still fading (the video stays in its
// window, the full picture comes back when the fade ends: main.cpp clears
// is_fading once the fade's time is up, before it reads this). A fade-in
// (SELECT opening it) is on screen at once. Never over the intro (before
// its first frame nothing is "playing", which guide_visible would count) or
// the Media Browser.
bool guide_painted(const AppState& s);

// Saving the channel last tuned (AppState::last_channel_id, persisted as
// playback.last_channel) when it changes. The id is compared only on a
// frame where tuned_channel moved — a tune's load ran, or a rebuild
// re-anchored it — never every frame (a string compare per frame for a
// change that comes with a tune). Seed both from the loaded settings.
struct LastChannelWatch {
    int seen_tuned = -1;
    std::string saved_id;
    // True (once) when last_channel_id differs from the value last saved, as
    // seen on a tuned_channel move: the caller saves the settings.
    bool changed(const AppState& s);
};

enum class GuideEvent {
    Rotate,       // encoder turn / D-pad up-down (delta)
    Select,       // encoder press / SELECT (press edge)
    ChannelStep,  // NEXT / PREV (press edge), after on_next() / on_prev() ran
};

// What main.cpp knows when an input is decided, besides AppState.
struct GuideFacts {
    // guide_visible(state) in guide style when the event arrived —
    // ChannelStep: BEFORE on_next() / on_prev() ran (a step out of nothing
    // playing makes the guide's state "loading").
    bool on_screen = false;
    int rows = 0;  // channels
    // PlaylistPlayback::pending_tune() — ChannelStep: AFTER the step.
    std::optional<int> pending_tune;
    bool cursor_row_tunable = false;  // the cursor's row has a schedule
    bool owns_pipeline = false;       // PlaylistPlayback::owns_pipeline()
    std::chrono::steady_clock::time_point now{};  // a Close's fade start
};

// The calls main.cpp makes for an input.
struct GuideCommand {
    // PlaylistPlayback::tune(tune, /*keep_overlay=*/false), then
    // guide_tune_taken(state, tune, pending_tune() == tune). -1: none.
    int tune = -1;
    bool toggle_picture = false;  // the transport's toggle_picture_crop()
};

// One input in Channel Guide style (the intro ignores SELECT, as on_select):
//   Rotate       shown: the cursor moves and the guide is the viewer's.
//                Hidden: nothing (main.cpp seeks, as in the List style).
//   Select       hidden: open on the tuned channel (pending tune first),
//                the viewer's, a Close's fade-out cancelled. Shown:
//                guide_press — Tune: ownership dropped (a viewer's tune
//                closes the guide at its load); Close: hidden, with
//                today's fade when a video plays; but with nothing playing
//                or loading (a failed item's dark window) the tuned row's
//                press RETRIES the channel (Tune; None without a
//                schedule); TogglePicture (when owns_pipeline) and None:
//                the viewer's.
//   ChannelStep  shown: the viewer's, the cursor onto the pending tune.
// Rotate and ChannelStep during a viewer's Tune wait (guide_viewer_tune is
// the pending tune) do nothing to ownership or the cursor. Taking
// ownership also raises ui_visible_when_playing: with nothing playing the
// guide is drawn without it, and would vanish when the video came back.
GuideCommand guide_input(AppState& s, GuideEvent e, int delta, const GuideFacts& f);

// After main.cpp's tune(row, false) for a GuideCommand: taken (it is now
// the pending tune) — its wait begins (guide_viewer_tune); dropped (a
// switch in flight) — an input like any other, so the guide is the
// viewer's.
void guide_tune_taken(AppState& s, int row, bool taken);

// What main.cpp knows once per frame, besides AppState.
struct GuideFrameFacts {
    bool guide_style = false;  // PlaylistPlayback::guide_style()
    int rows = 0;
    std::optional<int> pending_tune;
    // picture_focus_may_stay() for this frame's video (app/overlay_nav.h).
    bool picture_may_stay = false;
};

// Once per frame, after the loads and picture_control_available:
//   - outside guide style, or with the overlay hidden, the viewer does not
//     own the guide; a viewer's Tune wait ends once its tune is no longer
//     pending (loaded or abandoned);
//   - on the frame guide_visible() turns true in guide style (the edge),
//     unless the viewer owns the guide by then (SELECT opened it), the
//     cursor goes to the tuned row (pending tune first). It turns true when
//     the overlay is raised over a video, or when nothing is left playing or
//     loading: a failed item's stop_to_menu, the Media Browser closing with
//     nothing playing (never while it owns the screen). Settings over the
//     guide does not hide it here.
//   - the Picture control is offered only while a channel is tuned (a
//     pending tune first): the guide draws it on the tuned channel's panel
//     and nowhere else, so with nothing tuned (a List video still playing
//     on entering guide style) picture_control_available is cleared and the
//     cursor cannot land on a control that is not drawn;
//   - the cursor is clamped: on the Picture control it rides through the
//     load gap between items while picture_may_stay, as the List's focus
//     does; off it, the control counts only while drawn. The List's
//     picture_focus is never set in guide style.
// Outside guide style the cursor and picture_focus are left alone; the edge
// is fed "not on screen".
void guide_frame(AppState& s, const GuideFrameFacts& f);

// Entering Channel Guide style (Settings -> Display -> Menu Style, or a
// settings restore) with a List playlist playing or loading (the playing
// indexes set): the channel to tune live — its row, the same row being the
// same channel, or 00 while Master Shuffle plays — so the box is live at
// once, joined at the scheduled point, rather than after the List item ends.
// nullopt with nothing playing or loading (the nothing-playing rule tunes
// then) and while the Media Browser owns the screen.
std::optional<int> guide_entry_tune(const AppState& s);

}  // namespace app
