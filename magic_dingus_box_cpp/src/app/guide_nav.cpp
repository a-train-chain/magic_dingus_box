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
    const int previous_row = c.row;
    c.row = std::max(0, std::min(c.row, rows - 1));
    if (c.row != previous_row || c.on_picture || rows <= 0) c.pick_at.reset();

    // Keep the cursor row on screen, and the window inside the list.
    if (c.row >= c.scroll + kGuideVisibleRows) c.scroll = c.row - kGuideVisibleRows + 1;
    if (c.row < c.scroll) c.scroll = c.row;
    const int max_scroll = std::max(0, rows - kGuideVisibleRows);
    c.scroll = std::max(0, std::min(c.scroll, max_scroll));
    return c;
}

GuideCursor guide_follow(GuideCursor c, int channel, int rows) {
    c.pick_at.reset();
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

bool guide_painted(const AppState& s) {
    if (s.showing_intro_video) return false;
    if (guide_visible(s)) return true;
#ifdef MEDIA_BROWSER_ENABLED
    if (s.current_screen == AppScreen::MediaBrowser) return false;
#endif
    // A Close's fade-out (guide_input): the overlay is hidden, the alpha
    // still falling.
    return s.is_fading && !s.fade_target_ui_visible;
}

bool movie_full_screen(const AppState& s) {
    if (!full_screen_films_possible(s)) return false;
#ifdef MEDIA_BROWSER_ENABLED
    if (s.current_screen == AppScreen::MediaBrowser) return false;
#endif
    const auto* item = s.current_playlist_item();
    return item && item->movie_tmdb_id > 0 && !guide_painted(s);
}

bool LastChannelWatch::changed(const AppState& s) {
    if (s.tuned_channel == seen_tuned) return false;
    seen_tuned = s.tuned_channel;
    if (s.last_channel_id == saved_id) return false;
    saved_id = s.last_channel_id;
    return true;
}

GuideCommand guide_input(AppState& s, GuideEvent e, int delta, const GuideFacts& f) {
    GuideCommand cmd;
    auto& cursor = s.guide_cursor;
    if (!f.pick_ahead || !f.on_screen || !f.cursor_row_tunable || cursor.on_picture)
        cursor.pick_at.reset();
    // row_airings is already in chronological order. Ignore expired entries
    // and starts at/after the exclusive 24-hour horizon, even for stale facts.
    const auto first = std::find_if(f.row_airings.begin(), f.row_airings.end(), [&](const OnAir& a) {
        return a.starts_at <= f.now_utc && a.ends_at > f.now_utc;
    });
    const auto limit = std::find_if(f.row_airings.begin(), f.row_airings.end(), [&](const OnAir& a) {
        return a.starts_at >= f.now_utc + 24 * 3600;
    });
    auto picked = f.row_airings.end();
    if (cursor.pick_at && first != f.row_airings.end()) {
        picked = std::find_if(first, limit, [&](const OnAir& a) { return a.starts_at == *cursor.pick_at; });
        if (picked == limit) picked = first;  // elapsed or changed schedule: on now
        cursor.pick_at = picked->starts_at;
    } else if (cursor.pick_at) {
        cursor.pick_at.reset();
    }
    const auto enter_row = [&] {
        if (!f.pick_ahead || !f.cursor_row_tunable || cursor.on_picture ||
            cursor.row < 0 || cursor.row >= f.rows ||
            first == f.row_airings.end() || viewer_tune_waiting(s, f)) return false;
        cursor.pick_at = first->starts_at;
        own(s);
        return true;
    };
    switch (e) {
        case GuideEvent::PickAhead:
            if (!f.pick_ahead || s.showing_intro_video || viewer_tune_waiting(s, f)) break;
            if (!f.on_screen) {
                own(s);
                s.is_fading = false;
                cursor = guide_open(f.pending_tune.value_or(s.tuned_channel), f.rows);
            }
            if (!cursor.pick_at) enter_row();
            break;
        case GuideEvent::Back:
            if (f.pick_ahead && f.on_screen && cursor.pick_at && !viewer_tune_waiting(s, f)) {
                cursor.pick_at.reset();
                own(s);
                cmd.consumed = true;
            }
            break;
        case GuideEvent::Horizontal:
            if (!f.pick_ahead || !f.on_screen || viewer_tune_waiting(s, f)) break;
            if (!cursor.pick_at) {
                if (delta > 0) enter_row();
                break;
            }
            [[fallthrough]];
        case GuideEvent::Rotate:
            if (!f.on_screen) break;  // hidden: main.cpp seeks
            // A viewer's Tune is loading: the cursor stays on its row and the
            // guide is not taken back — the tune still closes it.
            if (viewer_tune_waiting(s, f)) break;
            if (f.pick_ahead && cursor.pick_at && picked != f.row_airings.end()) {
                if (delta < 0) {
                    if (picked == first) cursor.pick_at.reset();
                    else cursor.pick_at = std::prev(picked)->starts_at;
                } else if (delta > 0 && std::next(picked) != limit) {
                    cursor.pick_at = std::next(picked)->starts_at;
                }
                own(s);
                break;
            }
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
            if (f.pick_ahead && cursor.pick_at && picked != f.row_airings.end() &&
                picked->starts_at > f.now_utc) {
                cmd.on_demand = *picked;
                s.guide_opened_by_viewer = false;
                s.off_air_return_id.clear();
                s.off_air_return_after_utc = 0;
                cursor.pick_at.reset();
                break;
            }
            GuidePress press = guide_press(s.guide_cursor, tuned, f.cursor_row_tunable);
            // Explicit on-now selection leaves a chosen future programme.
            // A normal live Close, Picture press, or an off-air chosen film
            // keeps its existing route.
            if (press == GuidePress::Close && s.on_demand && f.cursor_row_tunable &&
                cursor.pick_at && picked != f.row_airings.end() &&
                picked->starts_at <= f.now_utc && f.now_utc < picked->ends_at)
                press = GuidePress::Tune;
            // Nothing playing or loading (a failed item's dark window): the
            // tuned row's press retries the channel instead of closing a
            // guide that cannot close.
            const bool nothing_playing = !s.video_active && !has_playing_indexes(s);
            if (press == GuidePress::Close && nothing_playing) {
                press = f.cursor_row_tunable ? GuidePress::Tune : GuidePress::None;
            }
            switch (press) {
                case GuidePress::Tune:
                    cursor.pick_at.reset();
                    s.off_air_return_id.clear();
                    s.off_air_return_after_utc = 0;
                    // The viewer's tune closes the guide when its load runs
                    // (PlaylistPlayback decides then): ownership goes first.
                    s.guide_opened_by_viewer = false;
                    cmd.tune = s.guide_cursor.row;
                    break;
                case GuidePress::Close:
                    cursor.pick_at.reset();
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
            cursor.pick_at.reset();
            s.off_air_return_id.clear();
            s.off_air_return_after_utc = 0;
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
    if (!on_screen || !f.pick_ahead || !f.cursor_row_tunable) s.guide_cursor.pick_at.reset();
    if (s.guide_appeared.rose(on_screen) && !s.guide_opened_by_viewer) {
        s.guide_cursor = guide_open(f.pending_tune.value_or(s.tuned_channel), f.rows);
    }
    if (!f.guide_style) return;

    s.picture_focus = false;  // the List's; never set in guide style
    // Drawn only on the tuned channel's panel: nothing tuned, no control.
    const bool tuned = f.pending_tune.value_or(s.tuned_channel) >= 0;
    if (!tuned) s.picture_control_available = false;
    s.guide_cursor = guide_clamp(
        s.guide_cursor, f.rows,
        s.picture_control_available ||
            (tuned && s.guide_cursor.on_picture && f.picture_may_stay));
}

std::optional<int> guide_entry_tune(const AppState& s) {
#ifdef MEDIA_BROWSER_ENABLED
    if (s.current_screen == AppScreen::MediaBrowser) return std::nullopt;
#endif
    if (!has_playing_indexes(s)) return std::nullopt;
    return s.master_shuffle_active ? 0 : s.current_playlist_index;
}

}  // namespace app
