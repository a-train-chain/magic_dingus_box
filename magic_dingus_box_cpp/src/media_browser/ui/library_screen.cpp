#include "media_browser/ui/library_screen.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>

#include "media_browser/radarr/radarr_client.h"
#include "ui/renderer.h"
#include "ui/theme.h"

namespace media_browser::ui {

namespace {

// Retro home-menu-inspired layout (target 1280x720), shared with
// detail_screen.cpp. The visual idiom mirrors the home menu:
//   - "LIBRARY" header in the Zen Dots title font, steel-blue (accent2),
//     underlined with a full-width 2px rule (same pattern as "Playlists")
//   - Section dividers in steel-blue (accent2)
//   - Gold-outlined chips (no fill) — matches the home-menu's
//     border-and-text aesthetic instead of filled blocks
//   - Blinking ◂ marker on the focused chip / poster — same 500ms cycle
//     and color (accent2) as the playlist-list selection cursor
//   - Status dots in green / gold / red corner badges (same palette as
//     the home menu's now-playing / missing indicators)
//
// All chrome dimensions (header rule Y, padding X, footer hint Y) MUST
// match detail_screen.cpp so the screens read as a single coherent UI
// when the user moves between them.
constexpr float kPaddingX        = 32.0f;
constexpr float kHeaderBaselineY = 38.0f;     // baseline of "LIBRARY"
constexpr float kHeaderRuleY     = 58.0f;     // 2px steel-blue rule below header

// Filter chip strip — sits just below the header rule. Outlined-only.
// kChipPadX is bumped to 18px (from 14) so each chip's body breathes a bit
// more — the strip only ever holds 4 fixed chips, so we have plenty of
// horizontal slack to spend on legibility.
constexpr float kStripTop        = 76.0f;     // top of chip strip
constexpr float kChipH           = 32.0f;
constexpr float kChipPadX        = 18.0f;
constexpr float kChipMinGap      = 14.0f;     // floor for the dynamic gap
constexpr float kChipBorderW     = 2.0f;

// "Marker zone" reserved at each chip's right edge for the blinking ◂
// focus cursor. The zone exists ALWAYS (focused or not) so chip widths
// don't shift on focus — the label is centered inside the chip's body
// MINUS this zone, which keeps the cursor from drawing over the last
// letter of long labels like "Missing Upgrades".
constexpr float kMarkerZoneW     = 22.0f;

// Section divider beneath the filter strip. Same 2px steel-blue rule
// idiom as kHeaderRuleY — bookends the chip strip on top and bottom.
constexpr float kStripRuleY      = 124.0f;

// Poster grid begins below the strip rule.
constexpr float kGridTop         = 144.0f;
constexpr float kCellGapX        = 20.0f;
constexpr float kCellGapY        = 24.0f;
constexpr float kLabelAreaH      = 52.0f;     // title + year below the poster
constexpr float kPosterBorderW   = 2.0f;
constexpr float kPosterFocusW    = 3.0f;      // focused gold outline thickness

// Status dot in the top-right corner of each poster. Drawn as a small
// filled square (matching the home menu's pixel-art status badges) with
// a dark halo so it stays legible regardless of poster artwork.
constexpr float kDotSize         = 14.0f;
constexpr float kDotInset        = 8.0f;

// Footer hint band — 12px from the bottom of the screen, mirroring
// detail_screen.cpp.
constexpr float kFooterMargin    = 12.0f;
constexpr float kFooterReserve   = 36.0f;     // vertical room reserved for hint

// Backdrop poster tint — same deterministic Knuth hash used by Browse
// and Detail, so a given movie has the same fallback color everywhere.
::ui::Color poster_tint_for_tmdb(int tmdb_id) {
    uint32_t h = static_cast<uint32_t>(tmdb_id) * 2654435761u;  // Knuth hash
    uint8_t r = 64 + static_cast<uint8_t>((h >>  0) & 0x7F);
    uint8_t g = 40 + static_cast<uint8_t>((h >>  8) & 0x5F);
    uint8_t b = 80 + static_cast<uint8_t>((h >> 16) & 0x7F);
    return {r, g, b, 255};
}

// Truncate `text` with a trailing ellipsis if it exceeds max_w at font_size.
std::string truncate_to_width(::ui::Renderer& r, const std::string& text,
                              int font_size, float max_w) {
    if (r.mb_text_width(text, font_size) <= max_w) return text;
    const std::string ellipsis = "...";
    for (size_t n = text.size(); n > 0; --n) {
        std::string candidate = text.substr(0, n) + ellipsis;
        if (r.mb_text_width(candidate, font_size) <= max_w) return candidate;
    }
    return ellipsis;
}

// True if the case-insensitive prefix of `s` matches `prefix`. Used by
// the file-state classifier (preserved verbatim from the previous
// revision — quality-string parsing is data-model logic, not visual).
bool starts_with_ci(const std::string& s, const char* prefix) {
    size_t i = 0;
    for (; prefix[i] != '\0'; ++i) {
        if (i >= s.size()) return false;
        char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
        char b = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[i])));
        if (a != b) return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

const char* LibraryScreen::label_for_filter(Filter f) {
    switch (f) {
        case Filter::All:             return "All";
        case Filter::Unwatched:       return "Unwatched";
        case Filter::MissingUpgrades: return "Missing Upgrades";
        case Filter::Recent:          return "Recent";
    }
    return "";
}

bool LibraryScreen::is_1080p_quality(const std::string& q) {
    // Treat "Bluray-*", "Bluray", and "WEBDL-1080p" / "WEB-DL-1080p" as
    // "good enough, no upgrade needed". Anything else (SDTV, DVD,
    // WEBDL-720p, HDTV-720p, etc.) counts as upgradeable. This is a
    // deliberate approximation — the real quality-profile cutoff check
    // lives in Radarr, but we don't have the cutoff id on the client side.
    if (q.empty()) return false;
    if (starts_with_ci(q, "Bluray")) return true;
    if (starts_with_ci(q, "WEBDL-1080p")) return true;
    if (starts_with_ci(q, "WEB-DL-1080p")) return true;
    return false;
}

LibraryScreen::FileState LibraryScreen::classify(const Movie& m) {
    if (!m.has_file) return FileState::MissingFile;
    if (is_1080p_quality(m.file_quality)) return FileState::HasGoodFile;
    return FileState::UpgradeAvailable;
}

// ---------------------------------------------------------------------------
// Construction / lifecycle
// ---------------------------------------------------------------------------

LibraryScreen::LibraryScreen(RadarrClient& radarr) : radarr_(radarr) {}

void LibraryScreen::enter() {
    // Always refresh on (re-)entry so the library list reflects any
    // adds/removes that happened in DetailScreen since we were last
    // visible. The call is cheap on the mock client and acceptable for
    // the MVP cadence on the real HTTP client.
    reload();
}

void LibraryScreen::reload() {
    library_ = radarr_.get_library();
    loaded_ = true;
    rebuild_view();
}

void LibraryScreen::rebuild_view() {
    view_.clear();
    view_.reserve(library_.size());

    for (const auto& m : library_) {
        switch (filter_) {
            case Filter::All:
                view_.push_back(&m);
                break;
            case Filter::Unwatched:
                // MVP-scope: we don't track "watched" state anywhere yet,
                // so this collapses to "has_file == true" (i.e., the
                // films you could watch right now). Swap this for a real
                // watched-state lookup when view tracking lands.
                if (m.has_file) view_.push_back(&m);
                break;
            case Filter::MissingUpgrades:
                if (m.has_file && !is_1080p_quality(m.file_quality)) {
                    view_.push_back(&m);
                }
                break;
            case Filter::Recent:
                view_.push_back(&m);
                break;
        }
    }

    if (filter_ == Filter::Recent) {
        // Sort by added_at descending. ISO-8601 timestamps sort
        // lexicographically in chronological order, so plain string
        // comparison is fine — no need to parse the date.
        std::sort(view_.begin(), view_.end(),
                  [](const Movie* a, const Movie* b) {
                      return a->added_at > b->added_at;
                  });
    }

    // Reset cursor/scroll if the filter shrank the view under them.
    int n = static_cast<int>(view_.size());
    if (grid_cursor_ >= n) grid_cursor_ = std::max(0, n - 1);
    if (grid_cursor_ < 0) grid_cursor_ = 0;
    int cursor_row = n == 0 ? 0 : grid_cursor_ / kGridCols;
    if (scroll_row_ > cursor_row) scroll_row_ = cursor_row;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

Screen LibraryScreen::handle_input(const std::vector<platform::InputEvent>& events) {
    for (const auto& e : events) {
        // Always: Menu returns to Browse (back-stack MVP behavior —
        // matches QueueScreen which also returns to Browse).
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            return Screen::Browse;
        }

        // BTN2 (PLAY_PAUSE): "Play" for the focused movie. For now this
        // transitions to DetailScreen; Task 24 will wire real playback.
        if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed) {
            if (focus_ != Focus::PosterGrid) continue;
            if (view_.empty()) continue;
            if (grid_cursor_ < 0 ||
                grid_cursor_ >= static_cast<int>(view_.size())) continue;
            selected_tmdb_id_ = view_[grid_cursor_]->tmdb_id;
            return Screen::Detail;
        }

        // Vertical movement — ROTATE_VERTICAL (dpad up/down).
        if (e.action == platform::InputAction::ROTATE_VERTICAL) {
            int delta = e.delta;
            if (delta == 0) continue;
            if (focus_ == Focus::FilterStrip) {
                if (delta > 0) {
                    // Drop into the grid at row 0.
                    focus_ = Focus::PosterGrid;
                    if (view_.empty()) {
                        grid_cursor_ = 0;
                    } else {
                        grid_cursor_ = std::min(grid_cursor_,
                                                static_cast<int>(view_.size()) - 1);
                    }
                    scroll_row_ = 0;
                }
                // Up from the strip is a no-op.
            } else {
                // In the grid. Up from the top row jumps to the strip.
                int row = view_.empty() ? 0 : grid_cursor_ / kGridCols;
                int col = view_.empty() ? 0 : grid_cursor_ % kGridCols;
                if (delta < 0 && (row == 0 || view_.empty())) {
                    focus_ = Focus::FilterStrip;
                    filter_cursor_ = static_cast<int>(filter_);
                } else if (!view_.empty()) {
                    int new_row = row + (delta > 0 ? 1 : -1);
                    int max_row = (static_cast<int>(view_.size()) - 1) / kGridCols;
                    new_row = std::clamp(new_row, 0, max_row);
                    int new_idx = new_row * kGridCols + col;
                    if (new_idx < static_cast<int>(view_.size())) {
                        grid_cursor_ = new_idx;
                    } else {
                        grid_cursor_ = static_cast<int>(view_.size()) - 1;
                    }
                }
            }
            continue;
        }

        // Horizontal movement — ROTATE (dpad L/R and rotary wheel).
        if (e.action == platform::InputAction::ROTATE) {
            int delta = e.delta;
            if (delta == 0) continue;
            if (focus_ == Focus::FilterStrip) {
                int old = filter_cursor_;
                filter_cursor_ = std::clamp(filter_cursor_ + delta, 0,
                                            kNumFilters - 1);
                if (filter_cursor_ != old) {
                    // Apply the filter change immediately so the user
                    // sees the grid update without needing to confirm.
                    filter_ = static_cast<Filter>(filter_cursor_);
                    rebuild_view();
                }
            } else {
                if (view_.empty()) continue;
                int n = static_cast<int>(view_.size());
                grid_cursor_ = std::clamp(grid_cursor_ + delta, 0, n - 1);
            }
            continue;
        }

        // Select — A button or rotary click.
        if (e.action == platform::InputAction::SELECT && e.pressed) {
            if (focus_ == Focus::FilterStrip) {
                // Top-strip SELECT confirms the filter + drops back to
                // the grid. The horizontal handler already applied the
                // change, so this is mostly a "done picking" gesture.
                filter_ = static_cast<Filter>(filter_cursor_);
                rebuild_view();
                focus_ = Focus::PosterGrid;
                continue;
            }
            if (!view_.empty() &&
                grid_cursor_ >= 0 &&
                grid_cursor_ < static_cast<int>(view_.size())) {
                selected_tmdb_id_ = view_[grid_cursor_]->tmdb_id;
                return Screen::Detail;
            }
            continue;
        }
    }

    // Keep scroll_row_ such that the grid cursor is visible. Upper bound
    // is enforced in render() where we know visible_rows from the window
    // height.
    if (focus_ == Focus::PosterGrid && !view_.empty()) {
        int row = grid_cursor_ / kGridCols;
        if (row < scroll_row_) scroll_row_ = row;
    }

    return Screen::Library;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void LibraryScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    const ::ui::Theme& th = r.mb_theme();
    r.mb_fill_background();

    const float w = static_cast<float>(screen_w);
    const float h = static_cast<float>(screen_h);

    // 500ms blink cycle, sourced from epoch time so it stays in lockstep
    // with the home-menu cursor and DetailScreen's action button — all
    // three blinks visually breathe together when transitioning between
    // screens.
    auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    const bool blink_on = (epoch_ms / 500) % 2 == 0;

    // --- Top header bar: "LIBRARY" + (N movies) + back hint ----------
    // Same idiom as DetailScreen's "FEATURE PRESENTATION" header: a Zen
    // Dots heading in steel-blue (accent2), a dim count next to it, and
    // a full-width 2px rule beneath. The rule reads as a screen frame,
    // not a heading underline.
    {
        const std::string heading = "LIBRARY";
        int hd_size = th.font_heading_size;
        r.mb_draw_title_text(heading, kPaddingX, kHeaderBaselineY,
                             hd_size, th.accent2, 1.0f);

        // "(N movies)" count, dim, body font, baseline aligned with the
        // heading. Sits immediately to the right of the title with a
        // small gap so it reads as a subtitle.
        int count_size = th.font_medium_size;
        std::ostringstream cs;
        size_t shown = view_.size();
        cs << "(" << shown << (shown == 1 ? " movie" : " movies") << ")";
        std::string count_text = cs.str();
        int title_px = r.mb_title_text_width(heading, hd_size);
        float count_x = kPaddingX + static_cast<float>(title_px) + 14.0f;
        r.mb_draw_text(count_text, count_x, kHeaderBaselineY + 2.0f,
                       count_size, th.dim, 0.9f);

        // Right-aligned back hint, small + dim — same pattern as Detail.
        const std::string back_hint = "BTN4: back";
        int hint_size = th.font_small_size;
        int hw = r.mb_text_width(back_hint, hint_size);
        float hx = w - kPaddingX - static_cast<float>(hw);
        r.mb_draw_text(back_hint, hx, kHeaderBaselineY + 2.0f,
                       hint_size, th.dim, 0.9f);

        // Full-width 2px steel-blue rule beneath the header.
        r.mb_draw_line(kPaddingX, kHeaderRuleY,
                       w - kPaddingX, kHeaderRuleY,
                       2.0f, th.accent2, 0.95f);
    }

    // --- Filter chip strip --------------------------------------------
    // Outlined-only chips, no fill. Active chip: gold outline + gold
    // text. Inactive chip: dim outline + dim text. Focused chip (when
    // the strip has focus): thicker (3px) gold outline plus a blinking
    // ◂ inside the right edge — same marker idiom as DetailScreen's
    // action buttons and the home menu's playlist cursor.
    //
    // The strip only ever holds 4 fixed chips (All / Unwatched /
    // Missing Upgrades / Recent), so there's a lot of horizontal slack
    // on a 1280-wide screen. Rather than letting the chips cluster on
    // the left, we measure the total natural width of all chips at
    // their minimum gap and then redistribute any leftover slack into
    // the inter-chip gaps. End result: the four chips spread evenly
    // across the available width, filling the strip end-to-end.
    {
        int chip_size = th.font_small_size;
        int chip_baseline = r.mb_text_baseline(chip_size);

        // Natural width per chip = pad-left + label + pad-right + marker zone.
        // The marker zone is reserved on EVERY chip (focused or not) so chip
        // widths don't shift on focus changes — preventing a layout reflow
        // every 500ms as the cursor blinks.
        float chip_w[kNumFilters];
        float total_chip_w = 0.0f;
        for (int i = 0; i < kNumFilters; ++i) {
            Filter f = static_cast<Filter>(i);
            const char* label = label_for_filter(f);
            float label_w = static_cast<float>(r.mb_text_width(label, chip_size));
            chip_w[i] = label_w + 2.0f * kChipPadX + kMarkerZoneW;
            total_chip_w += chip_w[i];
        }

        // Compute the inter-chip gap. Start at the minimum, then
        // distribute any remaining slack across the (n-1) gaps. If the
        // chips somehow overflow the strip (very long font / tiny
        // screen), we fall back to the minimum gap and let them clip —
        // legibility of the labels matters more than fitting.
        const float strip_w = w - 2.0f * kPaddingX;
        float gap = kChipMinGap;
        if (kNumFilters > 1) {
            float available_for_gaps = strip_w - total_chip_w;
            float computed_gap = available_for_gaps
                               / static_cast<float>(kNumFilters - 1);
            if (computed_gap > gap) gap = computed_gap;
        }

        float chip_x = kPaddingX;
        float chip_y = kStripTop;

        // When focus is on the poster grid, fade the filter chips so the
        // user can still see what their options are but knows their input
        // affects posters, not the filter strip. The currently-active
        // filter chip stays at full alpha — it's the breadcrumb that says
        // "you're filtering by All".
        const float kInactiveChipFade = 0.4f;
        const bool grid_has_focus = (focus_ == Focus::PosterGrid);

        for (int i = 0; i < kNumFilters; ++i) {
            Filter f = static_cast<Filter>(i);
            bool is_active  = (f == filter_);
            bool is_focused = (focus_ == Focus::FilterStrip && i == filter_cursor_);

            const char* label = label_for_filter(f);
            float this_chip_w = chip_w[i];

            // Border + text colors. Active = gold (accent), inactive = dim.
            // Focused inherits whichever the chip already uses but with
            // a thicker outline so the focus state still stands out on a
            // chip that's already active.
            ::ui::Color border_color = is_active ? th.accent : th.dim;
            ::ui::Color label_color  = is_active ? th.accent : th.dim;
            float border_thickness   = is_focused ? 3.0f : kChipBorderW;
            float border_alpha       = is_focused ? 1.0f : (is_active ? 0.95f : 0.75f);
            float label_alpha        = is_focused ? 1.0f : (is_active ? 1.0f : 0.85f);

            // If the grid currently owns focus, de-emphasize every chip
            // EXCEPT the active filter chip (the breadcrumb).
            float fade = (grid_has_focus && !is_active) ? kInactiveChipFade : 1.0f;

            r.mb_stroke_rect(chip_x, chip_y, this_chip_w, kChipH,
                             border_thickness, border_color,
                             border_alpha * fade);

            // Center the label inside the FULL chip width. The chips are
            // sized/spaced comfortably enough that label centering in the
            // full body still leaves room near the right edge for the
            // focus marker to sit over (or near) empty space — most chip
            // labels are shorter than chip_w - 2*kChipPadX - kMarkerZoneW.
            float label_px = static_cast<float>(r.mb_text_width(label, chip_size));
            float lx = chip_x + (this_chip_w - label_px) / 2.0f;
            float ly = chip_y + (kChipH - static_cast<float>(chip_size)) / 2.0f
                     + static_cast<float>(chip_baseline);
            r.mb_draw_text(label, lx, ly, chip_size, label_color,
                           label_alpha * fade);

            // Blinking ◂ centered inside the reserved marker zone. Only
            // drawn when the strip itself has focus — when the grid has
            // focus, the chip has no marker (faded chips signal "not
            // currently navigable").
            if (focus_ == Focus::FilterStrip && is_focused && blink_on) {
                float marker_size = static_cast<float>(chip_size) * 0.45f;
                float marker_cx = chip_x + this_chip_w - kMarkerZoneW * 0.5f;
                float marker_cy = chip_y + kChipH / 2.0f;
                r.mb_fill_triangle(
                    marker_cx,                       marker_cy - marker_size,
                    marker_cx,                       marker_cy + marker_size,
                    marker_cx - marker_size * 1.2f,  marker_cy,
                    th.accent2, 1.0f);
            }

            chip_x += this_chip_w + gap;
        }

        // Section divider beneath the strip — matches the kHeaderRuleY
        // pattern, framing the chip row top-and-bottom so it reads as a
        // distinct band from the grid below.
        r.mb_draw_line(kPaddingX, kStripRuleY,
                       w - kPaddingX, kStripRuleY,
                       2.0f, th.accent2, 0.85f);
    }

    // --- Poster grid --------------------------------------------------
    // 5-column grid of posters (see kGridCols in library_screen.h).
    // Each cell shows artwork (with a deterministic Knuth-hash tint
    // fallback), a 2px gold border, a status corner dot (green/gold/red)
    // in the top-right, and the movie title + year underneath. Focused
    // cell gets a 3px gold outline plus a blinking ◂ to the right of
    // its title (drawn inside a reserved "marker zone" so it never
    // overlaps the last letter of the title).
    const float grid_left   = kPaddingX;
    const float grid_right  = w - kPaddingX;
    const float grid_inner  = grid_right - grid_left;
    const float grid_top    = kGridTop;
    const float grid_bottom = h - kFooterReserve;

    // Compute cell dimensions from the available width so the grid
    // breathes with the screen. N cells + (N-1) gaps = grid_inner.
    // Poster height is fixed at the standard 2:3 ratio of the cell width.
    const float cell_w = (grid_inner - (kGridCols - 1) * kCellGapX)
                       / static_cast<float>(kGridCols);
    const float poster_w = cell_w;
    const float poster_h = poster_w * 1.5f;             // 2:3 movie poster
    const float cell_h = poster_h + kLabelAreaH;

    // Scale the status corner dot with the cell width so it remains
    // visually balanced as the grid widens or tightens. ~6% of the
    // poster width tracks the original 14px dot at the previous 4-col
    // ~285px cell width and stays legible at the new 5-col ~227px cell.
    const float dot_size  = std::clamp(poster_w * 0.06f, 10.0f, kDotSize);
    const float dot_inset = std::clamp(poster_w * 0.035f, 6.0f, kDotInset);

    int visible_rows = std::max(1,
        static_cast<int>((grid_bottom - grid_top) / (cell_h + kCellGapY)));

    // Clamp scroll so the focused cell stays on screen.
    if (focus_ == Focus::PosterGrid && !view_.empty()) {
        int focused_row = grid_cursor_ / kGridCols;
        if (focused_row < scroll_row_) scroll_row_ = focused_row;
        if (focused_row >= scroll_row_ + visible_rows) {
            scroll_row_ = focused_row - visible_rows + 1;
        }
    }

    int total_rows = view_.empty() ? 0
                   : (static_cast<int>(view_.size()) - 1) / kGridCols + 1;
    int end_row = std::min(total_rows, scroll_row_ + visible_rows);

    // Centered single-message states. Loading: accent (gold) so it
    // reads as "active wait". Empty: dim so it reads as "no content".
    // Same vocabulary as DetailScreen's Loading / NoTmdb states.
    if (view_.empty()) {
        std::string msg;
        ::ui::Color msg_color = th.dim;
        if (!loaded_) {
            msg = "Loading library...";
            msg_color = th.accent;
        } else if (library_.empty() && filter_ == Filter::All) {
            msg = "Your library is empty. Add movies from Browse to build it.";
        } else if (library_.empty()) {
            msg = "No movies in library yet — add some from Browse";
        } else {
            msg = "No movies match this filter";
        }
        int msg_size = th.font_large_size;
        int msg_w = r.mb_text_width(msg, msg_size);
        float msg_x = (w - static_cast<float>(msg_w)) / 2.0f;
        float msg_y = grid_top + (grid_bottom - grid_top) / 2.0f
                    + static_cast<float>(r.mb_text_baseline(msg_size));
        r.mb_draw_text(msg, msg_x, msg_y, msg_size, msg_color, 0.9f);
    } else {
        for (int row = scroll_row_; row < end_row; ++row) {
            for (int col = 0; col < kGridCols; ++col) {
                int idx = row * kGridCols + col;
                if (idx >= static_cast<int>(view_.size())) break;
                const Movie& m = *view_[idx];

                float cell_x = grid_left + col * (cell_w + kCellGapX);
                float cell_y = grid_top + (row - scroll_row_) * (cell_h + kCellGapY);

                bool focused = (focus_ == Focus::PosterGrid && idx == grid_cursor_);

                // Poster artwork (or deterministic tint fallback). Same
                // poster_fit semantics as Detail's hero poster — fill
                // first, then overlay a 2px gold border so each cell
                // reads as a TV-monitor frame.
                ::ui::Color tint = poster_tint_for_tmdb(m.tmdb_id);
                r.mb_draw_poster_fit(m.poster_url,
                                     cell_x, cell_y, poster_w, poster_h,
                                     tint, 1.0f);

                // 2px gold border by default; 3px gold border when
                // focused. Both pure-outline — no fill — matching the
                // home-menu border-and-text aesthetic.
                float border_w = focused ? kPosterFocusW : kPosterBorderW;
                float border_alpha = focused ? 1.0f : 0.85f;
                r.mb_stroke_rect(cell_x, cell_y, poster_w, poster_h,
                                 border_w, th.accent, border_alpha);

                // Status corner dot — green / gold / red. Drawn as a
                // small filled square with a 2px dark halo so it stays
                // legible regardless of the underlying poster colors.
                FileState state = classify(m);
                ::ui::Color dot_color;
                switch (state) {
                    case FileState::HasGoodFile:      dot_color = th.highlight1; break; // green
                    case FileState::UpgradeAvailable: dot_color = th.accent;     break; // gold
                    case FileState::MissingFile:      dot_color = th.highlight2; break; // red
                }
                float dx = cell_x + poster_w - dot_inset - dot_size;
                float dy = cell_y + dot_inset;
                r.mb_fill_rect(dx - 2.0f, dy - 2.0f,
                               dot_size + 4.0f, dot_size + 4.0f,
                               th.bg, 0.9f);
                r.mb_fill_rect(dx, dy, dot_size, dot_size, dot_color, 1.0f);

                // Title + year beneath the poster. Body font, cream on
                // dim — same hierarchy as detail's metadata column.
                //
                // Marker-zone pattern: we still TRUNCATE the title to
                // `poster_w - kMarkerZoneW` so a long title can never
                // crowd or overflow into the focus cursor, but we CENTER
                // the (possibly-truncated) title in the FULL poster
                // width. The cursor will sit at the right edge over (or
                // near) empty space because most titles are shorter than
                // the truncation budget anyway.
                int t_size = th.font_medium_size;
                int t_baseline = r.mb_text_baseline(t_size);
                std::string title = m.title.empty() ? "Untitled" : m.title;
                float title_max_w = poster_w - kMarkerZoneW;
                std::string drawn_title =
                    truncate_to_width(r, title, t_size, title_max_w);
                float drawn_title_px = static_cast<float>(
                    r.mb_text_width(drawn_title, t_size));
                float title_x =
                    cell_x + (poster_w - drawn_title_px) / 2.0f;
                float t_y = cell_y + poster_h + 10.0f
                          + static_cast<float>(t_baseline);
                r.mb_draw_text(drawn_title, title_x, t_y, t_size, th.fg,
                               focused ? 1.0f : 0.9f);

                // Blinking ◂ centered inside the reserved marker zone
                // at the right edge of the title baseline. Same color
                // (accent2) and 500ms cycle as the chip-strip cursor
                // and the home-menu playlist cursor.
                if (focused && blink_on) {
                    float marker_size = static_cast<float>(t_size) * 0.40f;
                    float marker_cx = cell_x + poster_w - kMarkerZoneW * 0.5f;
                    float marker_cy = t_y - static_cast<float>(t_size) / 3.0f;
                    r.mb_fill_triangle(
                        marker_cx,                       marker_cy - marker_size,
                        marker_cx,                       marker_cy + marker_size,
                        marker_cx - marker_size * 1.2f,  marker_cy,
                        th.accent2, 1.0f);
                }

                if (m.year > 0) {
                    int y_size = th.font_small_size;
                    int y_baseline = r.mb_text_baseline(y_size);
                    float y_y = t_y + static_cast<float>(t_size) * 0.4f
                              + static_cast<float>(y_baseline);
                    std::string year = std::to_string(m.year);
                    r.mb_draw_text(year, cell_x, y_y, y_size, th.dim, 0.9f);
                }
            }
        }
    }

    // --- Footer hint --------------------------------------------------
    // Centered, dim, small body font — same placement and tone as
    // DetailScreen's bottom hint so the chrome reads identically across
    // the two screens.
    {
        const std::string hint =
            "Rotate: nav   BTN2: select   BTN4: back";
        int sz = th.font_small_size;
        int baseline = r.mb_text_baseline(sz);
        int hw = r.mb_text_width(hint, sz);
        float hx = (w - static_cast<float>(hw)) / 2.0f;
        float hy = h - kFooterMargin - static_cast<float>(sz)
                 + static_cast<float>(baseline);
        r.mb_draw_text(hint, hx, hy, sz, th.dim, 0.85f);
    }
}

}  // namespace media_browser::ui
