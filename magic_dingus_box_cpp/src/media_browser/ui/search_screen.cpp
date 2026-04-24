#include "media_browser/ui/search_screen.h"

#include <algorithm>
#include <cstdint>
#include <string>

#include "media_browser/radarr/radarr_client.h"
#include "ui/renderer.h"
#include "ui/theme.h"

namespace media_browser::ui {

namespace {

// --- Layout constants (fractions of screen height are chosen so the
// keyboard + query box eat roughly the top 40%, results the bottom 60%).
constexpr float kTopFrac             = 0.40f;
constexpr float kPaddingX            = 48.0f;
constexpr float kQueryBoxHeight      = 56.0f;
constexpr float kQueryBoxMarginTop   = 20.0f;
constexpr float kQueryBoxMarginBot   = 16.0f;
constexpr float kKeyGap              = 6.0f;
constexpr float kKbPaddingBottom     = 18.0f;

// Results grid (smaller than BrowseScreen's 4-col layout).
constexpr float kGridPaddingTop      = 18.0f;
constexpr float kBottomBarHeight     = 40.0f;
constexpr float kCellPadding         = 16.0f;
constexpr float kPosterW             = 180.0f;
constexpr float kPosterH             = 270.0f;
constexpr float kLabelAreaH          = 48.0f;
constexpr float kCellW               = kPosterW;
constexpr float kCellH               = kPosterH + kLabelAreaH;
constexpr float kOutlineThickness    = 4.0f;

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

// Friendlier on-key labels for the special keys that the widget stores
// as all-caps sentinels. Keeps the rendered text short enough to fit
// inside the small cells.
const char* display_label(const std::string& key) {
    if (key == "SPACE")  return "SPACE";
    if (key == "BACK")   return "BKSP";
    if (key == "ENTER")  return "OK";
    if (key == "CANCEL") return "X";
    return key.c_str();
}

}  // namespace

SearchScreen::SearchScreen(RadarrClient& radarr) : radarr_(radarr) {}

void SearchScreen::enter() {
    // Start fresh each time the user opens Search. If they navigated in
    // from Browse, they want an empty query.
    query_.clear();
    last_queried_.clear();
    results_.clear();
    grid_cursor_ = 0;
    scroll_row_ = 0;
    selected_tmdb_id_ = 0;
    searching_ = false;
    focus_ = Focus::Keyboard;
    last_input_time_ = std::chrono::steady_clock::time_point::min();

    // Open the virtual keyboard with empty text. We don't use the
    // on_enter/on_cancel callbacks — this screen handles its own flow —
    // but the widget requires the open() call to become active and
    // initialize its selected_row/col.
    keyboard_.open("", "Search Movies",
                   /*on_enter=*/nullptr, /*on_cancel=*/nullptr);
}

void SearchScreen::run_lookup_if_due() {
    if (query_ == last_queried_) return;
    if (last_input_time_ == std::chrono::steady_clock::time_point::min()) return;
    auto now = std::chrono::steady_clock::now();
    auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - last_input_time_).count();
    if (idle_ms < kDebounceMs) return;

    last_queried_ = query_;
    if (query_.empty()) {
        results_.clear();
        grid_cursor_ = 0;
        scroll_row_ = 0;
        searching_ = false;
        return;
    }
    searching_ = true;
    // RadarrClient::lookup is synchronous. At single-user scale this is
    // fine: it blocks the UI for the duration of one Radarr HTTP round
    // trip, which is typically under a second.
    results_ = radarr_.lookup(query_);
    searching_ = false;
    grid_cursor_ = 0;
    scroll_row_ = 0;
}

void SearchScreen::update() {
    run_lookup_if_due();
}

Screen SearchScreen::handle_input(const std::vector<platform::InputEvent>& events) {
    for (const auto& e : events) {
        // BTN4 / Menu: back to Browse (back stack — NOT all the way out
        // to the kiosk main menu; Browse handles that).
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            return Screen::Browse;
        }

        // Horizontal nav: always to the keyboard regardless of focus —
        // the results grid navigates with left/right too, but we model
        // the grid as only reachable via DOWN from the keyboard's
        // bottom row, and LEFT/RIGHT there still makes sense as grid
        // step. Implementation: if Focus::Results, use results
        // navigation; otherwise delegate to keyboard.
        if (e.action == platform::InputAction::ROTATE) {
            int delta = e.delta;
            if (delta == 0) continue;
            if (focus_ == Focus::Keyboard) {
                if (delta > 0) keyboard_.navigate_right();
                else           keyboard_.navigate_left();
            } else {
                // Results grid: horizontal step.
                if (results_.empty()) continue;
                int n = static_cast<int>(results_.size());
                grid_cursor_ = std::clamp(grid_cursor_ + delta, 0, n - 1);
            }
            continue;
        }

        // Vertical nav: special edge handling to hop between regions.
        if (e.action == platform::InputAction::ROTATE_VERTICAL) {
            int delta = e.delta;
            if (delta == 0) continue;
            if (focus_ == Focus::Keyboard) {
                if (delta > 0) {
                    // DOWN. If we're on the keyboard's last row AND we
                    // have results, hop to the results grid. Otherwise
                    // navigate down within the keyboard.
                    int last_row = static_cast<int>(keyboard_.get_layout().size()) - 1;
                    if (keyboard_.get_selected_row() == last_row &&
                        !results_.empty()) {
                        focus_ = Focus::Results;
                        grid_cursor_ = 0;
                        scroll_row_ = 0;
                    } else {
                        keyboard_.navigate_down();
                    }
                } else {
                    keyboard_.navigate_up();
                }
            } else {
                // In results grid.
                int row = grid_cursor_ / kGridCols;
                int col = grid_cursor_ % kGridCols;
                if (delta < 0 && row == 0) {
                    // UP from top row: focus back to keyboard.
                    focus_ = Focus::Keyboard;
                } else {
                    int new_row = row + (delta > 0 ? 1 : -1);
                    int max_row = results_.empty()
                                  ? 0
                                  : (static_cast<int>(results_.size()) - 1) / kGridCols;
                    new_row = std::clamp(new_row, 0, max_row);
                    int new_idx = new_row * kGridCols + col;
                    if (new_idx < static_cast<int>(results_.size())) {
                        grid_cursor_ = new_idx;
                    } else if (!results_.empty()) {
                        grid_cursor_ = static_cast<int>(results_.size()) - 1;
                    }
                }
            }
            continue;
        }

        // Select: keyboard keystroke or poster activation.
        if (e.action == platform::InputAction::SELECT && e.pressed) {
            if (focus_ == Focus::Keyboard) {
                // Snapshot the text buffer; call select() (which may
                // append a char, backspace, toggle caps/symbols, or
                // fire the ENTER callback); if the buffer changed,
                // mark now as the last-input time for the debounce.
                std::string before = keyboard_.get_text();
                keyboard_.select();
                std::string after = keyboard_.get_text();
                if (after != before) {
                    query_ = after;
                    last_input_time_ = std::chrono::steady_clock::now();
                }
                continue;
            }
            // Results grid: pick this poster and hand off to Detail.
            if (!results_.empty() &&
                grid_cursor_ >= 0 &&
                grid_cursor_ < static_cast<int>(results_.size())) {
                selected_tmdb_id_ = results_[grid_cursor_].tmdb_id;
                return Screen::Detail;
            }
            continue;
        }
    }

    return Screen::Search;
}

void SearchScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    const ::ui::Theme& th = r.mb_theme();

    r.mb_fill_background();

    // --- Region layout -------------------------------------------------
    float top_h = screen_h * kTopFrac;

    // --- Query box (above keyboard) -----------------------------------
    float qb_x = kPaddingX;
    float qb_y = kQueryBoxMarginTop;
    float qb_w = screen_w - 2.0f * kPaddingX;
    float qb_h = kQueryBoxHeight;

    r.mb_fill_rect(qb_x, qb_y, qb_w, qb_h, th.bg, 0.85f);
    r.mb_stroke_rect(qb_x, qb_y, qb_w, qb_h, 2.0f,
                     focus_ == Focus::Keyboard ? th.accent : th.dim,
                     1.0f);

    int query_font = th.font_large_size;
    int query_baseline = r.mb_text_baseline(query_font);
    float query_text_y = qb_y + (qb_h / 2.0f) - (query_font / 2.0f)
                       + static_cast<float>(query_baseline);

    // Blinking cursor, same visual pattern the existing virtual-keyboard
    // renderer uses.
    bool cursor_on = (std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count() / 500) % 2 == 0;
    std::string display = query_.empty() ? std::string("Type to search movies...")
                                         : query_;
    if (!query_.empty() && cursor_on) display += "_";

    r.mb_draw_text(display, qb_x + 16.0f, query_text_y, query_font,
                   query_.empty() ? th.dim : th.fg,
                   query_.empty() ? 0.6f : 1.0f);

    // "searching..." indicator, right-aligned inside the query box.
    if (searching_) {
        const std::string busy = "searching...";
        int busy_size = th.font_small_size;
        int busy_baseline = r.mb_text_baseline(busy_size);
        float busy_y = qb_y + (qb_h / 2.0f) - (busy_size / 2.0f)
                     + static_cast<float>(busy_baseline);
        float busy_w = static_cast<float>(r.mb_text_width(busy, busy_size));
        r.mb_draw_text(busy, qb_x + qb_w - busy_w - 16.0f, busy_y,
                       busy_size, th.accent, 0.9f);
    }

    // --- Virtual keyboard ---------------------------------------------
    float kb_top   = qb_y + qb_h + kQueryBoxMarginBot;
    float kb_bot   = top_h - kKbPaddingBottom;
    float kb_h     = std::max(120.0f, kb_bot - kb_top);
    float kb_x     = kPaddingX;
    float kb_w     = screen_w - 2.0f * kPaddingX;

    const auto& layout = keyboard_.get_layout();
    int nrows = static_cast<int>(layout.size());
    float row_h = (kb_h - kKeyGap * std::max(0, nrows - 1))
                / static_cast<float>(std::max(1, nrows));

    int kb_font = th.font_medium_size;
    int kb_baseline = r.mb_text_baseline(kb_font);

    bool kb_focused = (focus_ == Focus::Keyboard);

    for (int row = 0; row < nrows; ++row) {
        const auto& keys = layout[row];
        int ncols = static_cast<int>(keys.size());
        float col_w = (kb_w - kKeyGap * std::max(0, ncols - 1))
                    / static_cast<float>(std::max(1, ncols));
        float ky = kb_top + row * (row_h + kKeyGap);

        for (int col = 0; col < ncols; ++col) {
            float kx = kb_x + col * (col_w + kKeyGap);
            bool is_selected = kb_focused &&
                               row == keyboard_.get_selected_row() &&
                               col == keyboard_.get_selected_col();

            // Key background
            const ::ui::Color& bg = is_selected ? th.accent : th.action;
            r.mb_fill_rect(kx, ky, col_w, row_h, bg,
                           kb_focused ? 0.95f : 0.45f);
            r.mb_stroke_rect(kx, ky, col_w, row_h, 1.0f, th.dim, 0.5f);

            // Key label (centered).
            const char* lbl = display_label(keys[col]);
            int label_size = kb_font;
            if (std::string(lbl).length() > 1) label_size = th.font_small_size;
            int label_baseline = (label_size == kb_font)
                                 ? kb_baseline
                                 : r.mb_text_baseline(label_size);
            float tw = static_cast<float>(r.mb_text_width(lbl, label_size));
            float tx = kx + (col_w - tw) / 2.0f;
            float ty = ky + (row_h / 2.0f) - (label_size / 2.0f)
                     + static_cast<float>(label_baseline);

            const ::ui::Color& fg = is_selected ? th.bg : th.fg;
            r.mb_draw_text(lbl, tx, ty, label_size, fg,
                           kb_focused ? 1.0f : 0.7f);
        }
    }

    // Separator between keyboard region and results.
    float sep_y = top_h;
    r.mb_fill_rect(0.0f, sep_y, static_cast<float>(screen_w), 1.0f,
                   th.dim, 0.6f);

    // --- Results grid --------------------------------------------------
    float grid_top    = top_h + kGridPaddingTop;
    float grid_bottom = static_cast<float>(screen_h) - kBottomBarHeight;
    float grid_h      = grid_bottom - grid_top;

    int visible_rows = std::max(1,
        static_cast<int>(grid_h / (kCellH + kCellPadding)));

    if (focus_ == Focus::Results && !results_.empty()) {
        int focused_row = grid_cursor_ / kGridCols;
        if (focused_row < scroll_row_) scroll_row_ = focused_row;
        if (focused_row >= scroll_row_ + visible_rows) {
            scroll_row_ = focused_row - visible_rows + 1;
        }
    }

    int total_rows = results_.empty() ? 0
                   : (static_cast<int>(results_.size()) - 1) / kGridCols + 1;
    int end_row = std::min(total_rows, scroll_row_ + visible_rows);

    float grid_interior_w = static_cast<float>(screen_w) - 2.0f * kPaddingX;
    float col_gap = (grid_interior_w - kGridCols * kCellW)
                    / std::max(1.0f, static_cast<float>(kGridCols - 1));
    if (col_gap < kCellPadding) col_gap = kCellPadding;

    // Empty-state messaging.
    if (results_.empty()) {
        std::string msg;
        if (query_.empty()) {
            msg = "Type a title to see results";
        } else if (searching_) {
            msg = "Searching...";
        } else if (query_ != last_queried_) {
            // User typed but debounce hasn't fired yet.
            msg = "...";
        } else {
            msg = "No matches for \"" + query_ + "\"";
        }
        int msg_size = th.font_large_size;
        int msg_w = r.mb_text_width(msg, msg_size);
        float msg_x = (static_cast<float>(screen_w)
                       - static_cast<float>(msg_w)) / 2.0f;
        float msg_y = grid_top + grid_h / 2.0f
                    + static_cast<float>(r.mb_text_baseline(msg_size));
        r.mb_draw_text(msg, msg_x, msg_y, msg_size, th.dim, 0.8f);
    }

    for (int row = scroll_row_; row < end_row; ++row) {
        for (int col = 0; col < kGridCols; ++col) {
            int idx = row * kGridCols + col;
            if (idx >= static_cast<int>(results_.size())) break;
            const auto& m = results_[idx];

            float cell_x = kPaddingX + col * (kCellW + col_gap);
            float cell_y = grid_top + (row - scroll_row_) * (kCellH + kCellPadding);

            ::ui::Color tint = poster_tint_for_tmdb(m.tmdb_id);
            r.mb_fill_rect(cell_x, cell_y, kPosterW, kPosterH, tint, 1.0f);
            r.mb_stroke_rect(cell_x, cell_y, kPosterW, kPosterH, 1.0f,
                             th.dim, 0.4f);

            bool focused = (focus_ == Focus::Results && idx == grid_cursor_);
            if (focused) {
                r.mb_stroke_rect(cell_x - kOutlineThickness / 2.0f,
                                 cell_y - kOutlineThickness / 2.0f,
                                 kPosterW + kOutlineThickness,
                                 kPosterH + kOutlineThickness,
                                 kOutlineThickness,
                                 th.accent, 1.0f);
            }

            int title_size = th.font_small_size;
            int title_baseline = r.mb_text_baseline(title_size);
            std::string title = truncate_to_width(
                r, m.title.empty() ? "Untitled" : m.title,
                title_size, kCellW);
            float title_y = cell_y + kPosterH + 8.0f
                          + static_cast<float>(title_baseline);
            r.mb_draw_text(title, cell_x, title_y, title_size, th.fg,
                           focused ? 1.0f : 0.9f);

            if (m.year > 0) {
                std::string year = std::to_string(m.year);
                int year_size = th.font_small_size;
                int year_baseline = r.mb_text_baseline(year_size);
                float year_y = title_y + static_cast<float>(year_size) * 0.9f
                             + static_cast<float>(year_baseline) * 0.2f;
                r.mb_draw_text(year, cell_x, year_y, year_size, th.dim, 0.8f);
            }
        }
    }

    // --- Bottom hint bar ----------------------------------------------
    float bar_y = static_cast<float>(screen_h) - kBottomBarHeight;
    r.mb_fill_rect(0.0f, bar_y, static_cast<float>(screen_w), kBottomBarHeight,
                   th.bg, 0.75f);
    r.mb_fill_rect(0.0f, bar_y, static_cast<float>(screen_w), 1.0f,
                   th.dim, 0.6f);

    const std::string hint =
        (focus_ == Focus::Keyboard)
            ? "Type: SELECT   Down: into results   Back: Menu"
            : "Open: SELECT   Up: back to keyboard   Back: Menu";
    int hint_size = th.font_small_size;
    int hint_baseline = r.mb_text_baseline(hint_size);
    int hint_w = r.mb_text_width(hint, hint_size);
    float hint_x = (static_cast<float>(screen_w)
                    - static_cast<float>(hint_w)) / 2.0f;
    float hint_y = bar_y + (kBottomBarHeight / 2.0f) - (hint_size / 2.0f)
                 + static_cast<float>(hint_baseline);
    r.mb_draw_text(hint, hint_x, hint_y, hint_size, th.fg, 0.85f);
}

}  // namespace media_browser::ui
