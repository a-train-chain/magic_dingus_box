#include "guide_nav.h"

#include "app_state.h"

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

// ── Input rules ──────────────────────────────────────────────────────────

namespace {

// The playing indexes are set: a channel item is playing or loading.
bool has_playing_indexes(const AppState& s) {
    return s.current_playlist_index >= 0 && s.current_item_index >= 0;
}

// The viewer owns the guide. With nothing playing the guide is drawn
// without the overlay flag; raised here, it stays up when the video comes
// back.
void own(AppState& s) {
    s.guide_opened_by_viewer = true;
    s.ui_visible_when_playing = true;
}

// A viewer's Tune press is waiting for its load.
bool viewer_tune_waiting(const AppState& s, const GuideFacts& f) {
    return s.guide_viewer_tune >= 0 && f.pending_tune &&
           *f.pending_tune == s.guide_viewer_tune;
}

}  // namespace

bool guide_visible(const AppState& s) {
#ifdef MEDIA_BROWSER_ENABLED
    if (s.current_screen == AppScreen::MediaBrowser) return false;
#endif
    return s.ui_visible_when_playing || (!s.video_active && !has_playing_indexes(s));
}

GuideCommand guide_input(AppState& s, GuideEvent e, int delta, const GuideFacts& f) {
    GuideCommand cmd;
    switch (e) {
        case GuideEvent::Rotate:
            if (!f.on_screen) break;  // hidden: main.cpp seeks
            // A viewer's Tune is loading: the cursor stays on its row and the
            // guide is not taken back — the tune still closes it.
            if (viewer_tune_waiting(s, f)) break;
            s.guide_cursor = guide_rotate(
                s.guide_cursor, delta, f.rows,
                s.picture_control_available || s.guide_cursor.on_picture);
            own(s);
            break;

        case GuideEvent::Select: {
            if (s.showing_intro_video) break;  // as on_select
            const int tuned = f.pending_tune.value_or(s.tuned_channel);
            if (!f.on_screen) {
                // Open on the tuned channel; a Close's fade-out still running
                // is cancelled, so it shows at once.
                own(s);
                s.is_fading = false;
                s.guide_cursor = guide_open(tuned, f.rows);
                break;
            }
            GuidePress press = guide_press(s.guide_cursor, tuned, f.cursor_row_tunable);
            // Nothing playing or loading (a failed item's dark window): the
            // tuned row's press retries the channel instead of closing a
            // guide that cannot close.
            const bool nothing_playing = !s.video_active && !has_playing_indexes(s);
            if (press == GuidePress::Close && nothing_playing) {
                press = f.cursor_row_tunable ? GuidePress::Tune : GuidePress::None;
            }
            switch (press) {
                case GuidePress::Tune:
                    // The viewer's tune closes the guide when its load runs
                    // (PlaylistPlayback decides then): ownership goes first.
                    s.guide_opened_by_viewer = false;
                    cmd.tune = s.guide_cursor.row;
                    break;
                case GuidePress::Close:
                    s.guide_opened_by_viewer = false;
                    s.ui_visible_when_playing = false;
                    // Today's fade-out (the List's same-playlist toggle), only
                    // over a video: with none the menu is drawn regardless, and
                    // a fade would dim it to nothing and pop it back.
                    if (s.video_active) {
                        s.fade_start_time = f.now;
                        s.fade_target_ui_visible = false;
                        s.is_fading = true;
                    }
                    break;
                case GuidePress::TogglePicture:
                    // As on_select: a press that lands after the pipeline
                    // changed hands (before the per-frame clamp) was aimed at
                    // a control, not at a row.
                    cmd.toggle_picture = f.owns_pipeline;
                    own(s);
                    break;
                case GuidePress::None:
                    own(s);
                    break;
            }
            break;
        }

        case GuideEvent::ChannelStep:
            if (!f.on_screen) break;
            if (!viewer_tune_waiting(s, f)) own(s);
            // tuned_channel changes only when the load runs: follow the
            // channel being tuned at once.
            if (f.pending_tune) {
                s.guide_cursor = guide_follow(s.guide_cursor, *f.pending_tune, f.rows);
            }
            break;
    }
    return cmd;
}

void guide_tune_taken(AppState& s, int row, bool taken) {
    if (taken) {
        s.guide_viewer_tune = row;
    } else {
        own(s);  // dropped: an input like any other
    }
}

void guide_frame(AppState& s, const GuideFrameFacts& f) {
    if (!f.guide_style || !s.ui_visible_when_playing) s.guide_opened_by_viewer = false;
    if (!f.pending_tune || *f.pending_tune != s.guide_viewer_tune) s.guide_viewer_tune = -1;

    const bool on_screen = f.guide_style && guide_visible(s);
    if (s.guide_appeared.rose(on_screen) && !s.guide_opened_by_viewer) {
        s.guide_cursor = guide_open(f.pending_tune.value_or(s.tuned_channel), f.rows);
    }
    if (!f.guide_style) return;

    s.picture_focus = false;  // the List's; never set in guide style
    s.guide_cursor = guide_clamp(
        s.guide_cursor, f.rows,
        s.picture_control_available || (s.guide_cursor.on_picture && f.picture_may_stay));
}

}  // namespace app
