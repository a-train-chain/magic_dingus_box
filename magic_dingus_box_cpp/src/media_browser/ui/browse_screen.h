#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "app/app_state.h"
#include "media_browser/media_ref.h"
#include "media_browser/radarr/radarr_types.h"
#include "media_browser/tmdb_client.h"
#include "media_browser/ui/browse_logic.h"
#include "media_browser/ui/mb_filter_overlay.h"
#include "media_browser/ui/mb_filter_state.h"
#include "media_browser/ui/mb_screen.h"
#include "media_browser/ui/worker_pool.h"

namespace media_browser {
class RadarrClient;
class SonarrClient;
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
    BrowseScreen(RadarrClient& radarr, SonarrClient& sonarr, TmdbClient& tmdb,
                 ::app::AppState& state);
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
        ForYou = 5,
        Search = 6,
        Library = 7,
        Queue = 8,
        Settings = 9,
    };
    enum class Focus {
        CategoryStrip,
        FilterPanel,   // Phase B — only reachable when Category::Filter is active.
        PosterGrid,
    };

    // Filter panel row / control identifiers.
    enum class FilterRow { Genre = 0, Year = 1, SortBy = 2, Count = 3 };

    static constexpr int kNumContentCategories = 6;  // Popular..ForYou
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

    // Single source of truth for the Marquee strip — consumed by BOTH
    // handle_input() and render(). Was duplicated in the two functions
    // with a "keep in sync" comment.
    static constexpr Category kVisibleTabs[] = {
        Category::Popular,
        Category::TopRated,
        Category::ForYou,
        Category::Search,
        Category::Library,
        Category::Queue,
        Category::Settings,
    };
    static constexpr int kNumVisibleTabs =
        static_cast<int>(sizeof(kVisibleTabs) / sizeof(kVisibleTabs[0]));

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
    // Shared persist half of the overlay commit (spec 1b): write per-tab
    // filter state + save settings.json, WITHOUT the reload — the commit
    // path adds reload_for_category(), the shuffle path adds do_shuffle().
    void persist_filter_state(MbMode mode_for_write, FilterTabKind tab, const FilterState& fs);
    // The persisted Movies/TV mode. Single source of truth — BrowseScreen
    // keeps no shadow copy, so a mode written by the overlay's toggle handler
    // is visible to every reader on the next line.
    MbMode mode() const { return state_.display_settings.mb_mode; }
    bool tv_mode() const { return mode() == MbMode::Tv; }
    // Applied immediately when the overlay's MODE row toggles: re-kick the
    // library refresh (so the in-library hide has the new kind's set) and
    // reload the active content tab under the new mode.
    void apply_mode_change();
    // Spec 1b shuffle dispatch for the active tab.
    void do_shuffle();
    // Shuffle entry points (spec 1b): mirror load_category's synchronous
    // reset, then spawn the base page of a fresh window.
    void load_shuffle(Category cat, int base_page);
    void load_shuffle_discover(int base_page);
    // Background TTL refresh for the active chart tab (spec 1a): no clear,
    // no Loading state; swap happens in apply_pending only when the result
    // is ok and non-empty.
    void revalidate_active_chart();
    // True when the committed filter state for the active chart tab routes
    // it through /discover (extracted from reload_for_category).
    bool active_chart_filters_active() const;
    // Worker entry — runs the synchronous TMDB call off-thread.
    // Each spawned worker fetches ONE page; multiple page workers may be
    // in flight concurrently for the same category load (page 1 + a
    // prefetched page 2 + a scroll-driven page 3 etc.). Results are
    // tagged with their page number so apply_pending() can replace on
    // page 1 / append on page > 1.
    // `mode_for_page` is captured by VALUE at spawn time: a MODE toggle
    // mid-flight bumps the generation, but pinning the mode keeps the worker's
    // endpoint choice consistent with the filter it was handed.
    void run_load_page(uint64_t gen, Category cat, MbMode mode_for_page, int page,
                       bool is_revalidate = false);
    void run_reload_filter_page(uint64_t gen, DiscoverFilter filter, int page,
                                bool is_revalidate = false);
    void run_reload_tv_filter_page(uint64_t gen, TvDiscoverFilter filter, int page,
                                   bool is_revalidate = false);
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
    SonarrClient& sonarr_;
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
    // Snapshot of radarr_.is_reachable() at refresh time. DIAGNOSTIC ONLY
    // since Phase 2c-1 — render() branches on lib_fetch_ok_[mode()] instead,
    // because a Radarr outage must not blank the TMDB-sourced TV grids.
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
        int  page;         // absolute TMDB page number (window base .. base+4)
        bool no_more;      // true if this fetch indicates we hit the end of the list
        bool ok = true;    // TmdbList.ok — false = fetch/parse failure
        int  total_pages = 0;      // TmdbList.total_pages (0 when unknown)
        bool is_revalidate = false;  // background TTL refresh — skip swap on failure/empty
        std::string discover_sig;    // non-empty for discover fetches → total_pages cache key
        // Generation captured at publish time. The publish-side gen check
        // in run_load_page/run_reload_filter_page only proves the page was
        // current when it was PUSHED — a page can still sit in the queue
        // while Browse isn't ticking update() (user on Detail/Playback) and
        // get drained after a LATER generation bump (e.g. revalidate_active_
        // chart(), which — unlike load_category/load_shuffle — clears none
        // of the pagination state on bump). apply_pending() re-checks this
        // against tmdb_current_gen_ at drain time and skips stale entries.
        uint64_t gen = 0;
    };
    std::vector<PendingPage>   tmdb_pending_pages_;
    // result_ready_ is the fast atomic check update() uses to skip
    // the lock on every frame when no new result is in flight.
    std::atomic<bool>          tmdb_result_ready_{false};
    // All worker threads spawned during this screen's lifetime.
    // Joined in the destructor so a worker mid-CURL doesn't outlive
    // the BrowseScreen and segfault on result publication.
    // Reaped each update() tick — finished workers no longer pin their
    // thread objects for the process lifetime (see worker_pool.h).
    WorkerPool tmdb_workers_;

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
    std::unordered_set<MediaRef> loaded_refs_;

    // First page of the active pagination window. 1 for normal loads; the
    // random base after a shuffle. maybe_load_more_pages() loads
    // [page_window_base_, window_last_page(page_window_base_)] — the old code
    // treated kMaxLoadedPages as an ABSOLUTE page cap, which would have made
    // any shuffled base >= 6 load a single page and stop (spec 1b).
    int page_window_base_ = 1;
    // True when the active window's follow-up pages must be fetched through
    // /discover rather than the curated /popular or /top_rated endpoint.
    // Pre-existing bug fixed alongside the window-base work: maybe_load_more_
    // pages() used to always spawn scroll-driven pages against `category_`
    // (Popular/TopRated), so a filtered grid's pages 2+ silently came back
    // from the curated endpoint and mixed unfiltered results into a
    // supposedly-filtered grid.
    bool window_is_discover_ = false;
    // When a shuffled base page comes back genuinely empty (ok but 0 hits —
    // possible on the /discover path), fall back to a plain page-1 load.
    bool shuffle_retry_base1_ = false;
    // Age of the active chart grid (Popular/TopRated, curated or discover).
    // Default-constructed = never loaded. Drives the 6h TTL (spec 1a).
    std::chrono::steady_clock::time_point chart_loaded_at_{};
    // Last-seen total_pages per discover filter signature (spec 1b) —
    // key = TmdbClient::build_discover_url("", filter, 1).
    std::unordered_map<std::string, int> discover_total_pages_;

    // --- Phase B: filter state -------------------------------------
    DiscoverFilter current_filter_;
    // TV's discover filter is a SEPARATE type from DiscoverFilter on purpose
    // (different date params, different genre id space) — never one shared
    // struct.
    TvDiscoverFilter current_tv_filter_;
    std::vector<Genre> genres_;
    bool genres_loaded_ = false;
    // Async genre fetch. "Only ~200ms" was the happy path — the TMDB
    // client's retry ladder holds a dead egress for up to ~76s, and the
    // old synchronous call ran on the render thread (WatchdogSec=10).
    // Single-flight via genres_fetching_; worker publishes under the
    // mutex, render thread drains in update().
    std::atomic<bool> genres_fetching_{false};
    std::atomic<bool> genres_ready_{false};
    std::mutex genres_mtx_;
    std::vector<Genre> pending_genres_;
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
    std::unordered_set<MediaRef> library_refs_;
    // tmdb_ids of movies currently in the Radarr download queue. Populated
    // alongside library_refs_ on enter() by calling get_queue() and
    // cross-referencing with the library's radarr_id → tmdb_id mapping.
    // Drives the DOWNLOADING badge on poster cards.
    std::unordered_set<MediaRef> downloading_refs_;
    std::vector<QualityProfile> quality_profiles_;
    bool library_cached_ = false;

    // --- For You state (spec 1c) -----------------------------------
    // Cached merged list — activation re-renders this without refetching;
    // a new sample runs only on first entry, TTL expiry, or SHUFFLE.
    struct ForYouCache {
        std::vector<TmdbSearchHit> hits;
        std::chrono::steady_clock::time_point loaded_at{};
    };
    // Per mode. Keeping both means a MODE toggle does not throw away a merged
    // grid that cost 8 concurrent TMDB round-trips (~12 s worst case) to
    // build, and each mode gets its own honest 6 h TTL.
    ForYouCache foryou_[2];
    ForYouCache& foryou() { return foryou_[static_cast<int>(mode())]; }
    // One in-flight sample job. Workers capture the shared_ptr; a stale job
    // (gen mismatch) is simply never consumed. remaining==0 → ready to merge.
    struct SeedResult {
        bool ok = false;                    // TmdbList.ok of whichever call served it
        std::vector<TmdbSearchHit> hits;
    };
    struct ForYouJob {
        uint64_t gen = 0;
        bool background = false;            // TTL refresh — keep old grid on total failure
        MbMode mode = MbMode::Movies;   // which cache this job's result belongs to
        std::atomic<int> remaining{0};
        std::mutex mtx;
        std::vector<SeedResult> results;
    };
    std::shared_ptr<ForYouJob> foryou_job_;
    bool foryou_waiting_for_library_ = false;  // sample deferred until refresh lands
    bool foryou_failed_ = false;               // all seeds failed on an explicit load
    // Library-refresh outcome flags (spec 1c): set by apply_library_pending.
    bool lib_refresh_done_once_ = false;
    // Indexed by mode: Radarr answers for Movies, Sonarr for Tv.
    bool lib_fetch_ok_[2] = {false, false};
    void activate_foryou();
    void start_foryou_sample(bool background);
    void apply_foryou_pending();

    // --- Async Radarr library/services refresh (mirrors LibraryScreen) ---
    // enter() used to call is_reachable() + get_library() + get_queue()
    // (+ get_quality_profiles() on first entry) SYNCHRONOUSLY on the render
    // thread — 3-4 blocking HTTP round-trips (~200ms-1s+ over the VPN egress)
    // that stalled the whole kiosk every time the operator opened the movie
    // marquee. Those calls now run on lib_refresh_worker_; apply_library_pending()
    // drains the result on the render thread on the next update() tick.
    //
    // Correctness note for quick_add_focused(): it reads library_refs_ and
    // quality_profiles_. During the async window those sets keep the PREVIOUS
    // visit's data (they're only replaced atomically in apply_library_pending(),
    // never cleared first), so quick-add always sees complete — if up to one
    // refresh-cycle stale — data. Same staleness tolerance LibraryScreen and
    // QueueScreen already accept. On the very first entry the sets are empty,
    // which reads as "not in library yet" — identical to the pre-async state
    // during the blocking fetch, just non-blocking now.
    struct PendingLibrary {
        bool                        services_ok = false;
        // Named movie_refs (not library_refs) from the start: Task 8 adds a
        // tv_refs sibling, and renaming this field twice would churn the same
        // call sites for no benefit.
        std::unordered_set<MediaRef> movie_refs;
        // Kept per kind so a refresh where only one service answered replaces
        // just that service's contribution (see replace_refs_of_kind).
        std::unordered_set<MediaRef> tv_refs;
        std::unordered_set<MediaRef> downloading_refs;
        std::vector<QualityProfile> quality_profiles;
        bool                        quality_fetched = false;
        bool                        movie_fetch_ok = false;
        bool                        tv_fetch_ok    = false;
    };
    void refresh_library_async();              // non-blocking; spawns worker
    void run_library_refresh(bool fetch_quality);  // worker body (off render)
    void apply_library_pending();              // drain on render thread
    std::mutex        lib_pending_mtx_;
    PendingLibrary    lib_pending_;
    std::atomic<bool> lib_result_ready_{false};
    std::atomic<bool> lib_refresh_in_flight_{false};
    std::thread       lib_refresh_worker_;
};

}  // namespace media_browser::ui
