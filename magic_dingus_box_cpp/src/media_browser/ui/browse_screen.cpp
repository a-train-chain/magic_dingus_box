#include "media_browser/ui/browse_screen.h"
#include "media_browser/ui/mb_chrome.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>

#include <spdlog/spdlog.h>

#include "app/settings_persistence.h"
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

// ---------------------------------------------------------------------------
// Filter overlay helpers (Task 4 of v1.6.4: hybrid endpoint switching)
// ---------------------------------------------------------------------------

using FilterTabKind = ::media_browser::ui::FilterTabKind;
using FilterState   = ::media_browser::ui::FilterState;

FilterState read_filter_state(const ::app::AppState::DisplaySettings& s, FilterTabKind tab) {
    FilterState fs;
    if (tab == FilterTabKind::Popular) {
        fs.genre_mask = s.mb_popular_genre_mask;
        fs.decade     = static_cast<int>(s.mb_popular_decade);
        fs.min_rating = static_cast<int>(s.mb_popular_min_rating);
        fs.runtime    = static_cast<int>(s.mb_popular_runtime);
        fs.language   = static_cast<int>(s.mb_popular_language);
        fs.sort       = static_cast<int>(s.mb_popular_sort);
    } else {
        fs.genre_mask = s.mb_toprated_genre_mask;
        fs.decade     = static_cast<int>(s.mb_toprated_decade);
        fs.min_rating = static_cast<int>(s.mb_toprated_min_rating);
        fs.runtime    = static_cast<int>(s.mb_toprated_runtime);
        fs.language   = static_cast<int>(s.mb_toprated_language);
        fs.sort       = static_cast<int>(s.mb_toprated_sort);
    }
    return fs;
}

void write_filter_state(::app::AppState::DisplaySettings& s, FilterTabKind tab,
                        const FilterState& fs) {
    using DS = ::app::AppState::DisplaySettings;
    if (tab == FilterTabKind::Popular) {
        s.mb_popular_genre_mask = fs.genre_mask;
        s.mb_popular_decade     = static_cast<DS::MbDecade>(fs.decade);
        s.mb_popular_min_rating = static_cast<DS::MbMinRating>(fs.min_rating);
        s.mb_popular_runtime    = static_cast<DS::MbRuntime>(fs.runtime);
        s.mb_popular_language   = static_cast<DS::MbLanguage>(fs.language);
        s.mb_popular_sort       = static_cast<DS::MbDiscoverSort>(fs.sort);
    } else {
        s.mb_toprated_genre_mask = fs.genre_mask;
        s.mb_toprated_decade     = static_cast<DS::MbDecade>(fs.decade);
        s.mb_toprated_min_rating = static_cast<DS::MbMinRating>(fs.min_rating);
        s.mb_toprated_runtime    = static_cast<DS::MbRuntime>(fs.runtime);
        s.mb_toprated_language   = static_cast<DS::MbLanguage>(fs.language);
        s.mb_toprated_sort       = static_cast<DS::MbDiscoverSort>(fs.sort);
    }
}

bool any_filter_active(const FilterState& fs, FilterTabKind tab) {
    using DS = ::app::AppState::DisplaySettings;
    if (fs.genre_mask != 0) return true;
    if (fs.decade != 0)     return true;
    if (fs.min_rating != 0) return true;
    if (fs.runtime != 0)    return true;
    if (fs.language != 0)   return true;
    // Default sort differs per tab.
    if (tab == FilterTabKind::Popular &&
        fs.sort != static_cast<int>(DS::MbDiscoverSort::Popularity)) return true;
    if (tab == FilterTabKind::TopRated &&
        fs.sort != static_cast<int>(DS::MbDiscoverSort::TopRated))   return true;
    return false;
}

::media_browser::DiscoverFilter build_discover_filter(const FilterState& fs,
                                                      FilterTabKind tab) {
    ::media_browser::DiscoverFilter df;

    // Genre IDs from mask.
    const auto& gids = ::media_browser::ui::filter_overlay_genre_ids();
    for (int i = 0; i < static_cast<int>(gids.size()); ++i) {
        if ((fs.genre_mask & (1u << i)) != 0) df.genre_ids.push_back(gids[i]);
    }

    // Decade → year range.
    using D = ::app::AppState::DisplaySettings::MbDecade;
    auto dec = static_cast<D>(fs.decade);
    auto set_year_range = [&](int start, int end) {
        df.primary_release_year_gte = start;
        df.primary_release_year_lte = end;
    };
    switch (dec) {
        case D::D2020s:  set_year_range(2020, 2029); break;
        case D::D2010s:  set_year_range(2010, 2019); break;
        case D::D2000s:  set_year_range(2000, 2009); break;
        case D::D1990s:  set_year_range(1990, 1999); break;
        case D::D1980s:  set_year_range(1980, 1989); break;
        case D::D1970s:  set_year_range(1970, 1979); break;
        case D::Classic: df.primary_release_year_lte = 1969; break;
        case D::Any:     break;
    }

    // Min rating.
    using R = ::app::AppState::DisplaySettings::MbMinRating;
    auto rat = static_cast<R>(fs.min_rating);
    if (rat == R::Six)   df.vote_average_gte = 6.0f;
    if (rat == R::Seven) df.vote_average_gte = 7.0f;
    if (rat == R::Eight) df.vote_average_gte = 8.0f;

    // Runtime.
    using T = ::app::AppState::DisplaySettings::MbRuntime;
    auto rt = static_cast<T>(fs.runtime);
    if (rt == T::Under90)      { df.with_runtime_lte = 89; }
    if (rt == T::Range90To120) { df.with_runtime_gte = 90;  df.with_runtime_lte = 120; }
    if (rt == T::Range2To3Hr)  { df.with_runtime_gte = 121; df.with_runtime_lte = 180; }
    if (rt == T::Over3Hr)      { df.with_runtime_gte = 181; }

    // Language.
    using L = ::app::AppState::DisplaySettings::MbLanguage;
    auto lang = static_cast<L>(fs.language);
    auto set_lang = [&](const char* code) { df.with_original_language = std::string{code}; };
    switch (lang) {
        case L::English:  set_lang("en"); break;
        case L::Japanese: set_lang("ja"); break;
        case L::French:   set_lang("fr"); break;
        case L::Spanish:  set_lang("es"); break;
        case L::Korean:   set_lang("ko"); break;
        case L::Italian:  set_lang("it"); break;
        case L::German:   set_lang("de"); break;
        case L::Hindi:    set_lang("hi"); break;
        case L::Mandarin: set_lang("zh"); break;
        case L::Any:      break;
    }

    // Sort + baseline vote_count gate.
    using S = ::app::AppState::DisplaySettings::MbDiscoverSort;
    auto sort = static_cast<S>(fs.sort);
    switch (sort) {
        case S::Popularity:    df.sort_by = "popularity.desc"; break;
        case S::TopRated:      df.sort_by = "vote_average.desc"; break;
        case S::MostVoted:     df.sort_by = "vote_count.desc"; break;
        case S::RecentRelease: df.sort_by = "primary_release_date.desc"; break;
    }
    // Baseline vote_count gate matches what /popular and /top_rated bake in.
    df.vote_count_gte = (tab == FilterTabKind::Popular) ? 200 : 300;

    return df;
}

}  // namespace

BrowseScreen::BrowseScreen(RadarrClient& radarr, TmdbClient& tmdb,
                           ::app::AppState& state)
    : radarr_(radarr), tmdb_(tmdb), state_(state) {}

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
    // the Loading mode, then spawn a page-1 worker. Page 2 will be
    // prefetched automatically when page 1 lands; pages 3+ load on
    // demand as the cursor approaches the loaded end.
    //
    // The worker calls the appropriate TMDB endpoint off the render
    // thread — that call takes 6+ seconds over the VPN tunnel, which
    // is what was freezing the screen.
    movies_.clear();
    grid_cursor_ = 0;
    scroll_row_ = 0;
    next_page_to_fetch_ = 1;
    more_available_ = true;
    fetching_more_ = false;
    loaded_tmdb_ids_.clear();
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
    // Bump the generation so any in-flight workers from a previous
    // category drop their results when they finish. The new worker
    // captures this gen via spawn_page_worker.
    tmdb_current_gen_.fetch_add(1);
    spdlog::info("[BrowseScreen] load_category: {} (gen={})",
                 label_for_category(cat), tmdb_current_gen_.load());
    spawn_page_worker(cat, /*page=*/1);
}

void BrowseScreen::spawn_page_worker(Category cat, int page) {
    fetching_more_ = true;
    next_page_to_fetch_ = page + 1;
    const uint64_t gen = tmdb_current_gen_.load();
    if (cat == Category::Filter) {
        // Filter goes through discover(); branch on category here so
        // load_category(Filter) and scroll-driven follow-up pages both
        // hit the right endpoint. Pass filter by value to avoid races
        // with the user mutating current_filter_ while we fetch.
        tmdb_workers_.emplace_back(&BrowseScreen::run_reload_filter_page,
                                   this, gen, current_filter_, page);
    } else {
        tmdb_workers_.emplace_back(&BrowseScreen::run_load_page,
                                   this, gen, cat, page);
    }
}

void BrowseScreen::run_load_page(uint64_t gen, Category cat, int page) {
    // Captured-by-value: do the slow TMDB call here. If the user
    // switches categories before this returns, current_gen_ will
    // bump and our publish will be silently discarded.
    std::vector<TmdbSearchHit> result;
    switch (cat) {
        case Category::Popular:    result = tmdb_.get_popular(page);     break;
        case Category::NowPlaying: result = tmdb_.get_now_playing(page); break;
        case Category::TopRated:   result = tmdb_.get_top_rated(page);   break;
        case Category::Upcoming:   result = tmdb_.get_upcoming(page);    break;
        default: break;
    }

    // Heuristic for "no more pages": TMDB returns 20/page; the family-
    // safe filter trims a few. < 5 means we've effectively run out.
    const bool no_more = result.size() < 5;

    // Stale-check: bail without publishing if a newer load_category
    // has been requested. The new request's worker will write the
    // current result; we'd just clobber its in-flight state.
    if (gen != tmdb_current_gen_.load()) {
        spdlog::info("[BrowseScreen] page={} gen={} stale at publish (current={}); discarding",
                     page, gen, tmdb_current_gen_.load());
        return;
    }
    {
        std::lock_guard<std::mutex> lk(tmdb_result_mtx_);
        if (gen != tmdb_current_gen_.load()) return;  // re-check under lock
        tmdb_pending_pages_.push_back({std::move(result), page, no_more});
    }
    tmdb_result_ready_.store(true);
}

void BrowseScreen::reload_filter_results() {
    movies_.clear();
    grid_cursor_ = 0;
    scroll_row_ = 0;
    next_page_to_fetch_ = 1;
    more_available_ = true;
    fetching_more_ = false;
    loaded_tmdb_ids_.clear();
    loading_ = true;
    tmdb_current_gen_.fetch_add(1);
    spdlog::info(
        "[BrowseScreen] discover (async, gen={}): genre_ids.size={} year_gte={} year_lte={} sort_by={}",
        tmdb_current_gen_.load(),
        current_filter_.genre_ids.size(),
        current_filter_.primary_release_year_gte.value_or(-1),
        current_filter_.primary_release_year_lte.value_or(-1),
        current_filter_.sort_by);
    spawn_page_worker(Category::Filter, /*page=*/1);
}

void BrowseScreen::reload_for_category() {
    // Hybrid endpoint switching: when any filter is active use /discover/movie;
    // otherwise fall back to the canonical /popular or /top_rated endpoint so
    // the user sees TMDB's curated ranking when nothing is filtered.
    if (category_ == Category::Popular || category_ == Category::TopRated) {
        FilterTabKind tab = (category_ == Category::Popular)
                            ? FilterTabKind::Popular
                            : FilterTabKind::TopRated;
        FilterState fs = read_filter_state(state_.display_settings, tab);
        if (any_filter_active(fs, tab)) {
            // Filter active → route through /discover/movie.
            current_filter_ = build_discover_filter(fs, tab);
            reload_filter_results();
            return;
        }
        // No filter → use the curated endpoint.
        load_category(category_);
        return;
    }
    // For other categories just reload normally.
    load_category(category_);
}

void BrowseScreen::run_reload_filter_page(uint64_t gen, DiscoverFilter filter,
                                          int page) {
    auto result = tmdb_.discover(filter, page);
    const bool no_more = result.size() < 5;
    if (gen != tmdb_current_gen_.load()) {
        spdlog::info("[BrowseScreen] discover page={} gen={} stale; discarding",
                     page, gen);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(tmdb_result_mtx_);
        if (gen != tmdb_current_gen_.load()) return;
        tmdb_pending_pages_.push_back({std::move(result), page, no_more});
    }
    tmdb_result_ready_.store(true);
}

void BrowseScreen::apply_pending() {
    if (!tmdb_result_ready_.exchange(false)) return;
    std::vector<PendingPage> drained;
    {
        std::lock_guard<std::mutex> lk(tmdb_result_mtx_);
        drained = std::move(tmdb_pending_pages_);
    }
    // Drain in page order so a fast page-2 publish that beats page 1
    // (unlikely but possible if pages 1 and 2 race) still produces
    // correct movies_ ordering.
    std::sort(drained.begin(), drained.end(),
              [](const PendingPage& a, const PendingPage& b) {
                  return a.page < b.page;
              });
    for (auto& pp : drained) {
        // Page 1 is the canonical replacement — wipe state and rebuild
        // the seen-set from scratch. Pages > 1 append, skipping any
        // tmdb_id already loaded so TMDB's occasional cross-page
        // duplicates (same movie listed on page 1 AND page 2 when its
        // list shifts mid-fetch) don't produce duplicate poster tiles.
        size_t added = 0, dups = 0;
        if (pp.page == 1) {
            movies_.clear();
            loaded_tmdb_ids_.clear();
            movies_.reserve(pp.movies.size());
            for (auto& m : pp.movies) {
                if (loaded_tmdb_ids_.insert(m.tmdb_id).second) {
                    movies_.push_back(std::move(m));
                    ++added;
                } else {
                    ++dups;
                }
            }
            grid_cursor_ = 0;
            scroll_row_ = 0;
        } else {
            movies_.reserve(movies_.size() + pp.movies.size());
            for (auto& m : pp.movies) {
                if (loaded_tmdb_ids_.insert(m.tmdb_id).second) {
                    movies_.push_back(std::move(m));
                    ++added;
                } else {
                    ++dups;
                }
            }
        }
        if (pp.no_more) more_available_ = false;
        spdlog::info("[BrowseScreen] applied page {}: +{} movies, {} dup(s) "
                     "skipped (total {})",
                     pp.page, added, dups, movies_.size());
    }
    if (!drained.empty()) {
        loading_ = false;
        fetching_more_ = false;
    }
}

void BrowseScreen::maybe_load_more_pages() {
    // Don't fetch while another fetch is in flight, or after we've
    // confirmed end-of-list, or for nav chips, or while still in the
    // initial loading state. Hard-cap at kMaxLoadedPages.
    if (fetching_more_ || loading_) return;
    if (!more_available_) return;
    if (is_nav_chip(category_)) return;
    if (next_page_to_fetch_ > kMaxLoadedPages) return;

    // Auto-prefetch page 2 immediately after page 1 lands so the user
    // has a full second screen ready before they scroll. After that,
    // trigger when the focused row is within 1 row of the loaded end.
    const int rows_loaded = movies_.empty()
        ? 0
        : (static_cast<int>(movies_.size()) + kGridCols - 1) / kGridCols;
    const int cursor_row = grid_cursor_ / kGridCols;
    const bool prefetch_page2 = (next_page_to_fetch_ == 2);
    const bool near_end = (rows_loaded > 0) && (cursor_row >= rows_loaded - 1);
    if (!prefetch_page2 && !near_end) return;

    spdlog::info("[BrowseScreen] auto-fetching page {} ({})",
                 next_page_to_fetch_,
                 prefetch_page2 ? "page-2 prefetch" : "scroll-driven");
    spawn_page_worker(category_, next_page_to_fetch_);
}

void BrowseScreen::update() {
    filter_overlay_.tick();
    apply_pending();
    maybe_load_more_pages();
}

BrowseScreen::~BrowseScreen() {
    // Bump gen so any in-flight worker sees its result is stale and
    // bails before publishing. Then join all tracked workers so we
    // don't have a thread holding references to *this after the
    // screen is destroyed. Each worker is bounded by TmdbClient's
    // per-attempt 25s curl timeout × up to 3 attempts with 1.25s of
    // backoff, so worst-case shutdown wait is ~76s if the network
    // is genuinely unresponsive. In practice (link healthy) workers
    // complete in 1-7s. The longer ceiling is an accepted trade for
    // resilience under transient VPN-egress flakiness.
    tmdb_current_gen_.fetch_add(1);
    for (auto& t : tmdb_workers_) {
        if (t.joinable()) t.join();
    }
}

void BrowseScreen::cycle_filter_value(int delta) {
    if (delta == 0) return;
    switch (filter_row_) {
        case FilterRow::Genre: {
            // Ordering: "Any" (no selection) then genres_ in order.
            // Build a flat cycle list of size 1 + genres_.size().
            int n = 1 + static_cast<int>(genres_.size());
            if (n <= 0) return;
            // Find current index — single-select: use first genre_id if any.
            int idx = 0;
            if (!current_filter_.genre_ids.empty()) {
                int gid = current_filter_.genre_ids[0];
                for (size_t i = 0; i < genres_.size(); ++i) {
                    if (genres_[i].id == gid) { idx = 1 + static_cast<int>(i); break; }
                }
            }
            idx = ((idx + delta) % n + n) % n;
            if (idx == 0) current_filter_.genre_ids.clear();
            else current_filter_.genre_ids = { genres_[idx - 1].id };
            break;
        }
        case FilterRow::Year: {
            // Cycle: Any -> 1970..current_year -> Any.
            const int lo = 1970;
            const int hi = current_year_now();
            int n = 1 + (hi - lo + 1);
            if (n <= 0) return;
            // Use gte as the representative value; lte tracks it identically
            // (exact-year filter: gte == lte).
            int idx = 0;
            if (current_filter_.primary_release_year_gte.has_value()) {
                int y = *current_filter_.primary_release_year_gte;
                if (y >= lo && y <= hi) idx = 1 + (y - lo);
            }
            idx = ((idx + delta) % n + n) % n;
            if (idx == 0) {
                current_filter_.primary_release_year_gte.reset();
                current_filter_.primary_release_year_lte.reset();
            } else {
                int y = lo + (idx - 1);
                current_filter_.primary_release_year_gte = y;
                current_filter_.primary_release_year_lte = y;
            }
            break;
        }
        case FilterRow::SortBy: {
            int n = static_cast<int>(kSortOptions.size());
            if (n <= 0) return;
            current_sort_index_ =
                ((current_sort_index_ + delta) % n + n) % n;
            current_filter_.sort_by = kSortOptions[current_sort_index_].value;
            break;
        }
        default: return;
    }
    reload_filter_results();
}

Screen BrowseScreen::handle_input(const std::vector<platform::InputEvent>& events) {
    // Marquee 5-tab strip — content tabs left, transition tabs right.
    // Display order, left-to-right:
    //   Popular · Top Rated · Library · Search · Settings
    // BTN1 (PREV, yellow) walks left; BTN3 (NEXT, green) walks right;
    // movement stops at the ends (no wrap). Library, Search, and Settings
    // are transition-only — selecting them returns the corresponding
    // Screen enum value to the dispatcher in main.cpp, which swaps the
    // active screen. Now Playing was removed in v1.6.x — it overlapped
    // almost completely with Popular on TMDB's data, so collapsing them
    // removes a confusing-looking duplicate. Settings replaces it on the
    // right end of the strip.
    static constexpr Category kVisibleTabs[] = {
        Category::Popular,
        Category::TopRated,
        Category::Search,
        Category::Library,
        Category::Queue,
        Category::Settings,
    };
    constexpr int kNumVisibleTabs =
        static_cast<int>(sizeof(kVisibleTabs) / sizeof(kVisibleTabs[0]));

    // Reverse-lookup: where is category_ in the visible strip? Default to
    // 0 (Popular) if category_ holds a value that isn't a Marquee tab
    // (e.g. legacy persistence from the pre-Marquee 9-chip layout, or
    // NowPlaying from before v1.6.x).
    int strip_pos = 0;
    for (int i = 0; i < kNumVisibleTabs; ++i) {
        if (kVisibleTabs[i] == category_) { strip_pos = i; break; }
    }

    for (const auto& e : events) {
        // ================================================================
        // Overlay input capture: when filter_overlay_ is open/animating,
        // it consumes ROTATE_VERTICAL / SELECT. BTN4 is handled as a
        // toggle below (open/close). PLAY_PAUSE closes the overlay
        // instead of exiting the screen while the overlay is visible.
        // ================================================================
        if (filter_overlay_.is_visible()) {
            if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed) {
                filter_overlay_.on_btn4_close();
                continue;
            }
            if (e.action == platform::InputAction::ROTATE_VERTICAL) {
                if (e.delta != 0) filter_overlay_.on_rotate(e.delta);
                continue;
            }
            if (e.action == platform::InputAction::ROTATE) {
                if (e.delta != 0) filter_overlay_.on_rotate(e.delta);
                continue;
            }
            if (e.action == platform::InputAction::SELECT && e.pressed) {
                filter_overlay_.on_select();
                continue;
            }
            // Block BTN1/BTN3 while overlay is open so the user can't
            // accidentally tab-jump while the panel is in view.
            if (e.action == platform::InputAction::PREV ||
                e.action == platform::InputAction::NEXT) {
                continue;
            }
        }

        // BTN2 (PLAY_PAUSE, red) — back. Browse is the top of the
        // Marquee section, so back-from-Browse exits the Media Browser
        // entirely. Same destination as the existing BTN4 long-press
        // handled by the input dispatcher.
        if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed) {
            return Screen::Exit;
        }

        // BTN4 (SETTINGS_MENU, black) — toggles the filter overlay on
        // Popular + TopRated tabs. On other tabs it remains a no-op
        // (long-press still exits MB → MainMenu via the dispatcher).
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            if (filter_overlay_.is_visible()) {
                filter_overlay_.on_btn4_close();
            } else if (category_ == Category::Popular ||
                       category_ == Category::TopRated) {
                FilterTabKind tk = (category_ == Category::Popular)
                                   ? FilterTabKind::Popular
                                   : FilterTabKind::TopRated;
                filter_overlay_.open(tk, read_filter_state(state_.display_settings, tk));
                filter_overlay_.set_on_commit(
                    [this](const FilterState& fs, FilterTabKind tk2) {
                        write_filter_state(state_.display_settings, tk2, fs);
                        ::app::SettingsPersistence::save_settings(state_);
                        this->reload_for_category();
                    });
            }
            continue;
        }

        // BTN1 (PREV, yellow) — previous tab.
        if (e.action == platform::InputAction::PREV && e.pressed) {
            if (strip_pos == 0) continue;
            const int new_pos = strip_pos - 1;
            const Category new_cat = kVisibleTabs[new_pos];
            if (new_cat == Category::Library)  return Screen::Library;
            if (new_cat == Category::Search)   return Screen::Search;
            if (new_cat == Category::Queue)    return Screen::Queue;
            if (new_cat == Category::Settings) return Screen::MovieSettings;
            category_ = new_cat;
            strip_pos = new_pos;
            load_category(category_);
            focus_ = Focus::PosterGrid;
            continue;
        }

        // BTN3 (NEXT, green) — next tab.
        if (e.action == platform::InputAction::NEXT && e.pressed) {
            if (strip_pos >= kNumVisibleTabs - 1) continue;
            const int new_pos = strip_pos + 1;
            const Category new_cat = kVisibleTabs[new_pos];
            if (new_cat == Category::Library)  return Screen::Library;
            if (new_cat == Category::Search)   return Screen::Search;
            if (new_cat == Category::Queue)    return Screen::Queue;
            if (new_cat == Category::Settings) return Screen::MovieSettings;
            category_ = new_cat;
            strip_pos = new_pos;
            load_category(category_);
            focus_ = Focus::PosterGrid;
            continue;
        }

        // ROTATE (rotary CW/CCW + D-pad LEFT/RIGHT) — walk posters one
        // cell at a time, row-major. Stays inside the loaded grid.
        if (e.action == platform::InputAction::ROTATE) {
            if (movies_.empty()) continue;
            const int n = static_cast<int>(movies_.size());
            grid_cursor_ = std::clamp(grid_cursor_ + e.delta, 0, n - 1);
            continue;
        }

        // ROTATE_VERTICAL (D-pad UP/DOWN) — walk posters one row at a time.
        if (e.action == platform::InputAction::ROTATE_VERTICAL) {
            if (movies_.empty()) continue;
            const int row = grid_cursor_ / kGridCols;
            const int col = grid_cursor_ % kGridCols;
            const int max_row = (static_cast<int>(movies_.size()) - 1) / kGridCols;
            const int new_row = std::clamp(row + e.delta, 0, max_row);
            const int new_idx = new_row * kGridCols + col;
            if (new_idx < static_cast<int>(movies_.size())) {
                grid_cursor_ = new_idx;
            } else if (!movies_.empty()) {
                grid_cursor_ = static_cast<int>(movies_.size()) - 1;
            }
            continue;
        }

        // SELECT (rotary click + gamepad A) — open detail.
        if (e.action == platform::InputAction::SELECT && e.pressed) {
            if (!movies_.empty() && grid_cursor_ >= 0 &&
                grid_cursor_ < static_cast<int>(movies_.size())) {
                selected_tmdb_id_ = movies_[grid_cursor_].tmdb_id;
                return Screen::Detail;
            }
        }
    }

    // Keep scroll_row_ such that grid_cursor_ stays visible. Render uses 2
    // visible rows; clamp here so scroll doesn't get stuck above cursor.
    if (!movies_.empty()) {
        const int row = grid_cursor_ / kGridCols;
        if (row < scroll_row_) scroll_row_ = row;
        // Upper bound is enforced in render() once visible_rows is known.
    }

    return Screen::Browse;
}

// ----------------------------------------------------------------------------
// Rendering — Marquee design system (Phase 3).
// Header + tabs + footer hints come from media_browser::ui::chrome shared
// helpers; the poster grid + library badges + meta line are Browse-specific.
// ----------------------------------------------------------------------------

void BrowseScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    namespace chrome = ::media_browser::ui::chrome;
    const ::ui::Theme& th = r.mb_theme();

    r.mb_fill_background();

    // Same 6-tab strip the handler uses — keep the two in sync.
    // v1.6.x: Now Playing dropped; v1.6.x+: Queue inserted between Library
    // and Settings, Search moved to the left of Library.
    static constexpr Category kVisibleTabs[] = {
        Category::Popular,
        Category::TopRated,
        Category::Search,
        Category::Library,
        Category::Queue,
        Category::Settings,
    };
    constexpr int kNumVisibleTabs =
        static_cast<int>(sizeof(kVisibleTabs) / sizeof(kVisibleTabs[0]));

    // --- Header: "Marquee" title (left) + 5-tab strip (right) ---
    std::vector<chrome::TabSpec> tabs;
    tabs.reserve(kNumVisibleTabs);
    for (int i = 0; i < kNumVisibleTabs; ++i) {
        chrome::TabSpec t;
        t.label = label_for_category(kVisibleTabs[i]);
        t.state = (kVisibleTabs[i] == category_)
                      ? chrome::TabState::Active
                      : chrome::TabState::Inactive;
        tabs.push_back(t);
    }
    const int content_top = chrome::draw_screen_header(
        r, screen_w, "Marquee", tabs, /*focused_tab=*/-1);

    // Library/Search tabs are transition-only — handle_input() returns
    // Screen::Library/Search on tab activation, so render() should never
    // see them. Belt-and-suspenders early return.
    if (category_ == Category::Library || category_ == Category::Search) {
        return;
    }

    // --- Loading / empty / error states ---
    auto draw_centered_msg = [&](const std::string& msg, const ::ui::Color& c) {
        const int tw = r.mb_text_width(msg, 18);
        r.mb_draw_text(msg,
                       static_cast<float>((screen_w - tw) / 2),
                       static_cast<float>(screen_h / 2),
                       18, c);
    };
    const bool filter_available = (category_ == Category::Popular ||
                                   category_ == Category::TopRated);
    auto draw_baseline_footer = [&]() {
        chrome::draw_footer_hints(r, screen_w, screen_h, {
            {chrome::HintIcon::Btn1Yellow,  "Tab \xE2\x86\x90"},
            {chrome::HintIcon::Btn2Red,     "Back"},
            {chrome::HintIcon::Btn3Green,   "Tab \xE2\x86\x92"},
            {chrome::HintIcon::Btn4Black,   filter_available ? "Filter" : "\xE2\x80\x94"},
            {chrome::HintIcon::RotaryNav,   "Posters"},
            {chrome::HintIcon::RotaryPress, "Open"},
        });
    };

    if (!services_ok_) {
        draw_centered_msg("Radarr service offline", th.highlight2);
        draw_baseline_footer();
        return;
    }
    if (loading_ && movies_.empty()) {
        draw_centered_msg("Loading...", th.dim);
        draw_baseline_footer();
        return;
    }
    if (!loading_ && movies_.empty()) {
        draw_centered_msg("No movies in this category", th.dim);
        draw_baseline_footer();
        return;
    }

    // --- 9-column poster grid ---
    // Meta area below each poster fits 2 lines so long titles can wrap.
    // Year was previously appended to the meta line; it now lives inside
    // the poster card (bottom-right, on a semi-transparent dark pill via
    // chrome::draw_poster_card), so the meta line shows only the title.
    constexpr int kCellGap       = 8;
    constexpr int kRowGap        = 22;   // bumped from 16 to give the
                                         // 2-line meta area breathing
                                         // room before the next row
    constexpr int kVisibleRows   = 2;
    constexpr int kMetaFontPx    = 14;   // 14 px legibility floor under CRT
    constexpr int kMetaLineGap   = 2;
    constexpr int kMetaTotalH    = kMetaFontPx + kMetaLineGap + kMetaFontPx; // 30
    constexpr int kMetaGap       = 4;    // poster-bottom → meta-top
    const int content_w = screen_w - 2 * chrome::kSafeInset_px;
    const int cell_w = (content_w - (kGridCols - 1) * kCellGap) / kGridCols;
    const int poster_h = static_cast<int>(static_cast<float>(cell_w) * 1.5f); // 2:3 aspect
    const int cell_h = poster_h + kMetaGap + kMetaTotalH;
    const int grid_top = content_top + chrome::kPad3;
    const int grid_left = chrome::kSafeInset_px;

    // Keep cursor visible. We clamped lower bound in handle_input(); upper
    // bound depends on visible_rows which only exists at render time.
    const int cursor_row = grid_cursor_ / kGridCols;
    if (cursor_row >= scroll_row_ + kVisibleRows) {
        scroll_row_ = cursor_row - kVisibleRows + 1;
    }
    const int total_rows =
        (static_cast<int>(movies_.size()) + kGridCols - 1) / kGridCols;
    const int last_visible_row =
        std::min(scroll_row_ + kVisibleRows, total_rows);

    for (int row = scroll_row_; row < last_visible_row; ++row) {
        for (int col = 0; col < kGridCols; ++col) {
            const int idx = row * kGridCols + col;
            if (idx >= static_cast<int>(movies_.size())) break;
            const auto& movie = movies_[idx];

            const int x = grid_left + col * (cell_w + kCellGap);
            const int y = grid_top + (row - scroll_row_) * (cell_h + kRowGap);

            // Poster CARD: draws the colored tint plus title overlay, year,
            // top/bottom accent dashes, and IN LIBRARY badge — all the
            // Marquee design elements that make the slot read as a
            // designed object even before TMDB artwork loads. When real
            // artwork is later available, mb_draw_poster_or_tint can
            // overlay on top; for now the styled card IS the visual.
            const ::ui::Color tint = poster_tint_for_tmdb(movie.tmdb_id);
            const bool in_library = library_tmdb_ids_.count(movie.tmdb_id) > 0;
            chrome::draw_poster_card(
                r, x, y, cell_w, poster_h,
                movie.title, movie.year,
                tint, in_library, /*download_pct=*/-1,
                // TmdbSearchHit names the URL field `poster_path` even
                // though it's already a fully-resolved CDN URL (see
                // tmdb_client.cpp resolve_poster_url). The artwork
                // cache treats it as opaque, so the field-name mismatch
                // is harmless here.
                /*poster_url=*/movie.poster_path);

            // Meta line below poster: title only, wrapped to 2 lines
            // when needed. Year now lives inside the poster card. If
            // the title fits on one line, line 2 stays empty so the
            // tile reads compact; if not, the longest leading word
            // chunk that fits goes on line 1 and the remainder on
            // line 2 (truncate-with-ellipsis if line 2 also overflows).
            const std::string& title = movie.title;
            const float max_w_f = static_cast<float>(cell_w);
            std::string line1, line2;
            if (r.mb_text_width(title, kMetaFontPx) <= max_w_f) {
                line1 = title;
            } else {
                // Walk forward through whitespace, keep the longest
                // prefix that still fits in one line. Fallback: bisect
                // by character if the title has no spaces.
                size_t split = std::string::npos;
                size_t pos = 0;
                while (true) {
                    size_t next = title.find(' ', pos + 1);
                    if (next == std::string::npos) break;
                    if (r.mb_text_width(title.substr(0, next), kMetaFontPx)
                            > max_w_f) break;
                    split = next;
                    pos = next;
                }
                if (split == std::string::npos) {
                    line1 = truncate_to_width(r, title, kMetaFontPx, max_w_f);
                } else {
                    line1 = title.substr(0, split);
                    std::string remainder = title.substr(split + 1);
                    line2 = truncate_to_width(r, remainder, kMetaFontPx, max_w_f);
                }
            }
            const int meta_top = y + poster_h + kMetaGap;
            r.mb_draw_text(line1,
                           static_cast<float>(x),
                           static_cast<float>(meta_top + kMetaFontPx),
                           kMetaFontPx, th.dim);
            if (!line2.empty()) {
                r.mb_draw_text(line2,
                               static_cast<float>(x),
                               static_cast<float>(meta_top + kMetaFontPx
                                                  + kMetaLineGap + kMetaFontPx),
                               kMetaFontPx, th.dim);
            }

            // Focus ring on the cursor cell. 2 px gold, 2 px outside.
            if (idx == grid_cursor_) {
                chrome::draw_focus_ring(r, x, y, cell_w, poster_h);
            }
        }
    }

    // --- Footer hints ---
    chrome::draw_footer_hints(r, screen_w, screen_h, {
        {chrome::HintIcon::Btn1Yellow,  "Tab \xE2\x86\x90"},
        {chrome::HintIcon::Btn2Red,     "Back"},
        {chrome::HintIcon::Btn3Green,   "Tab \xE2\x86\x92"},
        {chrome::HintIcon::Btn4Black,   filter_available ? "Filter" : "\xE2\x80\x94"},
        {chrome::HintIcon::RotaryNav,   "Posters"},
        {chrome::HintIcon::RotaryPress, "Open"},
    });

    // Render filter overlay on top of everything else (draws above the grid).
    filter_overlay_.render(r, screen_w, screen_h);
}

}  // namespace media_browser::ui
