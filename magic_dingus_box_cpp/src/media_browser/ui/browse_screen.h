#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "app/app_state.h"
#include "media_browser/radarr/radarr_types.h"
#include "media_browser/tmdb_client.h"
#include "media_browser/ui/mb_filter_overlay.h"
#include "media_browser/ui/mb_screen.h"

namespace media_browser {
class RadarrClient;
class TmdbClient;
}

namespace media_browser::ui {

// Task 18 + Phase A/B (TMDB Discover overhaul) + v1.6.x Marquee redesign:
// the Browse screen.
//
// Layout (Marquee, post-v1.6.x):
//   - Chrome header (~120px): "Popular" title (left, ZenDots) + 5-tab
//     Marquee strip (right). Strip order, left-to-right:
//       Popular | Top Rated | Library | Search | Settings
//     - Popular and TopRated are content tabs — activating reloads the
//       9-column poster grid from the corresponding TMDB endpoint.
//     - Library, Search, and Settings are transition-only tabs —
//       selecting them returns the corresponding Screen enum value to
//       the dispatcher in main.cpp, which swaps the active screen.
//     - Active tab gets a gold rectangle border + gold label; inactive
//       tabs render in dim cream.
//   - The legacy 9-chip "two-group" layout (Popular/NowPlaying/TopRated/
//     Upcoming/Filter | Search/Library/Queue/Settings) was retired in
//     v1.6.x. NowPlaying was dropped (overlapped Popular on TMDB data).
//     Filter / Upcoming / Queue chips are no longer surfaced through the
//     strip — Filter is deferred to a future Phase C; Queue is reachable
//     via Browse → BTN3 dead-end (will revisit when Queue gets its own
//     entry point). Settings was promoted to the strip's right end.
//   - Main area: 9-column poster grid (2 visible rows of 2:3 cards).
//     Posters use the shared chrome::draw_poster_card helper for the
//     tinted-fill background, IN LIBRARY badge, and bottom-right year
//     pill. Title text wraps to 2 lines below the card.
//   - Bottom bar (~30px): control hints via chrome::draw_footer_hints.
//
// Data source (Phase A):
//   TMDB Discover / category endpoints directly — not Radarr's
//   /movie/lookup search. Radarr stays responsible for library,
//   add-movie, queue. Clean separation.
class BrowseScreen : public MbScreen {
public:
    BrowseScreen(RadarrClient& radarr, TmdbClient& tmdb, ::app::AppState& state);
    ~BrowseScreen();

    void enter() override;
    Screen handle_input(const std::vector<platform::InputEvent>& events) override;
    void update() override;
    void render(::ui::Renderer& r, int screen_w, int screen_h) override;

    // tmdb_id of the poster most recently selected by the user. Consumed by
    // the dispatcher in main.cpp to forward to DetailScreen on transition.
    int selected_tmdb_id() const { return selected_tmdb_id_; }

private:
    // Top-strip chip order. Content chips load a grid; nav chips transition.
    enum class Category {
        Popular = 0,
        NowPlaying = 1,
        TopRated = 2,
        Upcoming = 3,
        Filter = 4,
        Search = 5,
        Library = 6,
        Queue = 7,
        Settings = 8,
    };
    enum class Focus {
        CategoryStrip,
        FilterPanel,   // Phase B — only reachable when Category::Filter is active.
        PosterGrid,
    };

    // Filter panel row / control identifiers.
    enum class FilterRow { Genre = 0, Year = 1, SortBy = 2, Count = 3 };

    static constexpr int kNumContentCategories = 5;  // Popular..Filter
    static constexpr int kNumCategories = 9;
    // 9-column grid: at 1280×720 this fits TWO full rows of 2:3 posters
    // (~119×178 px each) inside the available grid height of 532 px, with
    // 45 px breathing room before the bottom bar. 18 posters visible per
    // page = 3.6× the catalogue density of the prior 5-col layout. Cell
    // width is computed dynamically in render() from the column count +
    // available width, so this is the only knob that needs to change.
    static constexpr int kGridCols = 9;

    static bool is_nav_chip(Category cat) {
        return static_cast<int>(cat) >= kNumContentCategories;
    }

    // Public load entry point. Spawns a background thread that does
    // the (slow, ~6s) TMDB fetch off the render thread; render() can
    // continue to draw in its existing Loading state until update()
    // drains the worker's pending result on a future tick. Idempotent
    // when called repeatedly with the same cat — generation counter
    // handles rapid category-flip without serializing on join.
    void load_category(Category cat);
    void reload_filter_results();
    // Hybrid endpoint reload: uses /popular or /top_rated when no filters
    // are active; switches to /discover/movie when any filter is set.
    // Called from the FilterOverlay commit callback.
    void reload_for_category();
    // Worker entry — runs the synchronous TMDB call off-thread.
    // Each spawned worker fetches ONE page; multiple page workers may be
    // in flight concurrently for the same category load (page 1 + a
    // prefetched page 2 + a scroll-driven page 3 etc.). Results are
    // tagged with their page number so apply_pending() can replace on
    // page 1 / append on page > 1.
    void run_load_page(uint64_t gen, Category cat, int page);
    void run_reload_filter_page(uint64_t gen, DiscoverFilter filter, int page);
    // Spawn a fresh worker for the given category + page under the
    // current generation. Sets fetching_more_ before returning.
    void spawn_page_worker(Category cat, int page);
    // After apply_pending() drains a result, decide whether to spawn
    // the next page worker. Called from update() each frame; cheap
    // when no fetch is needed (a few atomic loads + arithmetic).
    void maybe_load_more_pages();
    // Drains pending results from any completed workers into movies_.
    // Cheap (atomic load most frames); only takes the mutex when at
    // least one result is ready to consume.
    void apply_pending();
    // Lazily fetches /genre/movie/list on first entry to the Filter category.
    void ensure_genres_loaded();
    // Cycle the current filter_row_'s value by `delta` (+1 / -1 typical).
    void cycle_filter_value(int delta);
    // Retired in v1.6.x — was the BTN2 quick-add shortcut, replaced by
    // the back-grammar remap. Preserved intentionally in case a future
    // overlay or shortcut wants the same library-add flow without going
    // through DetailScreen. Add to the new caller, then remove the
    // [[maybe_unused]] when reactivating.
    [[maybe_unused]] void quick_add_focused();

    static const char* label_for_category(Category cat);

    RadarrClient& radarr_;
    TmdbClient& tmdb_;
    ::app::AppState& state_;

    FilterOverlay filter_overlay_;

    Category category_ = Category::Popular;
    Focus focus_ = Focus::PosterGrid;
    int category_cursor_ = 0;   // Index into the top strip when Focus::CategoryStrip.
    int grid_cursor_ = 0;       // Flat index into movies_ when Focus::PosterGrid.
    int scroll_row_ = 0;        // Topmost visible row index.

    // Result set for the active category. Uses TmdbSearchHit directly —
    // the only shape the renderer needs is tmdb_id/title/year/poster URL.
    std::vector<TmdbSearchHit> movies_;
    bool loaded_ = false;
    // True while load_category() is in-flight. Drives the "Loading..." state.
    bool loading_ = false;
    // Snapshot of radarr_.is_reachable() at enter() time. Drives the
    // "Radarr service offline" banner.
    bool services_ok_ = true;

    int selected_tmdb_id_ = 0;
    // Set when handle_input() wants the dispatcher to transition to Search
    // (via the top-strip Search category). Cleared on the next enter().
    bool want_search_screen_ = false;

    // --- Async TMDB fetch state ------------------------------------
    // The TMDB API takes 6+ seconds per call when egressing through
    // the VPN tunnel — that latency was visible as a screen freeze
    // every time the user entered Browse or switched categories. The
    // worker thread does the HTTP call off the render thread; render()
    // shows Loading state until apply_pending() drains the result on
    // a future update() tick.
    //
    // Generation counter pattern (same as ProwlarrClient): each call
    // to load_category bumps current_gen_; the worker captures gen at
    // spawn time and only publishes its result if current_gen_ still
    // matches. This lets a rapid category flip pre-empt a stale
    // worker without blocking the UI on join. Older workers run to
    // completion in the background and silently drop their results.
    std::atomic<uint64_t> tmdb_current_gen_{0};
    std::mutex            tmdb_result_mtx_;
    // Pending results from workers whose gen matches current_gen_.
    // Each entry is one fetched page tagged with its page number, so
    // multiple in-flight workers can publish concurrently without
    // overwriting each other; apply_pending() drains the queue and
    // replaces (page == 1) or appends (page > 1).
    struct PendingPage {
        std::vector<TmdbSearchHit> movies;
        int  page;       // 1, 2, 3, ...
        bool no_more;    // true if this fetch indicates we hit the end of the list
    };
    std::vector<PendingPage>   tmdb_pending_pages_;
    // result_ready_ is the fast atomic check update() uses to skip
    // the lock on every frame when no new result is in flight.
    std::atomic<bool>          tmdb_result_ready_{false};
    // All worker threads spawned during this screen's lifetime.
    // Joined in the destructor so a worker mid-CURL doesn't outlive
    // the BrowseScreen and segfault on result publication.
    std::vector<std::thread>   tmdb_workers_;

    // --- Pagination state (per-category) ---------------------------
    // The active category accumulates pages as the user scrolls. State
    // resets on category switch. TMDB returns 20 results per page; with
    // 9-col 2-row layout (18 visible per page) we prefetch page 2 after
    // page 1 lands so the user has a full second screen ready, then
    // fetch additional pages on-demand as the cursor approaches the
    // loaded end.
    int  next_page_to_fetch_ = 1;     // Next un-fetched page (1-based).
    bool more_available_     = true;  // False once a fetch returns near-empty.
    bool fetching_more_      = false; // A page worker is currently in flight.
    // Hard cap on accumulated movies — protects against unbounded growth
    // if a user somehow scrolls past 100 results in one session. TMDB
    // categories have 500+ pages but the user's appetite doesn't.
    static constexpr int kMaxLoadedPages = 5;  // 5 pages × 20 = ~100 movies.
    // Set of tmdb_ids already in movies_, so subsequent pages that overlap
    // with prior ones (TMDB occasionally re-emits the same movie across
    // page boundaries when its list shifts mid-fetch) don't get duplicate
    // tiles in the grid. Different cuts of the same movie have distinct
    // tmdb_ids, so this is exact-duplicate suppression only.
    std::unordered_set<int> loaded_tmdb_ids_;

    // --- Phase B: filter state -------------------------------------
    DiscoverFilter current_filter_;
    std::vector<Genre> genres_;
    bool genres_loaded_ = false;
    FilterRow filter_row_ = FilterRow::Genre;  // Focused row in the panel.

    // Available sort-by strings (paired with display labels in the .cpp).
    // current_sort_index_ is the index into a static array in the .cpp.
    int current_sort_index_ = 0;

    // --- Quick-add caches (preserved post-v1.6.x, see comment above) ---
    // Quick-add cache: in-library tmdb_id set + quality profile cache.
    // Pre-v1.6.x backed the BTN2 quick-add shortcut; preserved
    // post-grammar-remap because the same caches drive the IN LIBRARY
    // badge on poster cells and may back a future overlay shortcut.
    // Cached tmdb_ids already in the Radarr library, so quick-add doesn't
    // refetch the full library on every press. Populated on enter() and
    // refreshed after a successful add.
    std::unordered_set<int> library_tmdb_ids_;
    std::vector<QualityProfile> quality_profiles_;
    bool library_cached_ = false;
};

}  // namespace media_browser::ui
