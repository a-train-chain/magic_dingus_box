#include "media_browser/ui/browse_screen.h"

#include <algorithm>
#include <cstdint>
#include <string>

#include <spdlog/spdlog.h>

#include "media_browser/radarr/radarr_client.h"
#include "ui/renderer.h"
#include "ui/theme.h"

namespace media_browser::ui {

namespace {

// Poster card layout (fixed pixel sizes — real movie posters are ~2:3).
constexpr float kCategoryStripHeight = 72.0f;
constexpr float kBottomBarHeight     = 40.0f;
constexpr float kGridPaddingX        = 48.0f;
constexpr float kGridPaddingTop      = 24.0f;
constexpr float kCellPadding         = 18.0f;
constexpr float kPosterW             = 220.0f;
constexpr float kPosterH             = 330.0f;
constexpr float kLabelAreaH          = 56.0f;   // Title + year below poster
constexpr float kCellW               = kPosterW;
constexpr float kCellH               = kPosterH + kLabelAreaH;
constexpr float kOutlineThickness    = 4.0f;

// Deterministic colored tint for a tmdb_id, used as a poster placeholder
// until real image loading lands in a later phase. Picks a hue around the
// theme accent by salting the id into the R/G/B channels.
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

}  // namespace

BrowseScreen::BrowseScreen(RadarrClient& radarr) : radarr_(radarr) {}

void BrowseScreen::enter() {
    want_search_screen_ = false;
    // Health-check Radarr on entry so we can surface a banner when the
    // service isn't answering. Cheap — hits /api/v3/system/status. This
    // also lets the user know why the grid is empty if services are
    // still coming up after boot.
    services_ok_ = radarr_.is_reachable();
    spdlog::info("[BrowseScreen] enter: radarr_ok={}, loaded={}",
                 services_ok_, loaded_);
    if (!loaded_) {
        load_category(category_);
        loaded_ = true;
    }
}

const char* BrowseScreen::label_for_category(Category cat) {
    switch (cat) {
        case Category::Popular:    return "Popular";
        case Category::NowPlaying: return "Now Playing";
        case Category::TopRated:   return "Top Rated";
        case Category::Discover:   return "Discover";
        case Category::Search:     return "Search";
        case Category::Library:    return "Library";
        case Category::Queue:      return "Queue";
        case Category::Settings:   return "Settings";
    }
    return "";
}

const char* BrowseScreen::query_for_category(Category cat) {
    // These queries approximate TMDB-style categories through Radarr's
    // search-based /movie/lookup endpoint, which is the only discovery
    // surface Radarr exposes. They are intentionally coarse — swap in a
    // real TMDB Discover call in a later phase if we want genuine lists.
    // Nav chips (Search, Library, Queue, Settings) do not fetch; they
    // transition out to their own Screen.
    switch (cat) {
        case Category::Popular:    return "popular";
        case Category::NowPlaying: return "2026";
        case Category::TopRated:   return "best";
        case Category::Discover:   return "discover";
        case Category::Search:
        case Category::Library:
        case Category::Queue:
        case Category::Settings:   return "";  // Nav chips transition out — no fetch.
    }
    return "";
}

void BrowseScreen::load_category(Category cat) {
    movies_.clear();
    grid_cursor_ = 0;
    scroll_row_ = 0;
    if (is_nav_chip(cat)) return;
    const char* query = query_for_category(cat);
    if (!query || !*query) return;
    loading_ = true;
    spdlog::info("[BrowseScreen] load_category: {} (query=\"{}\")",
                 label_for_category(cat), query);
    movies_ = radarr_.lookup(query);
    loading_ = false;
    spdlog::info("[BrowseScreen] load_category done: {} results",
                 movies_.size());
    if (!movies_.empty()) {
        const auto& m = movies_.front();
        spdlog::info("[BrowseScreen] first result: tmdb_id={} title='{}' poster_url='{}'",
                     m.tmdb_id, m.title,
                     m.poster_url.empty() ? "(EMPTY)" : m.poster_url.substr(0, 80));
    }
}

Screen BrowseScreen::handle_input(const std::vector<platform::InputEvent>& events) {
    for (const auto& e : events) {
        // Always: Menu returns to kiosk main menu.
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            return Screen::Exit;
        }

        // Vertical movement — ROTATE_VERTICAL (dpad up/down).
        if (e.action == platform::InputAction::ROTATE_VERTICAL) {
            int delta = e.delta;
            if (delta == 0) continue;
            if (focus_ == Focus::CategoryStrip) {
                if (delta > 0) {
                    // Drop into the grid.
                    focus_ = Focus::PosterGrid;
                    grid_cursor_ = 0;
                    scroll_row_ = 0;
                }
                // Up from the strip is a no-op.
            } else {
                // In the grid. Up from the top row jumps to the strip.
                int row = grid_cursor_ / kGridCols;
                int col = grid_cursor_ % kGridCols;
                if (delta < 0 && row == 0) {
                    focus_ = Focus::CategoryStrip;
                    category_cursor_ = static_cast<int>(category_);
                } else {
                    int new_row = row + (delta > 0 ? 1 : -1);
                    int max_row = movies_.empty()
                                  ? 0
                                  : (static_cast<int>(movies_.size()) - 1) / kGridCols;
                    new_row = std::clamp(new_row, 0, max_row);
                    int new_idx = new_row * kGridCols + col;
                    if (new_idx < static_cast<int>(movies_.size())) {
                        grid_cursor_ = new_idx;
                    } else if (!movies_.empty()) {
                        grid_cursor_ = static_cast<int>(movies_.size()) - 1;
                    }
                }
            }
            continue;
        }

        // Horizontal movement — ROTATE (dpad L/R and rotary wheel).
        if (e.action == platform::InputAction::ROTATE) {
            int delta = e.delta;
            if (delta == 0) continue;
            if (focus_ == Focus::CategoryStrip) {
                category_cursor_ = std::clamp(category_cursor_ + delta, 0,
                                              kNumCategories - 1);
            } else {
                if (movies_.empty()) continue;
                int n = static_cast<int>(movies_.size());
                grid_cursor_ = std::clamp(grid_cursor_ + delta, 0, n - 1);
            }
            continue;
        }

        // Select — A button or rotary click.
        if (e.action == platform::InputAction::SELECT && e.pressed) {
            if (focus_ == Focus::CategoryStrip) {
                Category chosen = static_cast<Category>(category_cursor_);
                // Nav chips transition to their Screen without touching
                // category_ / movies_, so returning to Browse later keeps
                // the previous content grid intact.
                switch (chosen) {
                    case Category::Search:   return Screen::Search;
                    case Category::Library:  return Screen::Library;
                    case Category::Queue:    return Screen::Queue;
                    case Category::Settings: return Screen::MovieSettings;
                    default: break;  // Content chip — fall through.
                }
                category_ = chosen;
                load_category(category_);
                focus_ = Focus::PosterGrid;
                continue;
            }
            if (!movies_.empty() &&
                grid_cursor_ >= 0 &&
                grid_cursor_ < static_cast<int>(movies_.size())) {
                selected_tmdb_id_ = movies_[grid_cursor_].tmdb_id;
                return Screen::Detail;
            }
            continue;
        }
    }

    // Keep scroll_row_ such that the grid_cursor_ is visible. Visible rows
    // is derived at render time from screen height, but clamp to a safe
    // window here so scroll stays put if render hasn't run yet.
    if (focus_ == Focus::PosterGrid && !movies_.empty()) {
        int row = grid_cursor_ / kGridCols;
        if (row < scroll_row_) scroll_row_ = row;
        // Upper bound is handled in render() where we know visible_rows.
    }

    return Screen::Browse;
}

void BrowseScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    const ::ui::Theme& th = r.mb_theme();

    // Background
    r.mb_fill_background();

    // --- Top category strip -------------------------------------------
    r.mb_fill_rect(0.0f, 0.0f, static_cast<float>(screen_w), kCategoryStripHeight,
                   th.bg, 0.75f);

    int strip_font = th.font_medium_size;
    int strip_baseline = r.mb_text_baseline(strip_font);
    float strip_text_y = (kCategoryStripHeight / 2.0f) - (strip_font / 2.0f)
                       + static_cast<float>(strip_baseline);

    // Measure all labels to lay them out evenly across the strip.
    // Layout is two groups (content + nav) with a thin vertical divider
    // between them. The divider consumes kDividerW worth of the
    // inter-chip spacing so chips stay evenly distributed.
    constexpr float kDividerW = 2.0f;
    constexpr float kDividerPadX = 12.0f;   // padding on each side of divider
    float total_label_w = 0.0f;
    std::vector<float> label_widths(kNumCategories, 0.0f);
    for (int i = 0; i < kNumCategories; ++i) {
        float w = static_cast<float>(
            r.mb_text_width(label_for_category(static_cast<Category>(i)), strip_font));
        label_widths[i] = w;
        total_label_w += w;
    }
    float divider_total = kDividerW + 2.0f * kDividerPadX;
    float spacing = std::max(18.0f,
        (static_cast<float>(screen_w) - total_label_w - 2.0f * kGridPaddingX
         - divider_total)
        / static_cast<float>(kNumCategories - 1));

    float cursor_x = kGridPaddingX;
    for (int i = 0; i < kNumCategories; ++i) {
        Category cat = static_cast<Category>(i);
        const char* label = label_for_category(cat);
        // Only content chips can be "active" (the category that owns the
        // current grid). Nav chips are never active since they just
        // transition elsewhere.
        bool is_active = (!is_nav_chip(cat) && i == static_cast<int>(category_));
        bool is_focused = (focus_ == Focus::CategoryStrip && i == category_cursor_);
        // Nav chips use the action (steel blue) color when idle to
        // visually distinguish them from dim/accent content chips.
        ::ui::Color color;
        if (is_focused) {
            color = th.accent;
        } else if (is_active) {
            color = th.accent;
        } else if (is_nav_chip(cat)) {
            color = th.action;
        } else {
            color = th.dim;
        }
        float alpha = is_focused ? 1.0f : (is_nav_chip(cat) ? 0.9f : 0.85f);
        r.mb_draw_text(label, cursor_x, strip_text_y, strip_font, color, alpha);

        if (is_focused) {
            // Focus underline below the label.
            float underline_y = strip_text_y + 6.0f;
            r.mb_fill_rect(cursor_x, underline_y, label_widths[i], 3.0f,
                           th.accent, 1.0f);
        }
        cursor_x += label_widths[i];
        // Draw the divider between the last content chip and the first
        // nav chip, consuming the inter-chip gap on either side.
        if (i == kNumContentCategories - 1) {
            cursor_x += kDividerPadX;
            float div_h = kCategoryStripHeight * 0.5f;
            float div_y = (kCategoryStripHeight - div_h) / 2.0f;
            r.mb_fill_rect(cursor_x, div_y, kDividerW, div_h, th.dim, 0.6f);
            cursor_x += kDividerW + kDividerPadX;
        } else if (i < kNumCategories - 1) {
            cursor_x += spacing;
        }
    }

    // Thin separator below the strip.
    r.mb_fill_rect(0.0f, kCategoryStripHeight - 1.0f,
                   static_cast<float>(screen_w), 1.0f, th.dim, 0.6f);

    // --- Services-offline banner -------------------------------------
    // Drawn just below the category strip, NOT blocking the grid render
    // below — the user can still navigate the (empty) grid; the banner
    // just tells them why data is absent.
    float banner_h = 0.0f;
    if (!services_ok_) {
        constexpr float kOfflineBannerH = 32.0f;
        float by = kCategoryStripHeight;
        r.mb_fill_rect(0.0f, by, static_cast<float>(screen_w),
                       kOfflineBannerH, th.highlight2, 0.85f);
        const std::string msg = "Radarr service offline — check `docker ps` on Pi";
        int sz = th.font_small_size;
        int baseline = r.mb_text_baseline(sz);
        int tw = r.mb_text_width(msg, sz);
        float tx = (static_cast<float>(screen_w) - static_cast<float>(tw)) / 2.0f;
        float ty = by + (kOfflineBannerH / 2.0f) - (sz / 2.0f)
                 + static_cast<float>(baseline);
        r.mb_draw_text(msg, tx, ty, sz, th.fg, 1.0f);
        banner_h = kOfflineBannerH;
    }

    // --- Poster grid --------------------------------------------------
    float grid_top    = kCategoryStripHeight + banner_h + kGridPaddingTop;
    float grid_bottom = static_cast<float>(screen_h) - kBottomBarHeight;
    float grid_h      = grid_bottom - grid_top;

    int visible_rows = std::max(1,
        static_cast<int>(grid_h / (kCellH + kCellPadding)));

    // Clamp scroll so the focused cell stays on screen.
    if (focus_ == Focus::PosterGrid && !movies_.empty()) {
        int focused_row = grid_cursor_ / kGridCols;
        if (focused_row < scroll_row_) scroll_row_ = focused_row;
        if (focused_row >= scroll_row_ + visible_rows) {
            scroll_row_ = focused_row - visible_rows + 1;
        }
    }

    int total_rows = movies_.empty() ? 0
                   : (static_cast<int>(movies_.size()) - 1) / kGridCols + 1;
    int end_row = std::min(total_rows, scroll_row_ + visible_rows);

    // Empty-state message. Nav chips never become the active category_
    // (they just transition to another Screen), so this only fires for
    // content categories that returned zero results from Radarr.
    // If the category fetch hasn't completed yet, show "Loading..." —
    // lookup is synchronous so this is only visible for a fraction of a
    // second locally, but on slow networks (or while Radarr is warming
    // up after boot) the feedback matters.
    if (movies_.empty()) {
        std::string msg;
        int msg_size = th.font_large_size;
        ::ui::Color msg_color = th.dim;
        if (loading_) {
            msg = "Loading...";
        } else {
            msg = "Nothing here yet. Configure indexers via SSH tunnel: "
                  "ssh -L 9696:localhost:9696 magic@magicpi.local";
        }
        int msg_w = r.mb_text_width(msg, msg_size);
        float msg_x = (static_cast<float>(screen_w) - static_cast<float>(msg_w)) / 2.0f;
        float msg_y = grid_top + grid_h / 2.0f
                    + static_cast<float>(r.mb_text_baseline(msg_size));
        r.mb_draw_text(msg, msg_x, msg_y, msg_size, msg_color, 0.9f);
    }

    // Compute column spacing so the grid spans the interior evenly.
    float grid_interior_w = static_cast<float>(screen_w) - 2.0f * kGridPaddingX;
    float col_gap = (grid_interior_w - kGridCols * kCellW)
                    / std::max(1.0f, static_cast<float>(kGridCols - 1));
    if (col_gap < kCellPadding) col_gap = kCellPadding;

    for (int row = scroll_row_; row < end_row; ++row) {
        for (int col = 0; col < kGridCols; ++col) {
            int idx = row * kGridCols + col;
            if (idx >= static_cast<int>(movies_.size())) break;
            const auto& m = movies_[idx];

            float cell_x = kGridPaddingX + col * (kCellW + col_gap);
            float cell_y = grid_top + (row - scroll_row_) * (kCellH + kCellPadding);

            // Poster: real artwork if the async cache has it yet,
            // else the deterministic tint as a placeholder. Cache
            // auto-enqueues the fetch on first call.
            ::ui::Color tint = poster_tint_for_tmdb(m.tmdb_id);
            r.mb_draw_poster_or_tint(m.poster_url,
                                     cell_x, cell_y, kPosterW, kPosterH,
                                     tint, 1.0f);

            // Subtle inset to give it depth
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

            // Title (truncated to cell width)
            int title_size = th.font_medium_size;
            int title_baseline = r.mb_text_baseline(title_size);
            std::string title = truncate_to_width(r, m.title.empty() ? "Untitled" : m.title,
                                                  title_size, kCellW);
            float title_y = cell_y + kPosterH + 8.0f + static_cast<float>(title_baseline);
            r.mb_draw_text(title, cell_x, title_y, title_size, th.fg,
                           focused ? 1.0f : 0.9f);

            // Year
            if (m.year > 0) {
                std::string year = std::to_string(m.year);
                int year_size = th.font_small_size;
                int year_baseline = r.mb_text_baseline(year_size);
                float year_y = title_y + static_cast<float>(year_size) * 0.9f
                             + static_cast<float>(year_baseline) * 0.2f;
                r.mb_draw_text(year, cell_x, year_y, year_size, th.dim, 0.9f);
            }
        }
    }

    // --- Bottom hint bar ---------------------------------------------
    float bar_y = static_cast<float>(screen_h) - kBottomBarHeight;
    r.mb_fill_rect(0.0f, bar_y, static_cast<float>(screen_w), kBottomBarHeight,
                   th.bg, 0.75f);
    r.mb_fill_rect(0.0f, bar_y, static_cast<float>(screen_w), 1.0f,
                   th.dim, 0.6f);

    const std::string hint =
        "Rotate to navigate   RCLICK/A: Select   BTN4/B: Exit";
    int hint_size = th.font_small_size;
    int hint_baseline = r.mb_text_baseline(hint_size);
    int hint_w = r.mb_text_width(hint, hint_size);
    float hint_x = (static_cast<float>(screen_w) - static_cast<float>(hint_w)) / 2.0f;
    float hint_y = bar_y + (kBottomBarHeight / 2.0f) - (hint_size / 2.0f)
                 + static_cast<float>(hint_baseline);
    r.mb_draw_text(hint, hint_x, hint_y, hint_size, th.fg, 0.85f);
}

}  // namespace media_browser::ui
