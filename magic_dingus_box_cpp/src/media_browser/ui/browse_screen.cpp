#include "media_browser/ui/browse_screen.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <ctime>
#include <string>

#include <spdlog/spdlog.h>

#include "media_browser/radarr/radarr_client.h"
#include "media_browser/tmdb_client.h"
#include "ui/renderer.h"
#include "ui/theme.h"
#include "ui/toast.h"

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

// Filter panel dimensions (Phase B).
constexpr float kFilterPanelH        = 56.0f;
constexpr float kFilterControlW      = 260.0f;
constexpr float kFilterControlGap    = 18.0f;

// Hard-coded sort-by options. Keep small — TMDB supports more sorts but
// these three cover >99% of actual user intent for a movie browser and
// keep the UI legible.
struct SortOption {
    const char* label;   // Human-readable.
    const char* value;   // TMDB sort_by value.
};
constexpr std::array<SortOption, 3> kSortOptions = {{
    {"Popularity",   "popularity.desc"},
    {"Rating",       "vote_average.desc"},
    {"Release date", "primary_release_date.desc"},
}};

// Deterministic colored tint for a tmdb_id, used as a poster placeholder
// until the artwork cache has fetched the real image.
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

// Current calendar year — used as the upper bound of the Year cycle.
int current_year_now() {
    std::time_t now = std::time(nullptr);
    std::tm* lt = std::localtime(&now);
    if (!lt) return 2026;
    return 1900 + lt->tm_year;
}

}  // namespace

BrowseScreen::BrowseScreen(RadarrClient& radarr, TmdbClient& tmdb)
    : radarr_(radarr), tmdb_(tmdb) {}

void BrowseScreen::enter() {
    want_search_screen_ = false;
    // Health-check Radarr on entry so we can surface a banner when the
    // service isn't answering. Cheap — hits /api/v3/system/status.
    // We keep this even though Discovery is now TMDB-powered: Radarr is
    // still required for add-to-library / queue actions downstream.
    services_ok_ = radarr_.is_reachable();
    spdlog::info("[BrowseScreen] enter: radarr_ok={}, loaded={}",
                 services_ok_, loaded_);
    if (!loaded_) {
        load_category(category_);
        loaded_ = true;
    }

    // Cache library + quality profiles once so BTN2 quick-add doesn't
    // refetch on every press. Refreshed after a successful add.
    if (services_ok_ && !library_cached_) {
        auto lib = radarr_.get_library();
        library_tmdb_ids_.clear();
        for (const auto& m : lib) {
            if (m.tmdb_id > 0) library_tmdb_ids_.insert(m.tmdb_id);
        }
        quality_profiles_ = radarr_.get_quality_profiles();
        library_cached_ = true;
    }
}

void BrowseScreen::quick_add_focused() {
    // Only meaningful when a poster is focused.
    if (focus_ != Focus::PosterGrid) return;
    if (movies_.empty()) return;
    if (grid_cursor_ < 0 ||
        grid_cursor_ >= static_cast<int>(movies_.size())) return;
    const auto& hit = movies_[grid_cursor_];
    if (hit.tmdb_id <= 0) return;

    // Already in library? Short-circuit with a toast.
    if (library_tmdb_ids_.count(hit.tmdb_id) > 0) {
        ::ui::Toast::show("Already in library");
        return;
    }

    // Pick quality profile — prefer "HD-1080p", fall back to first.
    int qp = 0;
    for (const auto& p : quality_profiles_) {
        if (p.name == "HD-1080p") { qp = p.id; break; }
    }
    if (qp == 0 && !quality_profiles_.empty()) qp = quality_profiles_.front().id;
    if (qp == 0) {
        ::ui::Toast::show("No quality profile — check Radarr");
        return;
    }

    bool ok = radarr_.add_movie(hit.tmdb_id, qp, /*monitor=*/true);
    if (!ok) {
        ::ui::Toast::show("Add failed — see Radarr logs");
        return;
    }
    library_tmdb_ids_.insert(hit.tmdb_id);
    std::string msg = "Added: ";
    msg += (hit.title.empty() ? "movie" : hit.title);
    ::ui::Toast::show(msg);
}

const char* BrowseScreen::label_for_category(Category cat) {
    switch (cat) {
        case Category::Popular:    return "Popular";
        case Category::NowPlaying: return "Now Playing";
        case Category::TopRated:   return "Top Rated";
        case Category::Upcoming:   return "Upcoming";
        case Category::Filter:     return "Filter";
        case Category::Search:     return "Search";
        case Category::Library:    return "Library";
        case Category::Queue:      return "Queue";
        case Category::Settings:   return "Settings";
    }
    return "";
}

void BrowseScreen::ensure_genres_loaded() {
    if (genres_loaded_) return;
    genres_ = tmdb_.get_genres();
    genres_loaded_ = true;
    spdlog::info("[BrowseScreen] genres loaded: {} entries", genres_.size());
}

void BrowseScreen::load_category(Category cat) {
    movies_.clear();
    grid_cursor_ = 0;
    scroll_row_ = 0;
    if (is_nav_chip(cat)) return;
    loading_ = true;
    spdlog::info("[BrowseScreen] load_category: {}", label_for_category(cat));
    switch (cat) {
        case Category::Popular:    movies_ = tmdb_.get_popular();     break;
        case Category::NowPlaying: movies_ = tmdb_.get_now_playing(); break;
        case Category::TopRated:   movies_ = tmdb_.get_top_rated();   break;
        case Category::Upcoming:   movies_ = tmdb_.get_upcoming();    break;
        case Category::Filter:
            ensure_genres_loaded();
            movies_ = tmdb_.discover(current_filter_);
            break;
        default: break;
    }
    loading_ = false;
    spdlog::info("[BrowseScreen] load_category done: {} results",
                 movies_.size());
    if (!movies_.empty()) {
        const auto& m = movies_.front();
        spdlog::info("[BrowseScreen] first result: tmdb_id={} title='{}' poster_url='{}'",
                     m.tmdb_id, m.title,
                     m.poster_path.empty() ? "(EMPTY)" : m.poster_path.substr(0, 80));
    }
}

void BrowseScreen::reload_filter_results() {
    current_filter_.sort_by = kSortOptions[current_sort_index_].value;
    loading_ = true;
    movies_ = tmdb_.discover(current_filter_);
    loading_ = false;
    grid_cursor_ = 0;
    scroll_row_ = 0;
    spdlog::info(
        "[BrowseScreen] discover: genre_id={} year={} sort_by={} -> {} results",
        current_filter_.genre_id.value_or(-1),
        current_filter_.year.value_or(-1),
        current_filter_.sort_by,
        movies_.size());
}

void BrowseScreen::cycle_filter_value(int delta) {
    if (delta == 0) return;
    switch (filter_row_) {
        case FilterRow::Genre: {
            // Ordering: "Any" (id=-1) then genres_ in order.
            // Build a flat cycle list of size 1 + genres_.size().
            int n = 1 + static_cast<int>(genres_.size());
            if (n <= 0) return;
            // Find current index.
            int idx = 0;
            if (current_filter_.genre_id.has_value()) {
                int gid = *current_filter_.genre_id;
                for (size_t i = 0; i < genres_.size(); ++i) {
                    if (genres_[i].id == gid) { idx = 1 + static_cast<int>(i); break; }
                }
            }
            idx = ((idx + delta) % n + n) % n;
            if (idx == 0) current_filter_.genre_id.reset();
            else current_filter_.genre_id = genres_[idx - 1].id;
            break;
        }
        case FilterRow::Year: {
            // Cycle: Any -> 1970..current_year -> Any.
            const int lo = 1970;
            const int hi = current_year_now();
            int n = 1 + (hi - lo + 1);
            if (n <= 0) return;
            int idx = 0;
            if (current_filter_.year.has_value()) {
                int y = *current_filter_.year;
                if (y >= lo && y <= hi) idx = 1 + (y - lo);
            }
            idx = ((idx + delta) % n + n) % n;
            if (idx == 0) current_filter_.year.reset();
            else current_filter_.year = lo + (idx - 1);
            break;
        }
        case FilterRow::SortBy: {
            int n = static_cast<int>(kSortOptions.size());
            if (n <= 0) return;
            current_sort_index_ =
                ((current_sort_index_ + delta) % n + n) % n;
            break;
        }
        default: return;
    }
    reload_filter_results();
}

Screen BrowseScreen::handle_input(const std::vector<platform::InputEvent>& events) {
    for (const auto& e : events) {
        // Always: Menu returns to kiosk main menu.
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            return Screen::Exit;
        }

        // BTN2 (PLAY_PAUSE, red): quick-add focused poster to library.
        if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed) {
            quick_add_focused();
            continue;
        }

        // Vertical movement — ROTATE_VERTICAL (dpad up/down, BTN1/BTN3).
        if (e.action == platform::InputAction::ROTATE_VERTICAL) {
            int delta = e.delta;
            if (delta == 0) continue;
            if (focus_ == Focus::CategoryStrip) {
                if (delta > 0) {
                    // Drop down into the panel (if Filter active) or grid.
                    if (category_ == Category::Filter) {
                        focus_ = Focus::FilterPanel;
                        filter_row_ = FilterRow::Genre;
                    } else {
                        focus_ = Focus::PosterGrid;
                        grid_cursor_ = 0;
                        scroll_row_ = 0;
                    }
                }
                // Up from the strip is a no-op.
            } else if (focus_ == Focus::FilterPanel) {
                // FilterPanel only has ONE row of 3 chips (rendered side-by-
                // side) so up/down jumps between the strip and the grid,
                // matching the earlier UX model.
                if (delta < 0) {
                    focus_ = Focus::CategoryStrip;
                    category_cursor_ = static_cast<int>(category_);
                } else {
                    focus_ = Focus::PosterGrid;
                    grid_cursor_ = 0;
                    scroll_row_ = 0;
                }
            } else {
                // In the grid. Up from the top row jumps to the panel (if
                // Filter is active) or the strip.
                int row = grid_cursor_ / kGridCols;
                int col = grid_cursor_ % kGridCols;
                if (delta < 0 && row == 0) {
                    if (category_ == Category::Filter) {
                        focus_ = Focus::FilterPanel;
                    } else {
                        focus_ = Focus::CategoryStrip;
                        category_cursor_ = static_cast<int>(category_);
                    }
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
            } else if (focus_ == Focus::FilterPanel) {
                // Horizontal in the panel cycles the focused row's value
                // (which is the natural UX — rotary left/right changes the
                // genre / year / sort). UP/DOWN between rows would need
                // more rows; since we only have 3 controls laid out
                // horizontally, we walk between them on horizontal too.
                // Compromise: LEFT/RIGHT cycles VALUE for the focused row.
                // Rows are swapped via the dedicated button SELECT — see
                // below. Keeps controls discoverable without 3-layer nav.
                cycle_filter_value(delta);
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
                if (category_ == Category::Filter) {
                    focus_ = Focus::FilterPanel;
                    filter_row_ = FilterRow::Genre;
                } else {
                    focus_ = Focus::PosterGrid;
                }
                continue;
            }
            if (focus_ == Focus::FilterPanel) {
                // SELECT on the filter panel steps the focused row to
                // the next control (wraps). Left/Right adjusts VALUE;
                // SELECT moves BETWEEN controls. Simple + discoverable.
                int n = static_cast<int>(FilterRow::Count);
                int idx = (static_cast<int>(filter_row_) + 1) % n;
                filter_row_ = static_cast<FilterRow>(idx);
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
    float spacing = std::max(14.0f,
        (static_cast<float>(screen_w) - total_label_w - 2.0f * kGridPaddingX
         - divider_total)
        / static_cast<float>(kNumCategories - 1));

    float cursor_x = kGridPaddingX;
    for (int i = 0; i < kNumCategories; ++i) {
        Category cat = static_cast<Category>(i);
        const char* label = label_for_category(cat);
        bool is_active = (!is_nav_chip(cat) && i == static_cast<int>(category_));
        bool is_focused = (focus_ == Focus::CategoryStrip && i == category_cursor_);
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

    // --- Filter panel (Phase B) --------------------------------------
    float panel_h = 0.0f;
    if (category_ == Category::Filter) {
        float py = kCategoryStripHeight + banner_h;
        r.mb_fill_rect(0.0f, py, static_cast<float>(screen_w), kFilterPanelH,
                       th.bg, 0.9f);
        r.mb_fill_rect(0.0f, py + kFilterPanelH - 1.0f,
                       static_cast<float>(screen_w), 1.0f, th.dim, 0.6f);

        // Compose each control's text and render it as a chip.
        auto genre_label = [&]() -> std::string {
            if (!current_filter_.genre_id.has_value()) return "Genre: Any";
            int gid = *current_filter_.genre_id;
            for (const auto& g : genres_) {
                if (g.id == gid) return std::string("Genre: ") + g.name;
            }
            return "Genre: ?";
        };
        auto year_label = [&]() -> std::string {
            if (!current_filter_.year.has_value()) return "Year: Any";
            return std::string("Year: ") + std::to_string(*current_filter_.year);
        };
        auto sort_label = [&]() -> std::string {
            int idx = std::clamp(current_sort_index_, 0,
                                 static_cast<int>(kSortOptions.size()) - 1);
            return std::string("Sort: ") + kSortOptions[idx].label;
        };

        std::array<std::string, 3> labels = {genre_label(), year_label(), sort_label()};

        int pf_size = th.font_medium_size;
        int pf_baseline = r.mb_text_baseline(pf_size);
        float text_y = py + (kFilterPanelH / 2.0f) - (pf_size / 2.0f)
                     + static_cast<float>(pf_baseline);

        float cx = kGridPaddingX;
        for (int i = 0; i < 3; ++i) {
            bool is_row_focused =
                (focus_ == Focus::FilterPanel && i == static_cast<int>(filter_row_));
            ::ui::Color col = is_row_focused ? th.accent : th.fg;
            float chip_w = kFilterControlW;
            // Chip background so the focus ring reads clearly.
            if (is_row_focused) {
                r.mb_fill_rect(cx - 8.0f, py + 8.0f,
                               chip_w + 16.0f, kFilterPanelH - 16.0f,
                               th.accent, 0.15f);
            }
            std::string txt = truncate_to_width(r, labels[i], pf_size, chip_w);
            r.mb_draw_text(txt, cx, text_y, pf_size, col,
                           is_row_focused ? 1.0f : 0.9f);
            cx += chip_w + kFilterControlGap;
        }

        panel_h = kFilterPanelH;
    }

    // --- Poster grid --------------------------------------------------
    float grid_top    = kCategoryStripHeight + banner_h + panel_h + kGridPaddingTop;
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

    if (movies_.empty()) {
        std::string msg;
        int msg_size = th.font_large_size;
        ::ui::Color msg_color = th.dim;
        if (loading_) {
            msg = "Loading...";
        } else if (category_ == Category::Filter) {
            msg = "No matches. Adjust the filter above.";
        } else {
            msg = "Nothing here yet. TMDB may be unreachable — "
                  "check network + TMDB API key.";
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

            ::ui::Color tint = poster_tint_for_tmdb(m.tmdb_id);
            // TmdbSearchHit::poster_path is pre-resolved to a full URL
            // by parse_list_response — pass directly to the cache.
            r.mb_draw_poster_or_tint(m.poster_path,
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

    std::string hint;
    if (focus_ == Focus::FilterPanel) {
        hint = "Rotate: change value   RCLICK: next control   BTN1/BTN3: up/down   BTN4: back (hold: exit)";
    } else {
        hint = "Rotate: nav   RCLICK: open   BTN2: quick-add   BTN4: back (hold: exit)";
    }
    int hint_size = th.font_small_size;
    int hint_baseline = r.mb_text_baseline(hint_size);
    int hint_w = r.mb_text_width(hint, hint_size);
    float hint_x = (static_cast<float>(screen_w) - static_cast<float>(hint_w)) / 2.0f;
    float hint_y = bar_y + (kBottomBarHeight / 2.0f) - (hint_size / 2.0f)
                 + static_cast<float>(hint_baseline);
    r.mb_draw_text(hint, hint_x, hint_y, hint_size, th.fg, 0.85f);
}

}  // namespace media_browser::ui
