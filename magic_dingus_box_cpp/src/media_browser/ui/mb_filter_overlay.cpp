#include "media_browser/ui/mb_filter_overlay.h"

#include "media_browser/ui/mb_chrome.h"
#include "media_browser/ui/mb_ui_utils.h"
#include "ui/renderer.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace media_browser::ui {

namespace {

// ============================================================
// Panel geometry — mirrors the Library overlay exactly.
//   kOverlayPanelW         = 480    (Library value)
//   kOverlayPanelOpenX     = 760    (1280 - 40 bezel - 480)
//   kOverlayPanelTopY      = 120
//   kOverlayPanelBottomY   = 634
//   kOverlayPanelH         = 514
//   kOverlayPanelInnerPadX = 24
//   kOverlayPanelInnerPadY = 16
//   kOverlaySectionGap     = 18  — gap between section title and first row
//   kOverlayRowHeight      = 32  — per-row height in filter/sort sections
//   kOverlaySectionHeaderH = 28  — ZenDots section heading + gap below it
//   kPanelTitleFontPx      = 22  — ZenDots title ("FILTERS")
//   kSectionHeadingFontPx  = 14  — ZenDots section headers ("FILTER BY", "SORT BY")
//   kRowFontPx             = 16  — body-text font for row label + value
//   kStatFontPx            = 14  — not used in filter overlay (no stats block)
// ============================================================

constexpr int kScreenW               = 1280;
constexpr int kBezelInset            = 40;
// Gap between the panel's right edge and the bezel's inner edge.
// Keeps the right gold rule visible — without it the panel would slide
// under the bezel artwork and read as a 3-sided box.
constexpr int kOverlayPanelRightGap  = 20;

constexpr int kOverlayPanelW         = 480;
constexpr int kOverlayPanelOpenX     = kScreenW - kBezelInset - kOverlayPanelRightGap - kOverlayPanelW;  // 740
constexpr int kOverlayPanelClosedX   = kScreenW;                                  // off-screen right
constexpr int kOverlayPanelTopY      = 120;
constexpr int kOverlayPanelBottomY   = 634;
constexpr int kOverlayPanelH         = kOverlayPanelBottomY - kOverlayPanelTopY;  // 514
constexpr int kOverlayPanelInnerPadX = 24;
constexpr int kOverlayPanelInnerPadY = 16;

constexpr int kOverlaySectionGap     = 18;
constexpr int kOverlayRowHeight      = 32;
constexpr int kOverlaySectionHeaderH = 28;

constexpr int kPanelTitleFontPx      = 22;
constexpr int kSectionHeadingFontPx  = 14;
constexpr int kRowFontPx             = 16;

// Cursor triangle geometry — matches LibraryScreen overlay.
constexpr float kMarkerHalfH = 6.0f;
constexpr float kMarkerW     = 7.2f;

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
    if (state_ == State::Closed) return kOverlayPanelClosedX;
    if (state_ == State::Open)   return kOverlayPanelOpenX;
    const auto elapsed = duration_cast<milliseconds>(
        steady_clock::now() - anim_started_at_).count();
    if (state_ == State::SlidingIn) {
        const float t = std::clamp(
            static_cast<float>(elapsed) / static_cast<float>(kSlideInMs), 0.0f, 1.0f);
        return static_cast<int>(kOverlayPanelClosedX +
            ease_out_cubic(t) * (kOverlayPanelOpenX - kOverlayPanelClosedX));
    } else {  // SlidingOut
        const float t = std::clamp(
            static_cast<float>(elapsed) / static_cast<float>(kSlideOutMs), 0.0f, 1.0f);
        return static_cast<int>(kOverlayPanelOpenX +
            ease_in_cubic(t) * (kOverlayPanelClosedX - kOverlayPanelOpenX));
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
// render helpers — mirror LibraryScreen overlay's cursor + row draw pattern.
// ---------------------------------------------------------------------------

// Draw the gold right-pointing ▶ cursor marker at the left panel edge.
// marker_x is the absolute panel left-x (not content_x), so the triangle
// sits flush against the gold border rule — identical to Library overlay.
static void draw_cursor_marker(::ui::Renderer& r, int panel_x, int row_y,
                                const ::ui::Color& accent) {
    const float marker_x  = static_cast<float>(panel_x + 6);
    const float marker_cy = static_cast<float>(row_y + kOverlayRowHeight / 2);
    r.mb_fill_triangle(
        marker_x,            marker_cy - kMarkerHalfH,
        marker_x,            marker_cy + kMarkerHalfH,
        marker_x + kMarkerW, marker_cy,
        accent, 1.0f);
}

// Render a single filter row: label on the left (dim), value right-aligned.
// When focused + editing: value is gold + bracketed. Otherwise: fg.
// Label text-baseline matches Library overlay row style: row_y + kOverlayRowHeight - 10.
void FilterOverlay::render_single_row(::ui::Renderer& r, int panel_x, int x, int y,
                                       const char* label, const char* value,
                                       bool focused, bool editing) {
    const auto& th = r.mb_theme();

    if (focused) draw_cursor_marker(r, panel_x, y, th.accent);

    // Vertical centering: matches Library row baseline = row_y + kOverlayRowHeight - 10.
    const float fy_label = static_cast<float>(y + kOverlayRowHeight - 10);

    // Label on left, indented past cursor zone (content_x + 18 matches Library).
    r.mb_draw_text(label, static_cast<float>(x + 18), fy_label, kRowFontPx, th.dim);

    // Value right-aligned inside the panel content area.
    const int right_x = x + kOverlayPanelW - 2 * kOverlayPanelInnerPadX;
    if (editing) {
        std::string bracketed = std::string("[") + value + "]";
        const int tw = r.mb_text_width(bracketed.c_str(), kRowFontPx);
        r.mb_draw_text(bracketed.c_str(),
                       static_cast<float>(right_x - tw),
                       fy_label, kRowFontPx, th.accent);
    } else {
        const int tw = r.mb_text_width(value, kRowFontPx);
        r.mb_draw_text(value,
                       static_cast<float>(right_x - tw),
                       fy_label, kRowFontPx, th.fg);
    }
}

void FilterOverlay::render_reset_row(::ui::Renderer& r, int panel_x, int x, int y,
                                      bool focused) {
    const auto& th = r.mb_theme();

    if (focused) draw_cursor_marker(r, panel_x, y, th.accent);

    const float fy = static_cast<float>(y + kOverlayRowHeight - 10);
    r.mb_draw_text("RESET ALL", static_cast<float>(x + 18), fy,
                   kRowFontPx, focused ? th.accent : th.dim);

    // Right-aligned hint text.
    const char* hint = "press to clear";
    const int right_x = x + kOverlayPanelW - 2 * kOverlayPanelInnerPadX;
    const int tw = r.mb_text_width(hint, kSectionHeadingFontPx);
    r.mb_draw_text(hint, static_cast<float>(right_x - tw), fy,
                   kSectionHeadingFontPx, th.fg);
}

// ---------------------------------------------------------------------------
// render — panel chrome + content mirrors LibraryScreen overlay exactly.
// ---------------------------------------------------------------------------
void FilterOverlay::render(::ui::Renderer& r, int /*screen_w*/, int /*screen_h*/) {
    if (state_ == State::Closed) return;

    const auto& th = r.mb_theme();

    const int panel_x = compute_panel_left_x();
    const float fpx   = static_cast<float>(panel_x);
    const float fpy   = static_cast<float>(kOverlayPanelTopY);
    const float fpw   = static_cast<float>(kOverlayPanelW);
    const float fph   = static_cast<float>(kOverlayPanelH);

    // ---- Panel chrome — full 4-side gold box ----
    // Panel background — bg_lift, fully opaque.
    r.mb_fill_rect(fpx, fpy, fpw, fph, th.bg_lift);
    // Top edge: 2 px gold rule.
    r.mb_fill_rect(fpx, fpy, fpw, 2.0f, th.accent);
    // Bottom edge: 2 px gold rule.
    r.mb_fill_rect(fpx, fpy + fph - 2.0f, fpw, 2.0f, th.accent);
    // Left edge: 2 px gold rule.
    r.mb_fill_rect(fpx, fpy, 2.0f, fph, th.accent);
    // Right edge: 2 px gold rule (panel sits 20 px in from bezel so it's visible).
    r.mb_fill_rect(fpx + fpw - 2.0f, fpy, 2.0f, fph, th.accent);

    // ---- Content layout ----
    const int content_x = panel_x + kOverlayPanelInnerPadX;
    // Right-edge x for divider lines — mirrors Library's panel_x + W - padX.
    const int content_right = panel_x + kOverlayPanelW - kOverlayPanelInnerPadX;

    // ---- Title heading — "FILTERS" in gold ZenDots at 22 px ----
    const int title_baseline = kOverlayPanelTopY + kOverlayPanelInnerPadY +
                               kPanelTitleFontPx - 2;
    r.mb_draw_title_text("FILTERS",
                         static_cast<float>(content_x),
                         static_cast<float>(title_baseline),
                         kPanelTitleFontPx, th.accent);

    // 1 px dim divider beneath title — mirrors Library's divider1.
    const int divider1_y = title_baseline + 20;
    r.mb_draw_line(static_cast<float>(content_x),
                   static_cast<float>(divider1_y),
                   static_cast<float>(content_right),
                   static_cast<float>(divider1_y),
                   1.0f, th.dim, 0.5f);

    // ---- "FILTER BY" section ----
    const int filter_heading_y = divider1_y + kOverlaySectionGap;
    r.mb_draw_title_text("FILTER BY",
                         static_cast<float>(content_x),
                         static_cast<float>(filter_heading_y),
                         kSectionHeadingFontPx, th.accent);

    // Filter rows: GENRE, DECADE, MIN RATING, RUNTIME, LANGUAGE (rows 0-4).
    struct FilterRowDef {
        int         row_idx;
        const char* label;
        const char* const* values;
        int         count;
        int         selected;
    };
    const char* genre_val = genre_display_name(working_.genre_mask);
    const FilterRowDef filter_rows[] = {
        {0, "GENRE",      nullptr,         0,           0               },  // handled separately
        {1, "DECADE",     kDecadeLabels,   kNumDecades,  working_.decade },
        {2, "MIN RATING", kRatingLabels,   kNumRatings,  working_.min_rating},
        {3, "RUNTIME",    kRuntimeLabels,  kNumRuntimes, working_.runtime},
        {4, "LANGUAGE",   kLanguageLabels, kNumLanguages,working_.language},
    };

    const int filter_rows_y0 = filter_heading_y + kOverlaySectionHeaderH;
    for (int i = 0; i < 5; ++i) {
        const int row_y = filter_rows_y0 + i * kOverlayRowHeight;
        const bool focused = (focus_row_ == filter_rows[i].row_idx);
        const bool editing = focused && (mode_ == Mode::ValueSelect);

        const char* val;
        if (i == 0) {
            val = genre_val;
        } else {
            const auto& rd = filter_rows[i];
            val = (rd.selected >= 0 && rd.selected < rd.count)
                      ? rd.values[rd.selected]
                      : "?";
        }
        render_single_row(r, panel_x, content_x, row_y,
                          filter_rows[i].label, val, focused, editing);
    }

    // 1 px divider between FILTER BY and SORT BY sections.
    const int divider2_y = filter_rows_y0 + 5 * kOverlayRowHeight + 6;
    r.mb_draw_line(static_cast<float>(content_x),
                   static_cast<float>(divider2_y),
                   static_cast<float>(content_right),
                   static_cast<float>(divider2_y),
                   1.0f, th.dim, 0.5f);

    // ---- "SORT BY" section ----
    const int sort_heading_y = divider2_y + kOverlaySectionGap;
    r.mb_draw_title_text("SORT BY",
                         static_cast<float>(content_x),
                         static_cast<float>(sort_heading_y),
                         kSectionHeadingFontPx, th.accent);

    // Sort row (row 5): single row for sort-order selection.
    const int sort_row_y0 = sort_heading_y + kOverlaySectionHeaderH;
    {
        const bool focused = (focus_row_ == 5);
        const bool editing = focused && (mode_ == Mode::ValueSelect);
        const char* val = (working_.sort >= 0 && working_.sort < kNumSorts)
                              ? kSortLabels[working_.sort] : "?";
        render_single_row(r, panel_x, content_x, sort_row_y0,
                          "ORDER", val, focused, editing);
    }

    // 1 px divider before RESET ALL.
    const int divider3_y = sort_row_y0 + kOverlayRowHeight + 6;
    r.mb_draw_line(static_cast<float>(content_x),
                   static_cast<float>(divider3_y),
                   static_cast<float>(content_right),
                   static_cast<float>(divider3_y),
                   1.0f, th.dim, 0.5f);

    // ---- RESET ALL row (row 6) ----
    const int reset_row_y = divider3_y + kOverlaySectionGap / 2;
    render_reset_row(r, panel_x, content_x, reset_row_y, (focus_row_ == 6));

    // ---- Footer hint inside the panel — mirrors Library overlay ----
    const int hint_y = kOverlayPanelBottomY - kOverlayPanelInnerPadY;
    chrome::draw_hint_row(r, content_x, hint_y, {
        {chrome::HintIcon::Btn2Red,     "Close"},
        {chrome::HintIcon::Btn4Black,
         (mode_ == Mode::ValueSelect) ? "Done" : "Apply"},
        {chrome::HintIcon::RotaryNav,   "Rows"},
        {chrome::HintIcon::RotaryPress,
         (mode_ == Mode::ValueSelect) ? "Save" : "Edit"},
    });
}

}  // namespace media_browser::ui
