#include "media_browser/ui/browse_screen.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>

#include <spdlog/spdlog.h>

#include "media_browser/radarr/radarr_client.h"
#include "media_browser/tmdb_client.h"
#include "ui/renderer.h"
#include "ui/theme.h"
#include "ui/toast.h"

namespace media_browser::ui {

namespace {

// Retro home-menu-inspired layout, deliberately matching DetailScreen's
// chrome so the two screens read as one consistent kiosk surface. The
// shared idioms are:
//   - "BROWSE" header (Zen Dots, steel-blue) underlined with a 2px rule
//     spanning the full width — same pattern DetailScreen uses for
//     "FEATURE PRESENTATION".
//   - Steel-blue (accent2) section dividers between major regions.
//   - Gold-outline-only chips and focus rings (no fill blocks). The
//     previous Browse screen used filled chip backgrounds; we drop them
//     to match the home menu's border-and-text aesthetic.
//   - Blinking left-pointing ◂ triangle inside the focused chip,
//     identical 500ms cycle and steel-blue color as the home-menu
//     playlist cursor and DetailScreen's action-button cursor.
//   - Centered dim footer hint.

// Padding shared with DetailScreen so the header rule lines up exactly.
constexpr float kPaddingX        = 32.0f;
constexpr float kHeaderBaselineY = 38.0f;     // baseline of header text
constexpr float kHeaderRuleY     = 58.0f;     // Y of the 2px steel-blue rule
constexpr float kBottomBarHeight = 40.0f;

// Category chip strip lives directly below the header rule. Chips are
// outline-only with a gold border (focused = thicker + blinking ◂);
// nav chips use the steel-blue (accent2) color when not focused so they
// read as a different "type" from the content chips, but the geometry
// is identical.
constexpr float kChipStripTop    = kHeaderRuleY + 14.0f;
constexpr float kChipH           = 32.0f;
// Bumped from 14 -> 18 so chip text has more breathing room from the
// border on both sides, especially the long ones ("Now Playing"). The
// chip strip no longer feels cramped; combined with kMarkerZoneW it
// gives every chip a consistent shape regardless of label length.
constexpr float kChipPadX        = 18.0f;
// Minimum gap between chips. The actual gap is computed dynamically per-
// frame from the slack between natural width and available width, so the
// chip strip fills the full header bar instead of clustering on the left.
// 14 is the floor — anything tighter and chips visually merge.
constexpr float kChipMinGap      = 14.0f;
constexpr float kChipBorderW     = 2.0f;
constexpr float kChipFocusBorderW = 3.0f;
// Reserved zone on the RIGHT side of every chip for the blinking ◂ focus
// marker. The label is centered inside the LEFT portion of the chip
// (chip_w minus this zone) so the marker never overlaps the last letter
// of long labels like "Now Playing". The zone exists on every chip
// regardless of focus, so chip widths are stable across focus changes
// (no reflow when arrowing left/right). 22px is wide enough to hold the
// 14.5px-wide triangle plus a small inset on either side.
constexpr float kMarkerZoneW     = 22.0f;
// Steel-blue divider drawn between the last content chip and the first
// nav chip. Half-height bar, full alpha — separator without weight.
constexpr float kChipDividerW    = 2.0f;
constexpr float kChipDividerPadX = 10.0f;

// Section rule below the chip strip (and below the optional filter
// panel) — same steel-blue 2px rule that appears under the chip strip
// and above the action row in DetailScreen.
constexpr float kSectionRuleAlpha = 0.85f;

// Filter panel — same outline-chip language as the category strip,
// just with three wider chips that show the current Genre / Year / Sort
// selection and blink the ◂ marker on the focused row.
constexpr float kFilterPanelGap  = 12.0f;     // gap between strip rule and panel
constexpr float kFilterChipH     = 36.0f;
constexpr float kFilterChipGap   = 14.0f;
constexpr float kFilterChipMinW  = 220.0f;

// Poster grid. With kGridCols=5, cell width is now computed dynamically
// per-frame from the available interior width and column gap (see the
// render() block tagged "Compute column spacing"). Poster height is
// derived from a 2:3 movie-poster aspect ratio so cells stay correctly
// proportioned no matter what cell_w we land on.
constexpr float kGridPaddingX     = 32.0f;    // matches kPaddingX so columns align with the rule
constexpr float kGridPaddingTop   = 18.0f;
constexpr float kCellPaddingY     = 18.0f;
constexpr float kCellGap          = 18.0f;    // horizontal gap between cells
constexpr float kPosterAspect     = 1.5f;     // 2:3 portrait — TMDB poster aspect
// 42 px label area: at 9-col density the prior 56 px chunk consumed 31% of
// poster height and felt visually heavy. Trimming to 42 (≈24% of the new
// 178 px poster height) keeps title + year readable without the label
// dominating each cell.
constexpr float kLabelAreaH       = 42.0f;    // title + year area below poster
constexpr float kPosterBorderW    = 2.0f;

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

    // Always re-fetch the library on (re-)entry so the "in library"
    // cache reflects any adds/removes that happened on Detail since
    // we were last visible. Without this, removing a movie via Detail
    // → Confirm Remove leaves a stale entry in library_tmdb_ids_, and
    // the user gets "Already in library" toasts when re-adding the
    // same movie. Same cost (one Radarr GET, ~200ms) and same
    // pattern LibraryScreen::enter() already uses.
    //
    // Quality profiles are cached separately because they don't change
    // when adds/removes happen — only when the user reconfigures
    // quality profiles in Radarr's settings UI, which is rare enough
    // that we accept staleness. Refresh once on first entry.
    if (services_ok_) {
        auto lib = radarr_.get_library();
        library_tmdb_ids_.clear();
        for (const auto& m : lib) {
            if (m.tmdb_id > 0) library_tmdb_ids_.insert(m.tmdb_id);
        }
        if (!library_cached_) {
            quality_profiles_ = radarr_.get_quality_profiles();
            library_cached_ = true;
        }
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

    // Disk-space pre-flight (matches DetailScreen's check on the slower
    // add path — kept inline rather than extracted because each screen
    // is the only caller for its own add flow). Threshold is 15 GB
    // free; below that, surface a toast as a heads-up. We don't block
    // the add — small WEB-DL releases fit in <2 GB.
    {
        constexpr int64_t kWarnFreeBytes = 15LL * 1024 * 1024 * 1024;
        std::error_code ec;
        auto info = std::filesystem::space("/mnt/ssd/library", ec);
        if (!ec && info.available > 0
            && static_cast<int64_t>(info.available) < kWarnFreeBytes) {
            int gb_free = static_cast<int>(info.available
                                           / (1024 * 1024 * 1024));
            ::ui::Toast::show(
                "Warning: only " + std::to_string(gb_free)
                + " GB free — large releases may fail to import");
            // Fall through — let the user proceed; warning is informational.
        }
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
    // Async dispatch: clear the visible grid immediately (so the user
    // sees the right empty state for the new category, not stale
    // posters from the previous one), set loading_ so render shows
    // the Loading mode, then spawn a worker. The worker calls the
    // appropriate TMDB endpoint off the render thread — that call
    // takes 6+ seconds over the VPN tunnel, which is what was
    // freezing the screen.
    movies_.clear();
    grid_cursor_ = 0;
    scroll_row_ = 0;
    if (is_nav_chip(cat)) {
        loading_ = false;
        return;
    }
    if (cat == Category::Filter) {
        // Filter category needs genres loaded for the picker. Do this
        // sync because it's only ~200ms and only fires the first
        // time the user enters the Filter category.
        ensure_genres_loaded();
    }
    loading_ = true;
    const uint64_t my_gen = tmdb_current_gen_.fetch_add(1) + 1;
    spdlog::info("[BrowseScreen] load_category: {} (gen={})",
                 label_for_category(cat), my_gen);
    tmdb_workers_.emplace_back(&BrowseScreen::run_load_category, this,
                               my_gen, cat);
}

void BrowseScreen::run_load_category(uint64_t gen, Category cat) {
    // Captured-by-value: do the slow TMDB call here. If the user
    // switches categories before this returns, current_gen_ will
    // bump and our publish will be silently discarded.
    std::vector<TmdbSearchHit> result;
    switch (cat) {
        case Category::Popular:    result = tmdb_.get_popular();     break;
        case Category::NowPlaying: result = tmdb_.get_now_playing(); break;
        case Category::TopRated:   result = tmdb_.get_top_rated();   break;
        case Category::Upcoming:   result = tmdb_.get_upcoming();    break;
        case Category::Filter:     result = tmdb_.discover(current_filter_); break;
        default: break;
    }

    // Stale-check: bail without publishing if a newer load_category
    // has been requested. The new request's worker will write the
    // current result; we'd just clobber its in-flight state.
    if (gen != tmdb_current_gen_.load()) {
        spdlog::info("[BrowseScreen] gen={} stale at publish (current={}); discarding",
                     gen, tmdb_current_gen_.load());
        return;
    }
    {
        std::lock_guard<std::mutex> lk(tmdb_result_mtx_);
        if (gen != tmdb_current_gen_.load()) return;  // re-check under lock
        tmdb_pending_movies_ = std::move(result);
    }
    tmdb_result_ready_.store(true);
}

void BrowseScreen::reload_filter_results() {
    current_filter_.sort_by = kSortOptions[current_sort_index_].value;
    movies_.clear();
    grid_cursor_ = 0;
    scroll_row_ = 0;
    loading_ = true;
    const uint64_t my_gen = tmdb_current_gen_.fetch_add(1) + 1;
    spdlog::info(
        "[BrowseScreen] discover (async, gen={}): genre_id={} year={} sort_by={}",
        my_gen, current_filter_.genre_id.value_or(-1),
        current_filter_.year.value_or(-1),
        current_filter_.sort_by);
    // Pass filter by value — current_filter_ might mutate while the
    // worker is in flight if the user keeps cycling values.
    tmdb_workers_.emplace_back(&BrowseScreen::run_reload_filter, this,
                               my_gen, current_filter_);
}

void BrowseScreen::run_reload_filter(uint64_t gen, DiscoverFilter filter) {
    auto result = tmdb_.discover(filter);
    if (gen != tmdb_current_gen_.load()) {
        spdlog::info("[BrowseScreen] discover gen={} stale; discarding", gen);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(tmdb_result_mtx_);
        if (gen != tmdb_current_gen_.load()) return;
        tmdb_pending_movies_ = std::move(result);
    }
    tmdb_result_ready_.store(true);
}

void BrowseScreen::apply_pending() {
    if (!tmdb_result_ready_.load()) return;
    std::vector<TmdbSearchHit> incoming;
    {
        std::lock_guard<std::mutex> lk(tmdb_result_mtx_);
        incoming = std::move(tmdb_pending_movies_);
    }
    tmdb_result_ready_.store(false);
    movies_ = std::move(incoming);
    loading_ = false;
    grid_cursor_ = 0;
    scroll_row_ = 0;
    spdlog::info("[BrowseScreen] applied pending result: {} movies",
                 movies_.size());
    if (!movies_.empty()) {
        const auto& m = movies_.front();
        spdlog::info("[BrowseScreen] first result: tmdb_id={} title='{}' poster_url='{}'",
                     m.tmdb_id, m.title,
                     m.poster_path.empty() ? "(EMPTY)" : m.poster_path.substr(0, 80));
    }
}

void BrowseScreen::update() {
    apply_pending();
}

BrowseScreen::~BrowseScreen() {
    // Bump gen so any in-flight worker sees its result is stale and
    // bails before publishing. Then join all tracked workers so we
    // don't have a thread holding references to *this after the
    // screen is destroyed. Each worker is bounded by TmdbClient's
    // 10s curl timeout, so worst-case shutdown wait is ~10s; in
    // practice workers complete in 1-7s.
    tmdb_current_gen_.fetch_add(1);
    for (auto& t : tmdb_workers_) {
        if (t.joinable()) t.join();
    }
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

// ----------------------------------------------------------------------------
// Rendering — retro home-menu aesthetic, matched to DetailScreen.
// ----------------------------------------------------------------------------

namespace {

// Draw a blinking left-pointing ◂ marker centered at (cx, cy). The
// geometry and color exactly match DetailScreen's action-button cursor
// (and the home menu's playlist-list cursor) so the visual cue means
// the same thing across all screens. Callers are responsible for
// reserving a "marker zone" inside the chip and passing the zone's
// center X here, so the marker never overlaps chip text.
void draw_focus_cursor_at(::ui::Renderer& r, float cx, float cy,
                          float font_size, const ::ui::Color& color) {
    float marker_size = font_size * 0.45f;
    r.mb_fill_triangle(
        cx,                       cy - marker_size,
        cx,                       cy + marker_size,
        cx - marker_size * 1.2f,  cy,
        color, 1.0f);
}

}  // namespace

void BrowseScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    const ::ui::Theme& th = r.mb_theme();
    r.mb_fill_background();

    const float w = static_cast<float>(screen_w);
    const float h = static_cast<float>(screen_h);

    // 500ms blink cycle, sourced from epoch time so this screen, the
    // DetailScreen, and the home menu all breathe in lockstep — when the
    // user transitions between screens, the focus cursor doesn't jump
    // out of phase. Same formula as detail_screen.cpp.
    auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    const bool blink_on = (epoch_ms / 500) % 2 == 0;

    // --- Top header bar: "BROWSE" + right-aligned home hint ----------
    // Mirrors DetailScreen's header exactly: Zen Dots heading in
    // steel-blue (accent2), small dim hint pinned to the right, full-
    // width 2px steel-blue rule beneath. Same baseline (38) and rule Y
    // (58) so the chrome aligns pixel-for-pixel when toggling between
    // Browse and Detail.
    {
        const std::string heading = "BROWSE";
        int hd_size = th.font_heading_size;
        r.mb_draw_title_text(heading, kPaddingX, kHeaderBaselineY,
                             hd_size, th.accent2, 1.0f);

        // Right-aligned dim hint. BTN4 returns to the kiosk main menu —
        // identical convention to DetailScreen's "back" hint, except
        // here BTN4 == "home" because Browse IS the entry screen for the
        // media browser.
        const std::string back_hint = "BTN4: home";
        int hint_size = th.font_small_size;
        int hw = r.mb_text_width(back_hint, hint_size);
        float hx = w - kPaddingX - static_cast<float>(hw);
        float hy = kHeaderBaselineY + 2.0f;
        r.mb_draw_text(back_hint, hx, hy, hint_size, th.dim, 0.9f);

        // 2px steel-blue rule — the screen's primary horizontal frame.
        r.mb_draw_line(kPaddingX, kHeaderRuleY,
                       w - kPaddingX, kHeaderRuleY,
                       2.0f, th.accent2, 0.95f);
    }

    // --- Category chip strip -----------------------------------------
    // Outline-only chips, gold border for content chips, steel-blue
    // border for nav chips (so the user can tell at a glance which side
    // of the divider they're on). Active content category gets brighter
    // gold; inactive content chips fade to ~75% alpha. Focused chip
    // (regardless of active state) gets a thicker border, brighter color,
    // and the blinking ◂ marker — same idiom as DetailScreen buttons.
    //
    // Chip-width formula (this is the important bit):
    //
    //     chip_w = text_w + 2 * kChipPadX + kMarkerZoneW
    //
    // Every chip reserves kMarkerZoneW pixels on the right for the
    // blinking ◂ focus marker. The label is centered horizontally within
    // [chip_x, chip_x + chip_w - kMarkerZoneW] — i.e., the LEFT portion
    // only. This keeps the marker from ever overlapping the last letter
    // of long labels like "Now Playing", and keeps chip widths stable
    // across focus changes (the marker zone is always reserved, focused
    // or not, so chips don't shift around when the user arrows L/R).
    //
    // Strip-layout formula:
    //
    //   1. First pass: measure each chip's natural width with the formula
    //      above.
    //   2. Compute slack = available_w - sum(chip_w) - divider_w
    //                    - (chip_count - 1) * kChipMinGap.
    //   3. If slack > 0, distribute it as EXTRA gap evenly across the
    //      (chip_count - 1) inter-chip gaps. The divider also gets a
    //      share of the slack on each side via kChipDividerPadX.
    //   4. If slack <= 0, fall back to the minimum gap and let the strip
    //      slightly overflow (rare on a 1280px target).
    //
    // Net effect: the 9 chips spread across the full header bar instead
    // of clustering on the left, with the gap calculated dynamically so
    // the strip always fills the available width.
    float chip_y = kChipStripTop;
    int chip_font = th.font_small_size;
    int chip_baseline = r.mb_text_baseline(chip_font);

    // Pre-measure all chips. text_w is stored alongside chip_w so we can
    // center the label inside the non-marker portion below.
    std::array<float, kNumCategories> chip_widths{};
    std::array<float, kNumCategories> chip_text_widths{};
    float chips_total_w = 0.0f;
    for (int i = 0; i < kNumCategories; ++i) {
        float tw = static_cast<float>(
            r.mb_text_width(label_for_category(static_cast<Category>(i)),
                            chip_font));
        chip_text_widths[i] = tw;
        chip_widths[i] = tw + 2.0f * kChipPadX + kMarkerZoneW;
        chips_total_w += chip_widths[i];
    }
    // Divider occupies its own horizontal slot. The two kChipDividerPadX
    // values around it act as gaps in their own right, so when computing
    // distributable slack the divider effectively "consumes" two gap
    // slots — one on each side. The remaining inter-chip gaps number
    // (kNumCategories - 2): nine chips have 8 gaps between them, minus
    // the one gap that the divider replaces.
    float divider_total = kChipDividerW + 2.0f * kChipDividerPadX;
    constexpr int kInterChipGapCount = kNumCategories - 2;  // 7 gaps
    float available_w = w - 2.0f * kPaddingX;
    float min_gaps_total = kInterChipGapCount * kChipMinGap;
    float strip_w_min = chips_total_w + min_gaps_total + divider_total;
    // Slack = unused horizontal space at minimum spacing. Distribute it
    // evenly across the inter-chip gaps so the strip fills the bar.
    float slack = available_w - strip_w_min;
    float gap = kChipMinGap;
    if (slack > 0.0f && kInterChipGapCount > 0) {
        gap = kChipMinGap + slack / static_cast<float>(kInterChipGapCount);
    }

    // When focus is on the poster grid, fade the top-row chips so the user
    // can still see what their options are but immediately knows their
    // input won't affect chip selection. The currently-active content
    // chip stays at full alpha — it's the breadcrumb that says "you're
    // inside Popular".
    const float kInactiveChipFade = 0.4f;

    float cursor_x = kPaddingX;
    for (int i = 0; i < kNumCategories; ++i) {
        Category cat = static_cast<Category>(i);
        const char* label = label_for_category(cat);
        bool nav = is_nav_chip(cat);
        bool is_active = (!nav && i == static_cast<int>(category_));
        bool is_focused = (focus_ == Focus::CategoryStrip && i == category_cursor_);

        // Per-chip alpha multiplier: when focus is on the poster grid (or
        // filter panel), fade every top-strip chip EXCEPT the currently
        // active content chip, which stays bright as a breadcrumb.
        float chip_alpha_mul =
            (focus_ != Focus::CategoryStrip && !is_active)
                ? kInactiveChipFade
                : 1.0f;

        // Border color: gold for content chips, steel-blue for nav chips.
        // Active content chip stays full-alpha gold; inactive content
        // chips fade. Nav chips fade unless focused.
        ::ui::Color border_color = nav ? th.accent2 : th.accent;
        float border_alpha;
        if (is_focused)             border_alpha = 1.0f;
        else if (is_active)         border_alpha = 1.0f;
        else if (nav)               border_alpha = 0.85f;
        else                        border_alpha = 0.75f;
        float border_w = is_focused ? kChipFocusBorderW : kChipBorderW;

        float chip_w = chip_widths[i];
        r.mb_stroke_rect(cursor_x, chip_y, chip_w, kChipH,
                         border_w, border_color, border_alpha * chip_alpha_mul);

        // Text color matches the border so each chip reads as a single
        // visual unit. Slight alpha bump for active/focused states.
        ::ui::Color text_color = nav ? th.accent2 : th.accent;
        float text_alpha;
        if (is_focused)             text_alpha = 1.0f;
        else if (is_active)         text_alpha = 1.0f;
        else if (nav)               text_alpha = 0.9f;
        else                        text_alpha = 0.8f;

        // Center the label in the FULL chip width. Chips are now sized
        // generously enough (kChipPadX bumped + kMarkerZoneW reserved in
        // the width formula) that most labels are shorter than
        // chip_w - 2*kChipPadX - kMarkerZoneW, so the cursor still sits
        // over visually empty space at the right edge — no need to keep
        // the label off-center to make room for it.
        float tx = cursor_x + (chip_w - chip_text_widths[i]) / 2.0f;  // centered in full chip
        float ty = chip_y + (kChipH - static_cast<float>(chip_font)) / 2.0f
                 + static_cast<float>(chip_baseline);
        r.mb_draw_text(label, tx, ty, chip_font, text_color, text_alpha * chip_alpha_mul);

        // Active content chip gets a green "now-active" dot in the top-
        // left corner — same highlight1 color as the home-menu's now-
        // playing dot. Tiny but informative; the focus cursor is a
        // separate signal so the two don't conflict visually. The active
        // chip never fades (chip_alpha_mul == 1.0f when is_active), but
        // we still multiply for consistency.
        if (is_active && !is_focused) {
            float dot_r = 3.0f;
            r.mb_fill_rect(cursor_x + 6.0f, chip_y + 6.0f,
                           dot_r * 2.0f, dot_r * 2.0f,
                           th.highlight1, 1.0f * chip_alpha_mul);
        }

        // Blinking ◂ marker centered inside the reserved right-side
        // marker zone. Geometry matches DetailScreen's action-button
        // cursor exactly. Gated on focus_ == CategoryStrip — the marker
        // is a "you are here" cue specific to chip-strip focus, and
        // is_focused already implies that, but we belt-and-suspenders
        // it so the marker can never fire while focus is on the grid.
        if (is_focused && blink_on && focus_ == Focus::CategoryStrip) {
            float marker_cx = cursor_x + chip_w - kMarkerZoneW * 0.5f;
            draw_focus_cursor_at(r, marker_cx,
                                 chip_y + kChipH / 2.0f,
                                 static_cast<float>(chip_font),
                                 th.accent2);
        }

        cursor_x += chip_w;
        // After the last content chip, draw a short steel-blue divider
        // bar (matches DetailScreen's section dividers in color, but
        // half-height because it's separating sibling chips, not regions).
        if (i == kNumContentCategories - 1) {
            cursor_x += kChipDividerPadX;
            float div_h = kChipH * 0.7f;
            float div_y = chip_y + (kChipH - div_h) / 2.0f;
            r.mb_fill_rect(cursor_x, div_y, kChipDividerW, div_h,
                           th.accent2, 0.85f);
            cursor_x += kChipDividerW + kChipDividerPadX;
        } else if (i < kNumCategories - 1) {
            cursor_x += gap;
        }
    }

    // 2px steel-blue rule below the chip strip — the section divider
    // separating "navigation" chrome from the content area below.
    float strip_rule_y = chip_y + kChipH + 14.0f;
    r.mb_draw_line(kPaddingX, strip_rule_y,
                   w - kPaddingX, strip_rule_y,
                   2.0f, th.accent2, kSectionRuleAlpha);

    // --- Services-offline notice -------------------------------------
    // Outline-only treatment to match the rest of the chrome — no big
    // red bar across the screen. Just a thin highlight2 rule and a
    // centered warning string. Failure-state styling, not panic-state.
    float content_top = strip_rule_y + 12.0f;
    if (!services_ok_) {
        const std::string msg =
            "RADARR OFFLINE — check `docker ps` on Pi";
        int sz = th.font_small_size;
        int baseline = r.mb_text_baseline(sz);
        int tw = r.mb_text_width(msg, sz);
        float tx = (w - static_cast<float>(tw)) / 2.0f;
        float ty = content_top + static_cast<float>(baseline);
        r.mb_draw_text(msg, tx, ty, sz, th.highlight2, 1.0f);
        content_top += static_cast<float>(sz) + 12.0f;
    }

    // --- Filter panel (Phase B) --------------------------------------
    // Three outlined chips showing Genre / Year / Sort, laid out like
    // the category strip's chips but bigger and label-rich. Focused
    // chip blinks ◂ in steel-blue, identical to the strip cursor.
    float panel_h = 0.0f;
    if (category_ == Category::Filter) {
        // Compose the three labels.
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

        std::array<std::string, 3> labels = {
            genre_label(), year_label(), sort_label()};

        int pf_size = th.font_medium_size;
        int pf_baseline = r.mb_text_baseline(pf_size);

        // Three equal-width chips spanning the content area minus gaps.
        float panel_x = kPaddingX;
        float panel_w = w - 2.0f * kPaddingX;
        float chip_w = std::max(kFilterChipMinW,
            (panel_w - 2.0f * kFilterChipGap) / 3.0f);
        float panel_y = content_top + kFilterPanelGap;

        // SECTION LABEL above the chips ("FILTER") — small caps, steel-
        // blue, exactly like DetailScreen's "CAST"/"DIRECTED BY" labels.
        {
            int lbl_sz = th.font_small_size;
            int lbl_baseline = r.mb_text_baseline(lbl_sz);
            r.mb_draw_text("FILTER", kPaddingX,
                           panel_y - 4.0f + static_cast<float>(lbl_baseline)
                                          - static_cast<float>(lbl_sz),
                           lbl_sz, th.accent2, 0.95f);
        }

        // Same fade rule as the category strip: when focus is on the
        // poster grid, the filter panel chips fade so the user knows
        // input won't affect them. (When focus IS on the filter panel,
        // no fade.)
        float panel_alpha_mul =
            (focus_ == Focus::PosterGrid) ? kInactiveChipFade : 1.0f;

        for (int i = 0; i < 3; ++i) {
            bool is_row_focused =
                (focus_ == Focus::FilterPanel && i == static_cast<int>(filter_row_));
            float cx = panel_x + i * (chip_w + kFilterChipGap);

            float border_w = is_row_focused ? kChipFocusBorderW : kChipBorderW;
            float border_alpha = is_row_focused ? 1.0f : 0.85f;
            r.mb_stroke_rect(cx, panel_y, chip_w, kFilterChipH,
                             border_w, th.accent, border_alpha * panel_alpha_mul);

            // Center the label in the FULL chip width. Same change as
            // the category strip: chips are sized with kMarkerZoneW
            // reserved in the width formula, so centering in the full
            // width still leaves visual space on the right where the
            // cursor sits. truncate_to_width still uses the
            // (chip_w - 2*kChipPadX - kMarkerZoneW) area as the budget
            // so long labels get an ellipsis before they crowd the
            // cursor, but the resulting text is centered in the full
            // chip width.
            float label_area_w = chip_w - 2.0f * kChipPadX - kMarkerZoneW;
            std::string txt =
                truncate_to_width(r, labels[i], pf_size, label_area_w);
            float text_w = static_cast<float>(r.mb_text_width(txt, pf_size));
            float tx = cx + (chip_w - text_w) / 2.0f;  // centered in full chip
            float ty = panel_y + (kFilterChipH - static_cast<float>(pf_size)) / 2.0f
                     + static_cast<float>(pf_baseline);
            r.mb_draw_text(txt, tx, ty, pf_size, th.accent,
                           (is_row_focused ? 1.0f : 0.9f) * panel_alpha_mul);

            if (is_row_focused && blink_on && focus_ == Focus::FilterPanel) {
                float marker_cx = cx + chip_w - kMarkerZoneW * 0.5f;
                draw_focus_cursor_at(r, marker_cx,
                                     panel_y + kFilterChipH / 2.0f,
                                     static_cast<float>(pf_size),
                                     th.accent2);
            }
        }

        panel_h = kFilterPanelGap + kFilterChipH + 14.0f;

        // Section divider after the panel.
        float panel_rule_y = panel_y + kFilterChipH + 12.0f;
        r.mb_draw_line(kPaddingX, panel_rule_y,
                       w - kPaddingX, panel_rule_y,
                       2.0f, th.accent2, kSectionRuleAlpha);
    }

    // --- Poster grid --------------------------------------------------
    // 5 columns (was 4) so the user can scan more of the catalogue at a
    // glance. Cell width is computed from the interior width and column
    // gap, then poster height from a 2:3 portrait aspect (TMDB's native
    // poster aspect). Smaller posters are an acceptable trade — the user
    // explicitly asked for a denser grid.
    float grid_top    = content_top + panel_h + kGridPaddingTop;
    float grid_bottom = h - kBottomBarHeight;
    float grid_h      = grid_bottom - grid_top;

    // cell_w fits kGridCols cells across with (kGridCols-1) gaps of size
    // kCellGap inside the kGridPaddingX-padded interior. Posters use the
    // 2:3 movie-poster aspect ratio so they always look right.
    float grid_interior_w = w - 2.0f * kGridPaddingX;
    float cell_w = (grid_interior_w
                    - static_cast<float>(kGridCols - 1) * kCellGap)
                 / static_cast<float>(kGridCols);
    float poster_w = cell_w;
    float poster_h = poster_w * kPosterAspect;
    float cell_h = poster_h + kLabelAreaH;

    int visible_rows = std::max(1,
        static_cast<int>(grid_h / (cell_h + kCellPaddingY)));

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
        float msg_x = (w - static_cast<float>(msg_w)) / 2.0f;
        float msg_y = grid_top + grid_h / 2.0f
                    + static_cast<float>(r.mb_text_baseline(msg_size));
        r.mb_draw_text(msg, msg_x, msg_y, msg_size, msg_color, 0.9f);
    }

    for (int row = scroll_row_; row < end_row; ++row) {
        for (int col = 0; col < kGridCols; ++col) {
            int idx = row * kGridCols + col;
            if (idx >= static_cast<int>(movies_.size())) break;
            const auto& m = movies_[idx];

            float cell_x = kGridPaddingX + col * (cell_w + kCellGap);
            float cell_y = grid_top + (row - scroll_row_) * (cell_h + kCellPaddingY);

            ::ui::Color tint = poster_tint_for_tmdb(m.tmdb_id);
            r.mb_draw_poster_or_tint(m.poster_path,
                                     cell_x, cell_y, poster_w, poster_h,
                                     tint, 1.0f);

            bool focused = (focus_ == Focus::PosterGrid && idx == grid_cursor_);
            // 2px gold border on focus — same treatment as DetailScreen's
            // big-poster frame, no separate inset/outset rectangles. When
            // not focused, no border at all so the grid feels like an
            // unframed contact sheet (the chrome is the strip + dividers).
            if (focused) {
                r.mb_stroke_rect(cell_x, cell_y, poster_w, poster_h,
                                 kPosterBorderW, th.accent, 1.0f);
            }

            // Already-in-library indicator: small green dot in the top-
            // left corner of the poster. Same highlight1 color as the
            // home menu's now-playing dot — communicates "you have
            // this" at a glance, especially useful for the BTN2 quick-
            // add flow (the user knows not to press it).
            if (library_tmdb_ids_.count(m.tmdb_id) > 0) {
                float dot_w = 10.0f;
                r.mb_fill_rect(cell_x + 8.0f, cell_y + 8.0f,
                               dot_w, dot_w, th.highlight1, 1.0f);
            }

            // Title + year in the label area below the poster.
            //
            // Visual-weight strategy: at 9-col density we draw 18 titles
            // per page; if every label is full-bright the grid screams.
            // Quiet the UNFOCUSED labels (alpha 0.55) so they recede into
            // the page; the focused cell's title pops in accent color at
            // full alpha. Net effect: the eye lands on the focused movie
            // immediately, the rest are legible-on-glance support.
            int title_size = th.font_medium_size;
            int title_baseline = r.mb_text_baseline(title_size);
            std::string title = truncate_to_width(
                r, m.title.empty() ? "Untitled" : m.title,
                title_size, cell_w);
            float title_y = cell_y + poster_h + 8.0f
                          + static_cast<float>(title_baseline);
            ::ui::Color title_color = focused ? th.accent : th.fg;
            float title_alpha = focused ? 1.0f : 0.55f;
            r.mb_draw_text(title, cell_x, title_y, title_size, title_color,
                           title_alpha);

            if (m.year > 0) {
                std::string year = std::to_string(m.year);
                int year_size = th.font_small_size;
                int year_baseline = r.mb_text_baseline(year_size);
                float year_y = title_y + static_cast<float>(year_size) * 0.9f
                             + static_cast<float>(year_baseline) * 0.2f;
                float year_alpha = focused ? 0.9f : 0.5f;
                r.mb_draw_text(year, cell_x, year_y, year_size, th.dim,
                               year_alpha);
            }
        }
    }

    // --- Bottom hint --------------------------------------------------
    // Centered dim text, no background bar — matches DetailScreen's
    // footer exactly. The bar/fill style of the old footer fought the
    // outline-only chrome elsewhere on the screen.
    {
        std::string hint;
        if (focus_ == Focus::FilterPanel) {
            hint = "Rotate: change value   RCLICK: next control   "
                   "BTN1/BTN3: up/down   BTN4: home";
        } else {
            hint = "Rotate: nav   RCLICK: open   "
                   "BTN2: quick-add   BTN4: home";
        }
        int sz = th.font_small_size;
        int baseline = r.mb_text_baseline(sz);
        int hw = r.mb_text_width(hint, sz);
        float hx = (w - static_cast<float>(hw)) / 2.0f;
        float hy = h - 12.0f - static_cast<float>(sz)
                 + static_cast<float>(baseline);
        r.mb_draw_text(hint, hx, hy, sz, th.dim, 0.85f);
    }
}

}  // namespace media_browser::ui
