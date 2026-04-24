#include "media_browser/ui/library_screen.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>

#include "media_browser/radarr/radarr_client.h"
#include "ui/renderer.h"
#include "ui/theme.h"

namespace media_browser::ui {

namespace {

// --- Layout constants (pixels) -----------------------------------------

constexpr float kTopBarHeight        = 56.0f;
constexpr float kBottomBarHeight     = 40.0f;
constexpr float kGridPaddingX        = 48.0f;
constexpr float kGridPaddingTop      = 24.0f;
constexpr float kCellPadding         = 18.0f;
constexpr float kPosterW             = 220.0f;
constexpr float kPosterH             = 330.0f;
constexpr float kLabelAreaH          = 56.0f;
constexpr float kCellW               = kPosterW;
constexpr float kCellH               = kPosterH + kLabelAreaH;
constexpr float kOutlineThickness    = 4.0f;
constexpr float kChipPaddingX        = 14.0f;
constexpr float kChipPaddingY        = 6.0f;
constexpr float kChipGap             = 10.0f;
constexpr float kDotRadius           = 8.0f;
constexpr float kDotInset            = 10.0f;

// Deterministic colored tint for a tmdb_id, used as a poster placeholder.
// Mirrors BrowseScreen's scheme so the same movie looks the same wherever
// it's shown.
::ui::Color poster_tint_for_tmdb(int tmdb_id) {
    uint32_t h = static_cast<uint32_t>(tmdb_id) * 2654435761u;  // Knuth hash
    uint8_t r = 64 + static_cast<uint8_t>((h >>  0) & 0x7F);
    uint8_t g = 40 + static_cast<uint8_t>((h >>  8) & 0x5F);
    uint8_t b = 80 + static_cast<uint8_t>((h >> 16) & 0x7F);
    return {r, g, b, 255};
}

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

// True if the case-insensitive prefix of `s` matches `prefix`.
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

    // --- Top bar ------------------------------------------------------
    r.mb_fill_rect(0.0f, 0.0f, w, kTopBarHeight, th.bg, 0.75f);
    r.mb_fill_rect(0.0f, kTopBarHeight - 1.0f, w, 1.0f, th.dim, 0.6f);

    // Title "Library"
    int title_size = th.font_large_size;
    int title_baseline = r.mb_text_baseline(title_size);
    float title_y = (kTopBarHeight / 2.0f) - (title_size / 2.0f)
                  + static_cast<float>(title_baseline);
    r.mb_draw_text("Library", kGridPaddingX, title_y, title_size,
                   th.accent, 1.0f);
    int title_w = r.mb_text_width("Library", title_size);

    // Count "(N movies)"
    {
        int count_size = th.font_medium_size;
        int count_baseline = r.mb_text_baseline(count_size);
        std::ostringstream cs;
        size_t shown = view_.size();
        cs << "(" << shown << (shown == 1 ? " movie" : " movies") << ")";
        std::string count_text = cs.str();
        float count_x = kGridPaddingX + static_cast<float>(title_w) + 18.0f;
        float count_y = (kTopBarHeight / 2.0f) - (count_size / 2.0f)
                      + static_cast<float>(count_baseline);
        r.mb_draw_text(count_text, count_x, count_y, count_size, th.dim, 0.85f);
    }

    // Filter chips, right-aligned. Lay out right-to-left so the rightmost
    // chip hugs the right margin.
    int chip_size = th.font_small_size;
    int chip_baseline = r.mb_text_baseline(chip_size);

    // First, measure each chip's label width so we know where to start.
    float chip_label_w[kNumFilters];
    float total_chips_w = 0.0f;
    for (int i = 0; i < kNumFilters; ++i) {
        chip_label_w[i] = static_cast<float>(
            r.mb_text_width(label_for_filter(static_cast<Filter>(i)), chip_size));
        total_chips_w += chip_label_w[i] + 2.0f * kChipPaddingX;
    }
    total_chips_w += (kNumFilters - 1) * kChipGap;

    float chips_start_x = w - kGridPaddingX - total_chips_w;
    float chips_y = (kTopBarHeight - (chip_size + 2.0f * kChipPaddingY)) / 2.0f;
    float chip_x = chips_start_x;

    for (int i = 0; i < kNumFilters; ++i) {
        Filter f = static_cast<Filter>(i);
        bool is_active  = (f == filter_);
        bool is_focused = (focus_ == Focus::FilterStrip && i == filter_cursor_);

        float cw = chip_label_w[i] + 2.0f * kChipPaddingX;
        float ch = chip_size + 2.0f * kChipPaddingY;

        // Chip background — active gets a filled accent tint, others get
        // a dim/transparent fill.
        ::ui::Color bg_color = is_active ? th.accent : th.dim;
        float bg_alpha = is_active ? 0.35f : 0.20f;
        r.mb_fill_rect(chip_x, chips_y, cw, ch, bg_color, bg_alpha);

        // Focus outline (only when the strip has focus).
        if (is_focused) {
            r.mb_stroke_rect(chip_x, chips_y, cw, ch, 2.0f, th.accent, 1.0f);
        } else {
            r.mb_stroke_rect(chip_x, chips_y, cw, ch, 1.0f, th.dim, 0.5f);
        }

        // Label
        float label_x = chip_x + kChipPaddingX;
        float label_y = chips_y + kChipPaddingY + static_cast<float>(chip_baseline);
        ::ui::Color label_color = is_active ? th.accent : th.fg;
        float label_alpha = is_focused ? 1.0f : (is_active ? 1.0f : 0.85f);
        r.mb_draw_text(label_for_filter(f), label_x, label_y, chip_size,
                       label_color, label_alpha);

        chip_x += cw + kChipGap;
    }

    // --- Poster grid --------------------------------------------------
    float grid_top    = kTopBarHeight + kGridPaddingTop;
    float grid_bottom = h - kBottomBarHeight;
    float grid_h      = grid_bottom - grid_top;

    int visible_rows = std::max(1,
        static_cast<int>(grid_h / (kCellH + kCellPadding)));

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

    // Empty-state message
    if (view_.empty()) {
        std::string msg;
        if (!loaded_) {
            msg = "Loading library...";
        } else if (library_.empty()) {
            msg = "No movies in library yet — add some from Browse";
        } else {
            msg = "No movies match this filter";
        }
        int msg_size = th.font_large_size;
        int msg_w = r.mb_text_width(msg, msg_size);
        float msg_x = (w - static_cast<float>(msg_w)) / 2.0f;
        float msg_y = grid_top + grid_h / 2.0f
                    + static_cast<float>(r.mb_text_baseline(msg_size));
        r.mb_draw_text(msg, msg_x, msg_y, msg_size, th.dim, 0.9f);
    }

    // Column spacing so the grid spans the interior evenly.
    float grid_interior_w = w - 2.0f * kGridPaddingX;
    float col_gap = (grid_interior_w - kGridCols * kCellW)
                    / std::max(1.0f, static_cast<float>(kGridCols - 1));
    if (col_gap < kCellPadding) col_gap = kCellPadding;

    for (int row = scroll_row_; row < end_row; ++row) {
        for (int col = 0; col < kGridCols; ++col) {
            int idx = row * kGridCols + col;
            if (idx >= static_cast<int>(view_.size())) break;
            const Movie& m = *view_[idx];

            float cell_x = kGridPaddingX + col * (kCellW + col_gap);
            float cell_y = grid_top + (row - scroll_row_) * (kCellH + kCellPadding);

            // Poster: real artwork if cached, else deterministic tint.
            ::ui::Color tint = poster_tint_for_tmdb(m.tmdb_id);
            r.mb_draw_poster_or_tint(m.poster_url,
                                     cell_x, cell_y, kPosterW, kPosterH,
                                     tint, 1.0f);
            r.mb_stroke_rect(cell_x, cell_y, kPosterW, kPosterH, 1.0f,
                             th.dim, 0.4f);

            // Focus outline
            bool focused = (focus_ == Focus::PosterGrid && idx == grid_cursor_);
            if (focused) {
                r.mb_stroke_rect(cell_x - kOutlineThickness / 2.0f,
                                 cell_y - kOutlineThickness / 2.0f,
                                 kPosterW + kOutlineThickness,
                                 kPosterH + kOutlineThickness,
                                 kOutlineThickness,
                                 th.accent, 1.0f);
            }

            // State indicator dot, top-right corner of the poster.
            FileState state = classify(m);
            ::ui::Color dot_color;
            switch (state) {
                case FileState::HasGoodFile:      dot_color = th.highlight1; break; // green
                case FileState::UpgradeAvailable: dot_color = th.highlight3; break; // gold/orange
                case FileState::MissingFile:      dot_color = th.highlight2; break; // red
            }
            float dot_d = kDotRadius * 2.0f;
            float dot_x = cell_x + kPosterW - kDotInset - dot_d;
            float dot_y = cell_y + kDotInset;
            // A dark halo gives the dot contrast regardless of the
            // underlying poster tint.
            r.mb_fill_rect(dot_x - 2.0f, dot_y - 2.0f,
                           dot_d + 4.0f, dot_d + 4.0f, th.bg, 0.85f);
            r.mb_fill_rect(dot_x, dot_y, dot_d, dot_d, dot_color, 1.0f);

            // Title (truncated to cell width)
            int t_size = th.font_medium_size;
            int t_baseline = r.mb_text_baseline(t_size);
            std::string title = truncate_to_width(
                r, m.title.empty() ? "Untitled" : m.title, t_size, kCellW);
            float t_y = cell_y + kPosterH + 8.0f + static_cast<float>(t_baseline);
            r.mb_draw_text(title, cell_x, t_y, t_size, th.fg,
                           focused ? 1.0f : 0.9f);

            // Year
            if (m.year > 0) {
                std::string year = std::to_string(m.year);
                int y_size = th.font_small_size;
                int y_baseline = r.mb_text_baseline(y_size);
                float y_y = t_y + static_cast<float>(t_size) * 0.9f
                          + static_cast<float>(y_baseline) * 0.2f;
                r.mb_draw_text(year, cell_x, y_y, y_size, th.dim, 0.9f);
            }
        }
    }

    // --- Bottom hint bar ---------------------------------------------
    float bar_y = h - kBottomBarHeight;
    r.mb_fill_rect(0.0f, bar_y, w, kBottomBarHeight, th.bg, 0.75f);
    r.mb_fill_rect(0.0f, bar_y, w, 1.0f, th.dim, 0.6f);

    const std::string hint =
        "Select: Open details   LEFT/RIGHT: Filter   Menu: Back";
    int hint_size = th.font_small_size;
    int hint_baseline = r.mb_text_baseline(hint_size);
    int hint_w = r.mb_text_width(hint, hint_size);
    float hint_x = (w - static_cast<float>(hint_w)) / 2.0f;
    float hint_y = bar_y + (kBottomBarHeight / 2.0f) - (hint_size / 2.0f)
                 + static_cast<float>(hint_baseline);
    r.mb_draw_text(hint, hint_x, hint_y, hint_size, th.fg, 0.85f);
}

}  // namespace media_browser::ui
