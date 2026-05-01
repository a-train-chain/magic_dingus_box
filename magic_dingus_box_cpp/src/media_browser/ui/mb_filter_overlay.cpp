#include "media_browser/ui/mb_filter_overlay.h"

#include "ui/renderer.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace media_browser::ui {

namespace {

// Bezel inset — the wood-frame PNG reserves this many pixels on every edge.
constexpr int kBezelInset = 40;

// Top margin increased from 100 → 155 so the panel clears the tab-strip text
// (Popular | Top Rated | Search | Library | Queue | Settings).
constexpr int kPanelTopMarginPx    = 155;
constexpr int kPanelBottomMarginPx = kBezelInset;

// Single-line row: label + value on one line, ~38px tall.
constexpr int kRowHeightPx = 38;
constexpr int kPaddingX    = 24;

// Cursor triangle geometry — matches the ▶ style used by LibraryScreen.
constexpr float kMarkerHalfH = 6.0f;
constexpr float kMarkerW     = 7.2f;

float ease_in_cubic(float t)  { return t * t * t; }
float ease_out_cubic(float t) { float u = 1.0f - t; return 1.0f - u * u * u; }

// Genre catalog — index in this list maps to bit position in genre_mask.
struct GenreEntry { int tmdb_id; const char* display; };
constexpr GenreEntry kGenres[] = {
    {28,    "Action"},    {12,    "Adventure"}, {16,    "Animation"}, {35,    "Comedy"},
    {80,    "Crime"},     {99,    "Doc"},        {18,    "Drama"},     {10751, "Family"},
    {14,    "Fantasy"},   {36,    "History"},    {27,    "Horror"},    {10402, "Music"},
    {9648,  "Mystery"},   {10749, "Romance"},    {878,   "Sci-Fi"},    {10770, "TV Movie"},
    {53,    "Thriller"},  {10752, "War"},
};
constexpr int kNumGenres = static_cast<int>(sizeof(kGenres) / sizeof(kGenres[0]));

const char* kDecadeLabels[]   = {"Any", "2020s", "2010s", "2000s", "1990s", "1980s", "1970s", "Classic"};
const char* kRatingLabels[]   = {"Any", "6.0+", "7.0+", "8.0+"};
const char* kRuntimeLabels[]  = {"Any", "<90m", "90-120m", "2-3hr", "3hr+"};
const char* kLanguageLabels[] = {"Any", "English", "Japanese", "French", "Spanish",
                                  "Korean", "Italian", "German", "Hindi", "Mandarin"};
const char* kSortLabels[]     = {"Popularity", "Top Rated", "Most Voted", "Recent Release"};

constexpr int kNumDecades   = static_cast<int>(sizeof(kDecadeLabels)   / sizeof(kDecadeLabels[0]));
constexpr int kNumRatings   = static_cast<int>(sizeof(kRatingLabels)   / sizeof(kRatingLabels[0]));
constexpr int kNumRuntimes  = static_cast<int>(sizeof(kRuntimeLabels)  / sizeof(kRuntimeLabels[0]));
constexpr int kNumLanguages = static_cast<int>(sizeof(kLanguageLabels) / sizeof(kLanguageLabels[0]));
constexpr int kNumSorts     = static_cast<int>(sizeof(kSortLabels)     / sizeof(kSortLabels[0]));

// Returns the display string for the current genre_mask in single-select mode.
// "All" if mask is 0, else the genre name corresponding to the lowest set bit.
const char* genre_display_name(uint32_t mask) {
    if (mask == 0) return "All";
    const int bit = __builtin_ctz(mask);
    if (bit < kNumGenres) return kGenres[bit].display;
    return "?";
}

// Wrapping modular arithmetic for signed indices.
inline int wrap(int val, int count) {
    return ((val % count) + count) % count;
}

}  // namespace

const std::vector<int>& filter_overlay_genre_ids() {
    static const std::vector<int> ids = []() {
        std::vector<int> v;
        v.reserve(kNumGenres);
        for (int i = 0; i < kNumGenres; ++i) v.push_back(kGenres[i].tmdb_id);
        return v;
    }();
    return ids;
}

FilterOverlay::FilterOverlay() = default;

void FilterOverlay::open(FilterTabKind tab, const FilterState& current) {
    if (state_ == State::Open || state_ == State::SlidingIn) return;
    tab_      = tab;
    working_  = current;
    focus_row_ = 0;
    mode_     = Mode::RowSelect;   // always start in row-navigation mode
    state_    = State::SlidingIn;
    anim_started_at_ = std::chrono::steady_clock::now();
}

void FilterOverlay::close() {
    if (state_ == State::Closed || state_ == State::SlidingOut) return;
    state_ = State::SlidingOut;
    anim_started_at_ = std::chrono::steady_clock::now();
}

void FilterOverlay::tick() {
    using namespace std::chrono;
    if (state_ == State::Closed || state_ == State::Open) return;
    const auto elapsed = duration_cast<milliseconds>(
        steady_clock::now() - anim_started_at_).count();
    if (state_ == State::SlidingIn && elapsed >= kSlideInMs) {
        state_ = State::Open;
    } else if (state_ == State::SlidingOut && elapsed >= kSlideOutMs) {
        state_ = State::Closed;
    }
}

int FilterOverlay::compute_panel_left_x() const {
    using namespace std::chrono;
    constexpr int kScreenW      = 1280;
    constexpr int kPanelOpenX   = kScreenW - kBezelInset - kPanelWidthPx;  // 860
    constexpr int kPanelClosedX = kScreenW;  // off-screen right
    if (state_ == State::Closed) return kPanelClosedX;
    if (state_ == State::Open)   return kPanelOpenX;
    const auto elapsed = duration_cast<milliseconds>(
        steady_clock::now() - anim_started_at_).count();
    if (state_ == State::SlidingIn) {
        const float t = std::clamp(
            static_cast<float>(elapsed) / static_cast<float>(kSlideInMs), 0.0f, 1.0f);
        return static_cast<int>(kPanelClosedX + ease_out_cubic(t) * (kPanelOpenX - kPanelClosedX));
    } else {  // SlidingOut
        const float t = std::clamp(
            static_cast<float>(elapsed) / static_cast<float>(kSlideOutMs), 0.0f, 1.0f);
        return static_cast<int>(kPanelOpenX + ease_in_cubic(t) * (kPanelClosedX - kPanelOpenX));
    }
}

// ---------------------------------------------------------------------------
// cycle_focused_value — mutates working_ for the focused row.
// direction > 0 = forward through options, < 0 = backward.
// ---------------------------------------------------------------------------
void FilterOverlay::cycle_focused_value(int direction) {
    const int dir = (direction >= 0) ? 1 : -1;
    switch (focus_row_) {
        case 0: {
            // Genre: cycle "All" → Action → Adventure → … → War → "All"
            const uint32_t mask = working_.genre_mask;
            if (dir > 0) {
                if (mask == 0) {
                    working_.genre_mask = 1u;
                } else {
                    const int bit = __builtin_ctz(mask);
                    const int nb  = bit + 1;
                    working_.genre_mask = (nb >= kNumGenres) ? 0u : (1u << nb);
                }
            } else {
                if (mask == 0) {
                    working_.genre_mask = (1u << (kNumGenres - 1));
                } else {
                    const int bit = __builtin_ctz(mask);
                    working_.genre_mask = (bit == 0) ? 0u : (1u << (bit - 1));
                }
            }
            break;
        }
        case 1: working_.decade     = wrap(working_.decade     + dir, kNumDecades);   break;
        case 2: working_.min_rating = wrap(working_.min_rating + dir, kNumRatings);   break;
        case 3: working_.runtime    = wrap(working_.runtime    + dir, kNumRuntimes);  break;
        case 4: working_.language   = wrap(working_.language   + dir, kNumLanguages); break;
        case 5: working_.sort       = wrap(working_.sort       + dir, kNumSorts);     break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// on_rotate
//   RowSelect   → move cursor up/down through rows
//   ValueSelect → cycle the focused row's value
// ---------------------------------------------------------------------------
bool FilterOverlay::on_rotate(int delta) {
    if (!is_input_active()) return false;
    if (delta == 0) return false;

    if (mode_ == Mode::ValueSelect) {
        cycle_focused_value(delta);
    } else {
        int new_focus = focus_row_ + (delta > 0 ? 1 : -1);
        if (new_focus < 0)                  new_focus = 0;
        if (new_focus >= kFocusableRowCount) new_focus = kFocusableRowCount - 1;
        focus_row_ = new_focus;
    }
    return true;
}

// ---------------------------------------------------------------------------
// on_select (rotary press)
//   RowSelect + value row (0-5) → enter ValueSelect for that row
//   RowSelect + RESET ALL (row 6) → reset working_, stay in RowSelect
//   ValueSelect → save current value (already in working_ via rotate), exit to RowSelect
// ---------------------------------------------------------------------------
bool FilterOverlay::on_select() {
    if (!is_input_active()) return false;

    if (mode_ == Mode::ValueSelect) {
        // Value is already staged in working_ by rotate — just exit editing mode.
        mode_ = Mode::RowSelect;
    } else {
        // RowSelect
        if (focus_row_ == 6) {
            // RESET ALL — clear staging area, stay in RowSelect, no commit.
            working_ = FilterState{};
        } else {
            // Enter value-editing mode for this row.
            mode_ = Mode::ValueSelect;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// on_btn4_close
//   ValueSelect → exit to RowSelect (first press just saves the current edit)
//   RowSelect   → fire commit callback ONCE with accumulated working_ state, then close
// ---------------------------------------------------------------------------
bool FilterOverlay::on_btn4_close() {
    if (!is_visible()) return false;

    if (mode_ == Mode::ValueSelect) {
        // User pressed BTN4 while editing a value — treat as "done editing this row".
        // Staged value is already in working_. Return to row navigation.
        mode_ = Mode::RowSelect;
        return true;
    }

    // RowSelect: apply all staged changes in one shot.
    if (on_commit_) on_commit_(working_, tab_);
    close();
    return true;
}

// ---------------------------------------------------------------------------
// render helpers
// ---------------------------------------------------------------------------

// Render a single filter row: label on the left (dim), value on the right (fg
// normally; gold + brackets when in ValueSelect editing mode for this row).
void FilterOverlay::render_single_row(::ui::Renderer& r, int panel_x, int x, int y,
                                       const char* label, const char* value,
                                       bool focused, bool editing) {
    const auto& th = r.mb_theme();

    // Cursor triangle — gold right-pointing ▶ at left edge of panel.
    if (focused) {
        const float marker_x  = static_cast<float>(panel_x + 6);
        const float marker_cy = static_cast<float>(y + kRowHeightPx / 2);
        r.mb_fill_triangle(
            marker_x,            marker_cy - kMarkerHalfH,
            marker_x,            marker_cy + kMarkerHalfH,
            marker_x + kMarkerW, marker_cy,
            th.accent, 1.0f);
    }

    const float fy = static_cast<float>(y + (kRowHeightPx - 28) / 2);

    // Label on left (always dim).
    r.mb_draw_text(label, static_cast<float>(x), fy, 14, th.dim);

    // Value on right side of the panel.
    // In editing mode: gold color and bracketed. Otherwise: standard fg.
    const int right_x = x + kPanelWidthPx - 2 * kPaddingX;
    if (editing) {
        std::string bracketed = std::string("[") + value + "]";
        // Right-align by measuring text width.
        const int tw = r.mb_text_width(bracketed.c_str(), 16);
        r.mb_draw_text(bracketed.c_str(),
                       static_cast<float>(right_x - tw),
                       fy, 16, th.accent);
    } else {
        const int tw = r.mb_text_width(value, 16);
        r.mb_draw_text(value,
                       static_cast<float>(right_x - tw),
                       fy, 16, th.fg);
    }
}

void FilterOverlay::render_reset_row(::ui::Renderer& r, int panel_x, int x, int y,
                                      bool focused) {
    const auto& th = r.mb_theme();

    if (focused) {
        const float marker_x  = static_cast<float>(panel_x + 6);
        const float marker_cy = static_cast<float>(y + kRowHeightPx / 2);
        r.mb_fill_triangle(
            marker_x,            marker_cy - kMarkerHalfH,
            marker_x,            marker_cy + kMarkerHalfH,
            marker_x + kMarkerW, marker_cy,
            th.accent, 1.0f);
    }

    const float fy = static_cast<float>(y + (kRowHeightPx - 28) / 2);
    r.mb_draw_text("RESET ALL", static_cast<float>(x), fy, 14, th.dim);

    // Hint text on the right.
    const char* hint = "press to clear";
    const int tw = r.mb_text_width(hint, 14);
    const int right_x = x + kPanelWidthPx - 2 * kPaddingX;
    r.mb_draw_text(hint, static_cast<float>(right_x - tw), fy, 14, th.fg);
}

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------
void FilterOverlay::render(::ui::Renderer& r, int /*screen_w*/, int screen_h) {
    if (state_ == State::Closed) return;

    const auto& th = r.mb_theme();

    const int panel_x = compute_panel_left_x();
    const int panel_y = kPanelTopMarginPx;
    const int panel_w = kPanelWidthPx;
    const int panel_h = screen_h - kPanelTopMarginPx - kPanelBottomMarginPx;

    const float fpx = static_cast<float>(panel_x);
    const float fpy = static_cast<float>(panel_y);
    const float fpw = static_cast<float>(panel_w);
    const float fph = static_cast<float>(panel_h);

    // Panel fill.
    r.mb_fill_rect(fpx, fpy, fpw, fph, th.bg_lift, 0.96f);
    // Accent borders.
    r.mb_fill_rect(fpx, fpy, fpw, 2.0f, th.accent);
    r.mb_fill_rect(fpx, fpy + fph - 2.0f, fpw, 2.0f, th.accent);
    r.mb_fill_rect(fpx, fpy, 2.0f, fph, th.accent);

    const int content_x = panel_x + kPaddingX;
    int y = panel_y + 28;

    // Title heading.
    r.mb_draw_title_text("FILTERS",
                         static_cast<float>(content_x),
                         static_cast<float>(y),
                         20, th.accent);
    y += 36;

    // Separator line beneath title.
    r.mb_fill_rect(static_cast<float>(panel_x + kPaddingX),
                   static_cast<float>(y),
                   static_cast<float>(panel_w - 2 * kPaddingX),
                   1.0f,
                   th.dim, 0.4f);
    y += 10;

    // ---- Row 0: Genre (single-line value selector) ----
    {
        const bool focused = (focus_row_ == 0);
        const bool editing = focused && (mode_ == Mode::ValueSelect);
        render_single_row(r, panel_x, content_x, y,
                          "GENRE", genre_display_name(working_.genre_mask),
                          focused, editing);
        y += kRowHeightPx;
    }

    // ---- Rows 1-5: value selectors ----
    struct RowDef {
        int         row_idx;
        const char* label;
        const char* const* values;
        int         count;
        int         selected;
    };
    const RowDef value_rows[] = {
        {1, "DECADE",     kDecadeLabels,   kNumDecades,   working_.decade},
        {2, "MIN RATING", kRatingLabels,   kNumRatings,   working_.min_rating},
        {3, "RUNTIME",    kRuntimeLabels,  kNumRuntimes,  working_.runtime},
        {4, "LANGUAGE",   kLanguageLabels, kNumLanguages, working_.language},
        {5, "SORT BY",    kSortLabels,     kNumSorts,     working_.sort},
    };

    for (const auto& row : value_rows) {
        const bool focused = (focus_row_ == row.row_idx);
        const bool editing = focused && (mode_ == Mode::ValueSelect);
        const char* val = (row.selected >= 0 && row.selected < row.count)
                              ? row.values[row.selected]
                              : "?";
        render_single_row(r, panel_x, content_x, y, row.label, val, focused, editing);
        y += kRowHeightPx;
    }

    // Separator before Reset.
    r.mb_fill_rect(static_cast<float>(panel_x + kPaddingX),
                   static_cast<float>(y),
                   static_cast<float>(panel_w - 2 * kPaddingX),
                   1.0f,
                   th.dim, 0.4f);
    y += 8;

    // ---- Row 6: Reset All ----
    render_reset_row(r, panel_x, content_x, y, (focus_row_ == 6));
    y += kRowHeightPx;

    // ---- Footer hint ----
    {
        const char* hint =
            (mode_ == Mode::ValueSelect)
                ? "PRESS: save  \xc2\xb7  BTN4: done"   // UTF-8 middle dot
                : "PRESS: edit  \xc2\xb7  BTN4: apply & close";
        const int hint_y = panel_y + panel_h - 28;
        r.mb_draw_text(hint,
                       static_cast<float>(content_x),
                       static_cast<float>(hint_y),
                       12, th.dim);
    }
}

}  // namespace media_browser::ui
