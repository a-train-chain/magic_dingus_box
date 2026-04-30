#include "media_browser/ui/mb_filter_overlay.h"

#include "ui/renderer.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace media_browser::ui {

namespace {

constexpr int kPanelLeftMarginPx = 0;
constexpr int kPanelTopMarginPx = 100;
constexpr int kPanelBottomMarginPx = 100;
constexpr int kRowHeightPx = 56;
constexpr int kPaddingX = 24;

// Cursor triangle geometry — matches the ▶ style used by LibraryScreen.
constexpr float kMarkerHalfH = 6.0f;
constexpr float kMarkerW     = 7.2f;

float ease_in_cubic(float t) { return t * t * t; }
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
    tab_ = tab;
    working_ = current;
    focus_row_ = 0;
    state_ = State::SlidingIn;
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
    if (state_ == State::Closed) return -kPanelWidthPx;
    if (state_ == State::Open)   return kPanelLeftMarginPx;
    const auto elapsed = duration_cast<milliseconds>(
        steady_clock::now() - anim_started_at_).count();
    if (state_ == State::SlidingIn) {
        const float t = std::clamp(
            static_cast<float>(elapsed) / static_cast<float>(kSlideInMs), 0.0f, 1.0f);
        const float eased = ease_out_cubic(t);
        return static_cast<int>(-kPanelWidthPx + eased * kPanelWidthPx);
    } else {  // SlidingOut
        const float t = std::clamp(
            static_cast<float>(elapsed) / static_cast<float>(kSlideOutMs), 0.0f, 1.0f);
        const float eased = ease_in_cubic(t);
        return static_cast<int>(-eased * kPanelWidthPx);
    }
}

bool FilterOverlay::on_rotate(int delta) {
    if (!is_input_active()) return false;
    if (delta == 0) return false;
    int new_focus = focus_row_ + (delta > 0 ? 1 : -1);
    if (new_focus < 0) new_focus = 0;
    if (new_focus >= kFocusableRowCount) new_focus = kFocusableRowCount - 1;
    focus_row_ = new_focus;
    return true;
}

bool FilterOverlay::on_select() {
    if (!is_input_active()) return false;

    switch (focus_row_) {
        case 0: {
            // Genre — cycle through single-genre selections.
            // Sequence: none -> Action(bit 0) -> Adventure(bit 1) -> ... -> War(bit 17) -> none
            const uint32_t mask = working_.genre_mask;
            if (mask == 0) {
                working_.genre_mask = 1u;  // Action
            } else {
                const int bit = __builtin_ctz(mask);
                const int nb = bit + 1;
                working_.genre_mask = (nb >= kNumGenres) ? 0u : (1u << nb);
            }
            break;
        }
        case 1: working_.decade     = (working_.decade + 1)     % kNumDecades;   break;
        case 2: working_.min_rating = (working_.min_rating + 1) % kNumRatings;   break;
        case 3: working_.runtime    = (working_.runtime + 1)    % kNumRuntimes;  break;
        case 4: working_.language   = (working_.language + 1)   % kNumLanguages; break;
        case 5: working_.sort       = (working_.sort + 1)       % kNumSorts;     break;
        case 6:
            working_ = FilterState{};
            break;
        default:
            return true;
    }

    if (on_commit_) on_commit_(working_, tab_);
    return true;
}

bool FilterOverlay::on_btn4_close() {
    if (!is_visible()) return false;
    close();
    return true;
}

void FilterOverlay::render(::ui::Renderer& r, int screen_w, int screen_h) {
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

    // Panel fill — bg_lift, near-opaque so text behind is suppressed.
    r.mb_fill_rect(fpx, fpy, fpw, fph, th.bg_lift, 0.96f);
    // Top rule in gold.
    r.mb_fill_rect(fpx, fpy, fpw, 2.0f, th.accent);
    // Bottom rule in gold.
    r.mb_fill_rect(fpx, fpy + fph - 2.0f, fpw, 2.0f, th.accent);
    // Right rule in gold (left edge is at screen edge, no border needed there).
    r.mb_fill_rect(fpx + fpw - 2.0f, fpy, 2.0f, fph, th.accent);

    const int content_x = panel_x + kPaddingX;
    int y = panel_y + 32;

    // Title heading — gold ZenDots font.
    r.mb_draw_title_text("FILTERS",
                         static_cast<float>(content_x),
                         static_cast<float>(y),
                         22, th.accent);
    y += 40;

    // ---- Row 0: Genre ----
    if (focus_row_ == 0) {
        const float marker_x  = static_cast<float>(panel_x + 6);
        const float marker_cy = static_cast<float>(y + 12);
        r.mb_fill_triangle(
            marker_x,              marker_cy - kMarkerHalfH,
            marker_x,              marker_cy + kMarkerHalfH,
            marker_x + kMarkerW,   marker_cy,
            th.accent, 1.0f);
    }
    render_genre_row(r, content_x, y);
    y += kRowHeightPx;

    // ---- Rows 1-5: value selectors ----
    struct ValueRowDef {
        int          row_idx;
        const char*  label;
        const char* const* values;
        int          count;
        int          selected;
    };
    const ValueRowDef value_rows[] = {
        {1, "DECADE",     kDecadeLabels,   kNumDecades,   working_.decade},
        {2, "MIN RATING", kRatingLabels,   kNumRatings,   working_.min_rating},
        {3, "RUNTIME",    kRuntimeLabels,  kNumRuntimes,  working_.runtime},
        {4, "LANGUAGE",   kLanguageLabels, kNumLanguages, working_.language},
        {5, "SORT BY",    kSortLabels,     kNumSorts,     working_.sort},
    };

    for (const auto& row : value_rows) {
        if (focus_row_ == row.row_idx) {
            const float marker_x  = static_cast<float>(panel_x + 6);
            const float marker_cy = static_cast<float>(y + 12);
            r.mb_fill_triangle(
                marker_x,              marker_cy - kMarkerHalfH,
                marker_x,              marker_cy + kMarkerHalfH,
                marker_x + kMarkerW,   marker_cy,
                th.accent, 1.0f);
        }
        std::vector<std::string> vs;
        vs.reserve(row.count);
        for (int i = 0; i < row.count; ++i) vs.emplace_back(row.values[i]);
        render_value_row(r, content_x, y, row.label, vs, row.selected);
        y += kRowHeightPx;
    }

    // ---- Row 6: Reset ----
    if (focus_row_ == 6) {
        const float marker_x  = static_cast<float>(panel_x + 6);
        const float marker_cy = static_cast<float>(y + 12);
        r.mb_fill_triangle(
            marker_x,              marker_cy - kMarkerHalfH,
            marker_x,              marker_cy + kMarkerHalfH,
            marker_x + kMarkerW,   marker_cy,
            th.accent, 1.0f);
    }
    render_reset_row(r, content_x, y);
}

void FilterOverlay::render_genre_row(::ui::Renderer& r, int x, int y) {
    const auto& th = r.mb_theme();

    r.mb_draw_text("GENRE",
                   static_cast<float>(x),
                   static_cast<float>(y),
                   14, th.dim);

    constexpr int kChipPad = 6;
    constexpr int kChipH   = 22;
    const int max_x = x + kPanelWidthPx - 2 * kPaddingX;

    int cx = x;
    int cy = y + 18;

    for (int i = 0; i < kNumGenres; ++i) {
        const bool on = (working_.genre_mask & (1u << i)) != 0;
        const int chip_w = r.mb_text_width(kGenres[i].display, 12) + kChipPad * 2;
        if (cx + chip_w > max_x) {
            cx = x;
            cy += kChipH + 4;
        }
        if (on) {
            r.mb_fill_rect(static_cast<float>(cx), static_cast<float>(cy),
                           static_cast<float>(chip_w), static_cast<float>(kChipH),
                           th.accent, 1.0f);
            r.mb_draw_text(kGenres[i].display,
                           static_cast<float>(cx + kChipPad),
                           static_cast<float>(cy + 4),
                           12, th.bg);
        } else {
            r.mb_fill_rect(static_cast<float>(cx), static_cast<float>(cy),
                           static_cast<float>(chip_w), static_cast<float>(kChipH),
                           th.bg_lift, 0.6f);
            r.mb_draw_text(kGenres[i].display,
                           static_cast<float>(cx + kChipPad),
                           static_cast<float>(cy + 4),
                           12, th.dim);
        }
        cx += chip_w + 4;
    }
}

void FilterOverlay::render_value_row(::ui::Renderer& r, int x, int y,
                                      const std::string& label,
                                      const std::vector<std::string>& values,
                                      int selected_index) {
    const auto& th = r.mb_theme();

    r.mb_draw_text(label,
                   static_cast<float>(x),
                   static_cast<float>(y),
                   14, th.dim);
    if (selected_index >= 0 && selected_index < static_cast<int>(values.size())) {
        r.mb_draw_text(values[selected_index],
                       static_cast<float>(x),
                       static_cast<float>(y + 18),
                       16, th.fg);
    }
}

void FilterOverlay::render_reset_row(::ui::Renderer& r, int x, int y) {
    const auto& th = r.mb_theme();

    r.mb_draw_text("RESET ALL",
                   static_cast<float>(x),
                   static_cast<float>(y),
                   14, th.dim);
    r.mb_draw_text("(press to clear filters)",
                   static_cast<float>(x),
                   static_cast<float>(y + 18),
                   14, th.fg);
}

}  // namespace media_browser::ui
