#include "media_browser/ui/browse_screen.h"
#include "media_browser/ui/mb_chrome.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <random>
#include <string>
#include <system_error>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "app/settings_persistence.h"
#include "media_browser/radarr/radarr_client.h"
#include "media_browser/sonarr/sonarr_client.h"
#include "media_browser/tmdb_client.h"
#include "media_browser/ui/mb_filter_state.h"
#include "media_browser/ui/mb_recs.h"
#include "media_browser/ui/mb_ui_utils.h"
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
using MediaKind     = ::media_browser::MediaKind;
using MediaRef      = ::media_browser::MediaRef;

}  // namespace

BrowseScreen::BrowseScreen(RadarrClient& radarr, SonarrClient& sonarr,
                           TmdbClient& tmdb, ::app::AppState& state,
                           bool sonarr_configured)
    : radarr_(radarr), sonarr_(sonarr), tmdb_(tmdb), state_(state),
      sonarr_configured_(sonarr_configured) {}

void BrowseScreen::enter() {
    want_search_screen_ = false;
    if (!loaded_) {
        load_category(category_);
        loaded_ = true;
    } else if (category_ == Category::ForYou) {
        if (tmdb_grid_stale(foryou().loaded_at, std::chrono::steady_clock::now()) &&
            !foryou().hits.empty()) {
            start_foryou_sample(/*background=*/true);   // spec 1a SWR for For You
        } else if (foryou().hits.empty()) {
            activate_foryou();                          // never loaded — entry rule
        }
    } else if (!is_nav_chip(category_) && category_ != Category::Filter &&
               tmdb_grid_stale(chart_loaded_at_, std::chrono::steady_clock::now())) {
        // Spec 1a: 6h TTL, evaluated on enter() only (tab switches already
        // refetch unconditionally). Stale-while-revalidate — the old grid
        // stays on screen until a fresh page 1 lands.
        revalidate_active_chart();
    }
    // Kick off the Radarr health-check + library/queue fetch on a worker
    // thread instead of blocking the render thread on 3-4 HTTP round-trips.
    // Stale "in library"/"downloading" badges from the previous visit stay
    // visible until apply_library_pending() lands (next update() ticks). See
    // the PendingLibrary block in the header for the quick-add correctness
    // rationale.
    refresh_library_async();
}

void BrowseScreen::refresh_library_async() {
    // CAS guard: at most one refresh in flight. A second enter() (or any
    // future periodic caller) while one is running is a no-op. Snapshot
    // library_cached_ HERE on the render thread and pass it to the worker
    // so the worker never touches a render-thread-owned member.
    bool expected = false;
    if (!lib_refresh_in_flight_.compare_exchange_strong(expected, true)) return;
    // Reap a previous finished-but-not-joined worker before reusing the handle.
    if (lib_refresh_worker_.joinable()) lib_refresh_worker_.join();
    const bool fetch_quality = !library_cached_;
    lib_refresh_worker_ =
        std::thread(&BrowseScreen::run_library_refresh, this, fetch_quality);
}

void BrowseScreen::run_library_refresh(bool fetch_quality) {
    // Worker thread: all blocking Radarr I/O happens here. Touches NO live
    // member except lib_pending_/lib_result_ready_/lib_refresh_in_flight_.
    PendingLibrary r;
    // Sonarr's TV library runs CONCURRENTLY with the Radarr block below, and
    // on EVERY refresh regardless of the active mode — a MODE toggle must not
    // wait on a round-trip before the in-library hide is correct. Concurrency
    // is not an optimisation here: SonarrClient::get_library_checked() is a
    // single http_get with NO retry ladder against Config::timeout_secs = 5,
    // and refresh_library_async() runs on every Browse entry INCLUDING every
    // re-entry from Detail/Playback. Inline-serial would therefore add up to
    // 5 s to every entry for movie-only owners who never open TV mode,
    // delaying the retro-hide sweep, the DOWNLOADING badges and the
    // lib_refresh_done_once_ gate For You blocks on. Worst case is now
    // max(radarr, sonarr), not their sum.
    //
    // No queue cross-reference for TV — nothing is addable in 2c-1, so no TV
    // poster can be downloading. The checked shape already separates "Sonarr
    // said the library is empty" from "the request failed", which is the only
    // signal this needs, so it does not ride on Radarr's services_ok.
    std::unordered_set<MediaRef> tv_refs;
    std::atomic<bool> tv_ok{false};
    // Constructing std::thread can itself throw std::system_error (thread
    // exhaustion — realistic on the 1.5 GB Pi 4B, this project's binding
    // performance envelope). run_library_refresh is ITSELF a std::thread
    // entry function (see refresh_library_async's
    // `std::thread(&BrowseScreen::run_library_refresh, ...)`), so an
    // uncaught exception here would escape it and call std::terminate() —
    // a hard kiosk crash instead of a degraded TV mode. Mirror
    // ensure_genres_loaded()'s idiom: default-construct sonarr_worker
    // (non-joinable) and only give it a live thread if the spawn succeeds.
    // On failure it stays non-joinable — safe for both the RAII guard below
    // and the explicit join() further down — tv_ok stays false, and we
    // proceed with just the Radarr half; that is already the correct
    // "Sonarr didn't answer" path (see apply_library_pending's
    // r.tv_fetch_ok handling).
    std::thread sonarr_worker;
    try {
        sonarr_worker = std::thread([this, &tv_refs, &tv_ok]() {
            if (auto tv_lib = sonarr_.get_library_checked()) {
                for (const auto& srs : *tv_lib) {
                    if (srs.tmdb_id > 0) {
                        tv_refs.insert(MediaRef{MediaKind::Tv, srs.tmdb_id});
                    }
                }
                tv_ok.store(true);
            }
        });
    } catch (const std::system_error& e) {
        spdlog::warn("[BrowseScreen] failed to spawn Sonarr library worker "
                     "(proceeding with Radarr only): {}", e.what());
    }
    // RAII join, defensive rather than load-bearing: run_library_refresh has
    // no early return today, so the plain sonarr_worker.join() before publish
    // (below) already covers the only path this function takes. This guard's
    // job is to keep that guarantee true if a future edit adds one, without
    // anyone having to remember to join on every new exit. It does NOT make
    // an exception thrown out of the Radarr block below safe: the guard's
    // destructor runs during unwinding and joins sonarr_worker cleanly, but
    // the exception then keeps propagating and escapes run_library_refresh
    // itself — the entry function of a std::thread — which calls
    // std::terminate() regardless of whether the join already happened.
    struct SonarrJoin {
        std::thread& t;
        ~SonarrJoin() { if (t.joinable()) t.join(); }
    } sonarr_join{sonarr_worker};
    // Health-check Radarr so render() can surface a banner when the service
    // isn't answering. Radarr is still required for add-to-library / queue
    // actions even though Discovery is TMDB-powered.
    r.services_ok = radarr_.is_reachable();
    if (r.services_ok) {
        auto lib_checked = radarr_.get_library_checked();
        r.movie_fetch_ok = lib_checked.has_value();
        // Both ternary operands must be lvalues or this forces a full copy of
        // the library on every refresh (a prvalue std::vector<Movie>{} on the
        // false branch makes the whole expression a prvalue).
        static const std::vector<Movie> kEmptyLibrary;
        const auto& lib = lib_checked ? *lib_checked : kEmptyLibrary;
        // Build radarr_id → tmdb_id map for queue cross-reference below.
        std::unordered_map<int, int> radarr_to_tmdb;
        for (const auto& m : lib) {
            if (m.tmdb_id > 0) {
                r.movie_refs.insert(MediaRef{MediaKind::Movie, m.tmdb_id});
                radarr_to_tmdb[m.radarr_id] = m.tmdb_id;
            }
        }
        // Populate the downloading set from the current Radarr queue.
        for (const auto& qi : radarr_.get_queue()) {
            auto it = radarr_to_tmdb.find(qi.movie_id);
            if (it != radarr_to_tmdb.end()) {
                r.downloading_refs.insert(MediaRef{MediaKind::Movie, it->second});
            }
        }
        // Quality profiles change only when the operator reconfigures Radarr
        // (rare), so we fetch them once — fetch_quality is a render-thread
        // snapshot of !library_cached_, avoiding a cross-thread read.
        if (fetch_quality) {
            r.quality_profiles = radarr_.get_quality_profiles();
            r.quality_fetched = true;
        }
    }
    // tv_refs is written only by the worker and read only after this join, so
    // the plain (non-atomic) container needs no further synchronisation.
    // Guarded: sonarr_worker is non-joinable when the spawn above threw, and
    // std::thread::join() itself throws std::system_error on a non-joinable
    // thread — exactly the crash this whole block exists to avoid.
    //
    // KNOWN BOUNDED LATENCY (final-review Fix 6, judged not worth a
    // concurrency change): there is exactly ONE publish point for this
    // function's result — apply_library_pending() drains it as a single
    // PendingLibrary struct after THIS join, further below. Radarr typically
    // answers in ~200 ms, but if Sonarr is configured and unreachable (its
    // container stopped, VPN down, etc.), get_library_checked()'s 5 s
    // timeout with no retry (see the comment above sonarr_worker's spawn)
    // holds this join — and therefore Radarr's already-finished result too —
    // for up to 5 s. That means Movies mode's posters, IN LIBRARY hide, and
    // DOWNLOADING badges wait on Sonarr's timeout in that scenario, even
    // though nothing in Movies mode needs Sonarr's answer. A clean fix would
    // split the publish so each service's result lands independently, but
    // that touches the CAS single-flight guard (lib_refresh_in_flight_), the
    // generation/TTL bookkeeping, and the deferred For-You hook in
    // apply_library_pending() — reviewed as not worth the risk for this
    // branch. See "Sonarr stopped bounds Movies mode..." in
    // docs/superpowers/plans/2026-08-01-tv-browse.md's hardware checklist.
    if (sonarr_worker.joinable()) sonarr_worker.join();
    r.tv_refs = std::move(tv_refs);
    r.tv_fetch_ok = tv_ok.load();
    {
        std::lock_guard<std::mutex> lk(lib_pending_mtx_);
        lib_pending_ = std::move(r);
    }
    lib_result_ready_.store(true);
    lib_refresh_in_flight_.store(false);
}

void BrowseScreen::apply_library_pending() {
    if (!lib_result_ready_.load()) return;
    PendingLibrary r;
    {
        std::lock_guard<std::mutex> lk(lib_pending_mtx_);
        r = std::move(lib_pending_);
    }
    lib_result_ready_.store(false);

    services_ok_ = r.services_ok;
    lib_refresh_done_once_ = true;
    lib_fetch_ok_[static_cast<int>(MbMode::Movies)] =
        r.services_ok && r.movie_fetch_ok;
    lib_fetch_ok_[static_cast<int>(MbMode::Tv)] = r.tv_fetch_ok;
    spdlog::info("[BrowseScreen] library refresh applied: radarr_ok={}, "
                 "movies={} (ok={}), shows={} (ok={}), downloading={}",
                 services_ok_, r.movie_refs.size(), r.movie_fetch_ok,
                 r.tv_refs.size(), r.tv_fetch_ok, r.downloading_refs.size());
    // Replace per kind, never wholesale: when only one service answered, the
    // other's contribution must survive or its mode's in-library hide
    // silently stops working. Replacing (rather than clearing then filling)
    // means quick_add_focused() never reads a momentarily-empty set.
    bool refs_changed = false;
    // MOVIE GATE IS `r.services_ok` ALONE — unchanged from the shipped code.
    // Adding `&& r.movie_fetch_ok` here would RETAIN the previous sets when a
    // reachable Radarr's library GET fails, instead of replacing them with
    // empties. That is probably better behavior, but it is a change to a
    // shipped movie path, and this task is the TV task. Movie mode must
    // behave identically; if the retain-on-failure fix is wanted it gets its
    // own change and its own hardware check.
    if (r.services_ok) {
        replace_refs_of_kind(library_refs_, MediaKind::Movie, r.movie_refs);
        // Deliberate asymmetry for now: downloading_refs_ is still a full
        // replace, not kind-aware like library_refs_ above. Safe today
        // because only Radarr (movies) feeds this. Once Sonarr feeds TV
        // downloading refs, a movie-only refresh landing here will wipe
        // every TV DOWNLOADING badge — switch this to
        // replace_refs_of_kind(downloading_refs_, MediaKind::Movie,
        // r.downloading_refs) when that lands.
        downloading_refs_ = std::move(r.downloading_refs);
        refs_changed = true;
    }
    if (r.tv_fetch_ok) {
        replace_refs_of_kind(library_refs_, MediaKind::Tv, r.tv_refs);
        refs_changed = true;
    }
    if (r.services_ok && r.quality_fetched) {
        quality_profiles_ = std::move(r.quality_profiles);
        library_cached_   = true;
    }
    if (refs_changed) {
        // Retro-hide: pages can land before the (async) library refresh on a
        // cold entry, and a title can join the library mid-session — sweep the
        // visible grid and BOTH For You caches so owned titles disappear
        // instead of wearing a badge. The predicate is kind-aware, so a movie
        // is never hidden because a show shares its id. The refs stay in
        // loaded_refs_, so later append pages can't resurrect them.
        //
        // Intentional deviation from the shipped movie-only behavior: this
        // sweep used to be gated on `if (r.services_ok)` (Radarr answered).
        // Gating on `refs_changed` instead means it now ALSO runs when Radarr
        // is down but Sonarr answered — against the retained previous-visit
        // movie ref set (movie library_refs_ entries are untouched when
        // r.services_ok is false; see the MOVIE GATE comment above). This is
        // safe, not just tolerated: apply_pending() already drops in-library
        // hits at page-append time using this exact predicate
        // (library_refs_.count(media_ref_of(m)) > 0), so by the time this
        // sweep runs on a Radarr-down visit, movies_/foryou_ already contain
        // nothing it would remove — an extra no-op pass, not a behavior
        // change. Movie mode still behaves identically end to end.
        // MOVIES ONLY (product change, 2026-08-02): TV titles the owner
        // has are BADGED in the grids, not hidden. Movies hide because the
        // Library tab is their way back; TV has no Library listing until
        // 2c-3, so hiding an added show stranded it — unreachable from
        // every kiosk surface (charts hid it, For You excludes it, Library
        // and Queue are movie-only, and there is no TV search). The chart
        // poster, wearing its IN LIBRARY badge, IS the way back to the
        // series detail page. Revisit when 2c-3 lands the TV Library.
        auto owned = [this](const TmdbSearchHit& m) {
            return m.kind == MediaKind::Movie &&
                   library_refs_.count(media_ref_of(m)) > 0;
        };
        const size_t before = movies_.size();
        movies_.erase(std::remove_if(movies_.begin(), movies_.end(), owned),
                      movies_.end());
        for (auto& cache : foryou_) {
            cache.hits.erase(std::remove_if(cache.hits.begin(), cache.hits.end(), owned),
                             cache.hits.end());
        }
        if (movies_.size() != before) {
            if (grid_cursor_ >= static_cast<int>(movies_.size())) {
                grid_cursor_ = movies_.empty()
                    ? 0 : static_cast<int>(movies_.size()) - 1;
            }
            const int cursor_row = movies_.empty() ? 0 : grid_cursor_ / kGridCols;
            scroll_row_ = (cursor_row / 2) * 2;
            spdlog::info("[BrowseScreen] hid {} in-library title(s) from the grid",
                         before - movies_.size());
        }
    }
    // For You deferred-sample hook (spec 1c entry rule case b).
    if (foryou_waiting_for_library_ && category_ == Category::ForYou) {
        foryou_waiting_for_library_ = false;
        start_foryou_sample(/*background=*/false);
    }
}

void BrowseScreen::activate_foryou() {
    // Invalidate any in-flight page worker from a different tab (spec 1c
    // review fix, Critical): load_category's ForYou early-return runs
    // BEFORE the unconditional gen bump other categories get, and only the
    // Sample case below reaches start_foryou_sample's own bump — so without
    // this, a slow Popular/TopRated page worker still in flight when the
    // user tabs to For You would publish under a still-current gen and
    // apply_pending (which has no category check) would overwrite whatever
    // For You is showing with foreign posters, persistently, since none of
    // UseCache/WaitForLibrary/ServiceUnavailable/EmptyLibrary schedule
    // anything that would repaint over it. The Sample path's second bump
    // via start_foryou_sample is harmless — that job captures the
    // post-bump gen.
    tmdb_current_gen_.fetch_add(1);
    // Structural clear (spec 1c review fix, Important): the deferred hook in
    // apply_library_pending() only fires this flag's reset when
    // category_==ForYou at the moment a refresh happens to land, so a
    // tab-away between WaitForLibrary and the refresh landing leaves it
    // stuck true. Clearing here means every activation starts clean; the
    // WaitForLibrary case below re-sets it when it actually applies.
    foryou_waiting_for_library_ = false;
    foryou_failed_ = false;
    // Kind-aware decision: the EmptyLibrary check looks at the active mode's
    // seed pool, not the whole (mixed-kind) library_refs_ — a movie-only
    // library must not read as "non-empty" while browsing TV, and vice versa.
    const MediaKind kind = tv_mode() ? MediaKind::Tv : MediaKind::Movie;
    switch (decide_foryou_entry(!foryou().hits.empty(), lib_refresh_done_once_,
                                lib_fetch_ok_[static_cast<int>(mode())],
                                seed_pool(library_refs_, kind).empty())) {
        case ForYouEntry::UseCache:
            movies_ = foryou().hits;
            loading_ = false;
            more_available_ = false;   // no scroll-driven pages on For You
            fetching_more_ = false;
            // Stale-while-revalidate on ACTIVATION, not just on enter().
            // decide_foryou_entry short-circuits on the cache with no age
            // check, and the TTL was only ever evaluated in enter() — so a
            // MODE toggle (which reaches here via reload_for_category ->
            // load_category(ForYou) -> activate_foryou) would show a
            // days-old grid verbatim. The per-mode split makes that far
            // likelier, since the inactive mode's cache ages the whole time
            // the user browses the other one. Same SWR contract as enter():
            // show the cache, revalidate behind it, never clear, never
            // Loading.
            if (tmdb_grid_stale(foryou().loaded_at,
                                std::chrono::steady_clock::now())) {
                start_foryou_sample(/*background=*/true);
            }
            break;
        case ForYouEntry::WaitForLibrary:
            loading_ = true;
            foryou_waiting_for_library_ = true;
            refresh_library_async();   // CAS-guarded no-op when already in flight
            break;
        case ForYouEntry::ServiceUnavailable:
        case ForYouEntry::EmptyLibrary:
            loading_ = false;          // render() branches on the flags below
            break;
        case ForYouEntry::Sample:
            start_foryou_sample(/*background=*/false);
            break;
    }
}

void BrowseScreen::start_foryou_sample(bool background) {
    // Sample min(8, library size) seeds uniformly without replacement, from
    // the ACTIVE mode's kind only. TMDB's movie and TV id spaces OVERLAP
    // (e.g. id 1396 is both Breaking Bad and an unrelated film) and the
    // recommendation endpoints are kind-specific
    // (/movie/{id}/recommendations vs /tv/{id}/recommendations) — a TV ref
    // fed through the movie endpoint would silently return valid
    // recommendations for the wrong, unrelated movie, no error, no log line.
    // seed_pool() (browse_logic.h) is the single place that filters by kind.
    const MediaKind kind = tv_mode() ? MediaKind::Tv : MediaKind::Movie;
    std::vector<int> pool = seed_pool(library_refs_, kind);
    if (pool.empty()) { loading_ = false; return; }
    // Structural clear (spec 1c review fix, Important): covers the direct
    // shuffle/TTL callers (do_shuffle(), enter()'s SWR branch) that reach
    // this function without going through activate_foryou()'s own clear —
    // a sample kicking off here means any earlier "waiting on the library"
    // state is moot. Deferred until after the pool.empty() bail (final
    // review fix): clearing it unconditionally meant a shuffle that hit an
    // empty pool while WaitForLibrary was still pending erased the flag,
    // starving the deferred hook in apply_library_pending() that would
    // otherwise retry the sample once the library finishes loading —
    // leaving a dead-end "No movies" state until the next tab activation.
    foryou_waiting_for_library_ = false;
    // In-flight guard: a slow/dead egress can leave a prior sample's seed
    // workers still running when the user shuffles or re-enters again —
    // without this, each trigger stacks another full batch of workers onto
    // tmdb_workers_. The existing generation machinery already makes a
    // superseded job's results get silently discarded, so this is purely a
    // worker-stacking cap, not a correctness fix.
    if (foryou_job_ && foryou_job_->remaining.load(std::memory_order_acquire) > 0) {
        if (foryou_job_->mode == mode()) {
            // Same mode: a repeat trigger would only stack another batch of
            // workers onto a slow egress. Ignore, as before.
            spdlog::info("[BrowseScreen] For You sample already in flight; ignoring");
            return;
        }
        // DIFFERENT mode: the toggle already bumped the generation, so that
        // job's results are guaranteed to be discarded at drain time. Bailing
        // out here would be a dead end — this early return happens BEFORE
        // `loading_ = true`, and load_category has already cleared movies_, so
        // For You would sit on a blank "no shows" state for the ~12 s the old
        // sample takes to finish and then repaint nothing, until the user
        // tabbed away and back. Drop the stale job and start the replacement.
        spdlog::info("[BrowseScreen] For You: dropping in-flight {} sample for a {} one",
                     foryou_job_->mode == MbMode::Tv ? "tv" : "movies",
                     tv_mode() ? "tv" : "movies");
        foryou_job_.reset();   // its workers run on; the gen check discards them
    }
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::vector<int> seeds;
    const int want = std::min<int>(8, static_cast<int>(pool.size()));
    std::sample(pool.begin(), pool.end(), std::back_inserter(seeds), want, rng);

    if (!background) {
        movies_.clear();
        grid_cursor_ = 0;
        scroll_row_ = 0;
        loading_ = true;
    }
    more_available_ = false;
    fetching_more_ = false;
    foryou_failed_ = false;
    tmdb_current_gen_.fetch_add(1);
    auto job = std::make_shared<ForYouJob>();
    job->gen = tmdb_current_gen_.load();
    job->background = background;
    job->mode = mode();
    job->remaining.store(static_cast<int>(seeds.size()));
    foryou_job_ = job;
    spdlog::info("[BrowseScreen] For You sample: {} seeds (gen={}, background={})",
                 seeds.size(), job->gen, background);
    for (int seed : seeds) {
        tmdb_workers_.spawn([this, job, seed, kind]() {
            // Recommendations with the documented similar-fallback — the same
            // contract the playback overlay uses (spec 1c step 2). The
            // endpoint pair is kind-specific: a movie id and a show id can be
            // the same number, so calling the wrong family returns a plausible
            // grid of the wrong medium rather than an error.
            auto list = (kind == MediaKind::Tv)
                            ? tmdb_.get_tv_recommendations(seed, /*page=*/1)
                            : tmdb_.get_recommendations(seed, /*page=*/1);
            if (list.hits.empty()) {
                auto fb = (kind == MediaKind::Tv)
                              ? tmdb_.get_tv_similar(seed, /*page=*/1)
                              : tmdb_.get_similar(seed, /*page=*/1);
                if (fb.ok || !list.ok) list = std::move(fb);
            }
            {
                std::lock_guard<std::mutex> lk(job->mtx);
                job->results.push_back({list.ok, std::move(list.hits)});
            }
            job->remaining.fetch_sub(1, std::memory_order_acq_rel);
        });
    }
}

void BrowseScreen::apply_foryou_pending() {
    if (!foryou_job_) return;
    if (foryou_job_->remaining.load(std::memory_order_acquire) != 0) return;
    auto job = std::move(foryou_job_);
    if (job->gen != tmdb_current_gen_.load()) return;  // preempted — discard
    std::vector<SeedResult> results;
    {
        std::lock_guard<std::mutex> lk(job->mtx);
        results = std::move(job->results);
    }
    int ok_seeds = 0;
    std::vector<std::vector<TmdbSearchHit>> per_seed;
    per_seed.reserve(results.size());
    for (auto& sr : results) {
        if (sr.ok) ++ok_seeds;
        per_seed.push_back(std::move(sr.hits));
    }
    if (ok_seeds == 0) {
        // Timestamp rule (spec 1c): all seeds failed → timestamp untouched so
        // the next enter() retries. Explicit load shows the error state;
        // background TTL keeps the old grid.
        spdlog::warn("[BrowseScreen] For You: all {} seeds failed", results.size());
        if (!job->background) {
            loading_ = false;
            foryou_failed_ = true;
        }
        return;
    }
    // The job knows which mode it was started under — a MODE toggle mid-flight
    // must not file TV results into the movie cache. (The generation check
    // above already discards a preempted job; this covers the case where the
    // toggle's own reload left this job's generation current.)
    ForYouCache& target = foryou_[static_cast<int>(job->mode)];
    target.hits = merge_recommendations(per_seed, library_refs_);
    target.loaded_at = std::chrono::steady_clock::now();
    spdlog::info("[BrowseScreen] For You merged: {} titles from {} ok seed(s), mode={}",
                 target.hits.size(), ok_seeds,
                 job->mode == MbMode::Tv ? "tv" : "movies");
    if (category_ == Category::ForYou && job->mode == mode()) {
        movies_ = target.hits;
        grid_cursor_ = 0;
        scroll_row_ = 0;
        loading_ = false;
        more_available_ = false;
        fetching_more_ = false;
    }
}

void BrowseScreen::quick_add_focused() {
    // *** TRAP FOR A FUTURE CALLER, DO NOT REACTIVATE AS-IS. ***
    // This function is KIND-BLIND: the in-library check below correctly
    // uses media_ref_of(hit)/library_refs_.count(...) (kind-aware, since
    // Phase 2c-1), but the add itself is hardcoded to
    // radarr_.add_movie(hit.tmdb_id, ...). Since Phase 2c-1, movies_ can
    // hold TV rows (TV mode's Popular/TopRated/For You all populate it),
    // and movie and TV TMDB id spaces OVERLAP COMPLETELY — e.g. id 1396 is
    // Breaking Bad (a show) AND an entirely unrelated film. A caller who
    // reactivates this while focused on a TV poster would silently add the
    // WRONG, UNRELATED MOVIE to Radarr — no error, no mismatch detectable
    // from the call site. Before wiring this back up: branch on
    // media_ref_of(hit).kind and route TV through a Sonarr add_series() call
    // (NOT implemented here — out of scope per the final whole-branch
    // review), never let a TV hit reach radarr_.add_movie(). Same hazard
    // class as the deferred SonarrMockClient traps documented in
    // sonarr_mock.cpp and tests/media_browser/test_sonarr_client.cpp.
    //
    // Only meaningful when a poster is focused.
    if (focus_ != Focus::PosterGrid) return;
    if (movies_.empty()) return;
    if (grid_cursor_ < 0 ||
        grid_cursor_ >= static_cast<int>(movies_.size())) return;
    const auto& hit = movies_[grid_cursor_];
    if (hit.tmdb_id <= 0) return;

    // Already in library? Short-circuit with a toast.
    if (library_refs_.count(media_ref_of(hit)) > 0) {
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

    // KIND-BLIND — see the hazard comment at the top of this function
    // before reactivating this caller. hit may be a TV row.
    bool ok = radarr_.add_movie(hit.tmdb_id, qp, /*monitor=*/true);
    if (!ok) {
        ::ui::Toast::show("Add failed — see Radarr logs");
        return;
    }
    library_refs_.insert(media_ref_of(hit));
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
        case Category::ForYou:     return "For You";
        case Category::Search:     return "Search";
        case Category::Library:    return "Library";
        case Category::Queue:      return "Queue";
        case Category::Settings:   return "Settings";
    }
    return "";
}

void BrowseScreen::ensure_genres_loaded() {
    // Kick-off only — the fetch itself runs on a worker (see the note on
    // genres_fetching_ in the header). The Filter picker renders an
    // empty genre list until update() drains the result, which beats the
    // render thread stalling into the systemd watchdog when the egress
    // is down.
    if (genres_loaded_ || genres_fetching_.load(std::memory_order_acquire)) {
        return;
    }
    genres_fetching_.store(true, std::memory_order_release);
    try {
        tmdb_workers_.spawn([this]() {
            auto g = tmdb_.get_genres();
            {
                std::lock_guard<std::mutex> lk(genres_mtx_);
                pending_genres_ = std::move(g);
            }
            genres_ready_.store(true, std::memory_order_release);
        });
    } catch (const std::system_error&) {
        genres_fetching_.store(false, std::memory_order_release);
    }
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
    loaded_refs_.clear();
    page_window_base_ = 1;
    shuffle_retry_base1_ = false;
    window_is_discover_ = false;
    if (is_nav_chip(cat)) {
        loading_ = false;
        return;
    }
    if (cat == Category::ForYou) {
        // For You is exempt from the unconditional tab-activation refetch
        // (spec 1c): activation re-renders the cached merged list when one
        // exists. load_category was already called with cleared grid state.
        activate_foryou();
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

void BrowseScreen::load_shuffle(Category cat, int base_page) {
    // Mirror load_category's synchronous reset (spec 1b: the reset cannot be
    // left to apply_pending — its replace branch only fires for the window
    // base page, and spawn_page_worker alone does not bump the generation).
    movies_.clear();
    grid_cursor_ = 0;
    scroll_row_ = 0;
    more_available_ = true;
    fetching_more_ = false;
    loaded_refs_.clear();
    shuffle_retry_base1_ = false;
    page_window_base_ = base_page;
    window_is_discover_ = false;
    loading_ = true;
    tmdb_current_gen_.fetch_add(1);
    spdlog::info("[BrowseScreen] load_shuffle: {} base={} (gen={})",
                 label_for_category(cat), base_page, tmdb_current_gen_.load());
    spawn_page_worker(cat, base_page);
}

void BrowseScreen::load_shuffle_discover(int base_page) {
    movies_.clear();
    grid_cursor_ = 0;
    scroll_row_ = 0;
    more_available_ = true;
    fetching_more_ = false;
    loaded_refs_.clear();
    shuffle_retry_base1_ = false;
    page_window_base_ = base_page;
    window_is_discover_ = true;
    loading_ = true;
    tmdb_current_gen_.fetch_add(1);
    spdlog::info("[BrowseScreen] load_shuffle_discover: base={} (gen={})",
                 base_page, tmdb_current_gen_.load());
    spawn_page_worker(Category::Filter, base_page);
}

bool BrowseScreen::active_chart_filters_active() const {
    if (category_ != Category::Popular && category_ != Category::TopRated) return false;
    const FilterTabKind tab = (category_ == Category::Popular)
                              ? FilterTabKind::Popular : FilterTabKind::TopRated;
    return any_filter_active(read_filter_state(state_.display_settings, mode(), tab), tab);
}

void BrowseScreen::revalidate_active_chart() {
    // In-flight guard: an in-flight fetch already owns the pagination
    // pipeline (fetching_more_, next_page_to_fetch_, page_window_base_) —
    // stomping it here would race the running worker. The TTL simply
    // retries on the box's next enter() rather than forcing a second
    // concurrent fetch.
    if (fetching_more_) {
        spdlog::info("[BrowseScreen] TTL revalidate: fetch already in flight; skipping");
        return;
    }
    // Spec 1a stale-while-revalidate: bump the generation NOW so in-flight
    // pages from the old (possibly shuffled) window are dropped rather than
    // appended after the swap, reset the window base to 1, but defer every
    // visible reset (grid, cursor, Loading) to the swap in apply_pending.
    tmdb_current_gen_.fetch_add(1);
    page_window_base_ = 1;
    shuffle_retry_base1_ = false;
    fetching_more_ = true;
    next_page_to_fetch_ = 2;
    const uint64_t gen = tmdb_current_gen_.load();
    spdlog::info("[BrowseScreen] TTL revalidate: {} (gen={})",
                 label_for_category(category_), gen);
    // A box retrying a failed initial load has an empty grid — without this,
    // it would sit on the silent "No movies in this category" empty state
    // for the entire ~76s TMDB retry ladder instead of showing "Loading...".
    if (movies_.empty()) {
        loading_ = true;
    }
    const FilterTabKind rev_tab = (category_ == Category::Popular)
                                  ? FilterTabKind::Popular : FilterTabKind::TopRated;
    const MbMode m = mode();
    if (active_chart_filters_active()) {
        window_is_discover_ = true;
        const FilterState fs = read_filter_state(state_.display_settings, m, rev_tab);
        if (m == MbMode::Tv) {
            current_tv_filter_ = build_tv_discover_filter(fs, rev_tab);
            tmdb_workers_.spawn([this, gen, filter = current_tv_filter_]() {
                run_reload_tv_filter_page(gen, filter, /*page=*/1, /*is_revalidate=*/true);
            });
        } else {
            current_filter_ = build_discover_filter(fs, rev_tab);
            tmdb_workers_.spawn([this, gen, filter = current_filter_]() {
                run_reload_filter_page(gen, filter, /*page=*/1, /*is_revalidate=*/true);
            });
        }
    } else {
        window_is_discover_ = false;
        tmdb_workers_.spawn([this, gen, cat = category_, m]() {
            run_load_page(gen, cat, m, /*page=*/1, /*is_revalidate=*/true);
        });
    }
}

void BrowseScreen::spawn_page_worker(Category cat, int page) {
    fetching_more_ = true;
    next_page_to_fetch_ = page + 1;
    const uint64_t gen = tmdb_current_gen_.load();
    const MbMode m = mode();
    if (cat == Category::Filter) {
        // Filter goes through discover(); branch on mode here so the window's
        // follow-up pages hit the same endpoint the base page did. Pass the
        // filter by value to avoid racing the user mutating it while we fetch.
        if (m == MbMode::Tv) {
            tmdb_workers_.spawn([this, gen, filter = current_tv_filter_, page]() {
                run_reload_tv_filter_page(gen, filter, page);
            });
        } else {
            tmdb_workers_.spawn([this, gen, filter = current_filter_, page]() {
                run_reload_filter_page(gen, filter, page);
            });
        }
    } else {
        tmdb_workers_.spawn([this, gen, cat, m, page]() {
            run_load_page(gen, cat, m, page);
        });
    }
}

void BrowseScreen::run_load_page(uint64_t gen, Category cat, MbMode mode_for_page, int page, bool is_revalidate) {
    TmdbList list;
    if (mode_for_page == MbMode::Tv) {
        // Only the two chart tabs exist in TV mode. NowPlaying/Upcoming have
        // no TV analogue and are not in the visible strip anyway.
        switch (cat) {
            case Category::Popular:  list = tmdb_.get_tv_popular(page);   break;
            case Category::TopRated: list = tmdb_.get_tv_top_rated(page); break;
            default: break;
        }
    } else {
        switch (cat) {
            case Category::Popular:    list = tmdb_.get_popular(page);     break;
            case Category::NowPlaying: list = tmdb_.get_now_playing(page); break;
            case Category::TopRated:   list = tmdb_.get_top_rated(page);   break;
            case Category::Upcoming:   list = tmdb_.get_upcoming(page);    break;
            default: break;
        }
    }
    const bool no_more = list.hits.size() < 5;
    if (gen != tmdb_current_gen_.load()) {
        spdlog::info("[BrowseScreen] page={} gen={} stale at publish (current={}); discarding",
                     page, gen, tmdb_current_gen_.load());
        return;
    }
    {
        std::lock_guard<std::mutex> lk(tmdb_result_mtx_);
        if (gen != tmdb_current_gen_.load()) return;
        PendingPage pp;
        pp.movies = std::move(list.hits);
        pp.page = page;
        pp.no_more = no_more;
        pp.ok = list.ok;
        pp.total_pages = list.total_pages;
        pp.is_revalidate = is_revalidate;
        pp.gen = gen;
        tmdb_pending_pages_.push_back(std::move(pp));
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
    loaded_refs_.clear();
    page_window_base_ = 1;
    shuffle_retry_base1_ = false;
    window_is_discover_ = true;
    loading_ = true;
    tmdb_current_gen_.fetch_add(1);
    if (tv_mode()) {
        spdlog::info(
            "[BrowseScreen] discover_tv (async, gen={}): genre_ids.size={} "
            "first_air_gte={} first_air_lte={} sort_by={}",
            tmdb_current_gen_.load(),
            current_tv_filter_.genre_ids.size(),
            current_tv_filter_.first_air_date_year_gte.value_or(-1),
            current_tv_filter_.first_air_date_year_lte.value_or(-1),
            current_tv_filter_.sort_by);
    } else {
        spdlog::info(
            "[BrowseScreen] discover (async, gen={}): genre_ids.size={} "
            "year_gte={} year_lte={} sort_by={}",
            tmdb_current_gen_.load(),
            current_filter_.genre_ids.size(),
            current_filter_.primary_release_year_gte.value_or(-1),
            current_filter_.primary_release_year_lte.value_or(-1),
            current_filter_.sort_by);
    }
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
        FilterState fs = read_filter_state(state_.display_settings, mode(), tab);
        if (any_filter_active(fs, tab)) {
            // Filter active → route through /discover/movie or /discover/tv.
            if (tv_mode()) current_tv_filter_ = build_tv_discover_filter(fs, tab);
            else           current_filter_    = build_discover_filter(fs, tab);
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

void BrowseScreen::persist_filter_state(MbMode mode_for_write, FilterTabKind tab,
                                        const FilterState& fs) {
    // KEEP this guard. write_filter_state is itself a no-op for ForYou, but
    // the guard suppresses the SAVE as well — and save_settings is a
    // synchronous serialize + tmp-write + fs::rename on the render thread.
    // SHUFFLE is a repeat-press action that routes here on every press, so
    // dropping the guard would cost one SD write per press for a payload that
    // provably did not change.
    if (tab == FilterTabKind::ForYou) return;
    write_filter_state(state_.display_settings, mode_for_write, tab, fs);
    ::app::SettingsPersistence::save_settings(state_);
}

void BrowseScreen::apply_mode_change() {
    // The library cache holds both kinds (Radarr for movies, Sonarr for TV),
    // but the OTHER kind's set may be stale or never fetched — re-kick so the
    // in-library hide is right for what is about to be drawn. CAS-guarded: a
    // no-op when one is in flight.
    refresh_library_async();
    // reload_for_category(), NOT load_category(). load_category is the
    // FILTER-BLIND entry point: it sets window_is_discover_ = false and routes
    // to the curated /tv/popular or /popular endpoint regardless of what is
    // persisted. Toggling into a mode that HAS active filters would then show
    // the unfiltered chart while the overlay row and settings.json both say
    // GENRE=Drama — and nothing self-corrects, because the overlay's commit
    // only fires when working_ != opened_ and the toggle re-syncs both. Worse,
    // active_chart_filters_active() would read true against a
    // window_is_discover_ == false grid, so the next SHUFFLE or TTL
    // revalidate would silently swap the same tab to a different result set.
    // reload_for_category is a strict superset — it falls through to
    // load_category(category_) for For You and for the no-filter case.
    reload_for_category();
}

void BrowseScreen::do_shuffle() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    if (category_ == Category::Popular || category_ == Category::TopRated) {
        if (active_chart_filters_active()) {
            const FilterTabKind tab = (category_ == Category::Popular)
                                      ? FilterTabKind::Popular : FilterTabKind::TopRated;
            const FilterState fs = read_filter_state(state_.display_settings, mode(), tab);
            std::string sig;
            if (tv_mode()) {
                current_tv_filter_ = build_tv_discover_filter(fs, tab);
                sig = TmdbClient::build_tv_discover_url("", current_tv_filter_, 1);
            } else {
                current_filter_ = build_discover_filter(fs, tab);
                sig = TmdbClient::build_discover_url("", current_filter_, 1);
            }
            const auto it = discover_total_pages_.find(sig);
            const int max_base =
                discover_max_base(it == discover_total_pages_.end() ? 0 : it->second,
                                  kMaxLoadedPages);
            load_shuffle_discover(
                pick_shuffle_base(page_window_base_, max_base, rng()));
        } else {
            const int max_base = (category_ == Category::Popular)
                                 ? kShuffleMaxBasePopular : kShuffleMaxBaseTopRated;
            load_shuffle(category_,
                         pick_shuffle_base(page_window_base_, max_base, rng()));
        }
    }
    if (category_ == Category::ForYou) {
        start_foryou_sample(/*background=*/false);  // fresh seed draw, clear + Loading
    }
}

void BrowseScreen::run_reload_filter_page(uint64_t gen, DiscoverFilter filter,
                                          int page, bool is_revalidate) {
    auto list = tmdb_.discover(filter, page);
    const bool no_more = list.hits.size() < 5;
    if (gen != tmdb_current_gen_.load()) {
        spdlog::info("[BrowseScreen] discover page={} gen={} stale; discarding",
                     page, gen);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(tmdb_result_mtx_);
        if (gen != tmdb_current_gen_.load()) return;
        PendingPage pp;
        pp.movies = std::move(list.hits);
        pp.page = page;
        pp.no_more = no_more;
        pp.ok = list.ok;
        pp.total_pages = list.total_pages;
        pp.is_revalidate = is_revalidate;
        pp.discover_sig = TmdbClient::build_discover_url("", filter, 1);
        pp.gen = gen;
        tmdb_pending_pages_.push_back(std::move(pp));
    }
    tmdb_result_ready_.store(true);
}

void BrowseScreen::run_reload_tv_filter_page(uint64_t gen, TvDiscoverFilter filter,
                                             int page, bool is_revalidate) {
    auto list = tmdb_.discover_tv(filter, page);
    const bool no_more = list.hits.size() < 5;
    if (gen != tmdb_current_gen_.load()) {
        spdlog::info("[BrowseScreen] discover_tv page={} gen={} stale; discarding",
                     page, gen);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(tmdb_result_mtx_);
        if (gen != tmdb_current_gen_.load()) return;
        PendingPage pp;
        pp.movies = std::move(list.hits);
        pp.page = page;
        pp.no_more = no_more;
        pp.ok = list.ok;
        pp.total_pages = list.total_pages;
        pp.is_revalidate = is_revalidate;
        // The TV signature can never alias a movie one — build_tv_discover_url
        // emits /discover/tv, build_discover_url emits /discover/movie.
        pp.discover_sig = TmdbClient::build_tv_discover_url("", filter, 1);
        pp.gen = gen;
        tmdb_pending_pages_.push_back(std::move(pp));
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
    // Set for any drained entry whose gen still matches at DRAIN time (not
    // just at publish time — see the gen field comment in PendingPage).
    // Gates the post-loop loading_/fetching_more_ reset: if every drained
    // entry turned out to be from a superseded generation, the current-gen
    // fetch is still in flight and owns those flags — leave them alone.
    bool applied_any = false;
    for (auto& pp : drained) {
        // total_pages cache for the discover shuffle clamp (spec 1b) — keyed
        // by filter signature, learned from any discover page, even a
        // superseded one — total_pages is a property of the query (the
        // filter that produced the response), not the generation, so it's
        // safe to learn from a page whose gen check below sends it to the
        // stale-skip continue.
        if (!pp.discover_sig.empty() && pp.total_pages > 0) {
            discover_total_pages_[pp.discover_sig] = pp.total_pages;
        }
        if (pp.gen != tmdb_current_gen_.load()) {
            spdlog::info("[BrowseScreen] page={} gen={} stale at drain (current={}); discarding",
                         pp.page, pp.gen, tmdb_current_gen_.load());
            continue;
        }
        applied_any = true;
        // Background revalidate that failed or came back empty: skip the swap
        // entirely — the old grid survives (spec 1a). Freeze pagination for
        // the stale grid: its window state no longer matches its content.
        if (pp.is_revalidate && (!pp.ok || pp.movies.empty())) {
            spdlog::warn("[BrowseScreen] TTL revalidate failed (ok={}, hits={}); keeping stale grid",
                         pp.ok, pp.movies.size());
            fetching_more_ = false;
            more_available_ = false;
            continue;
        }
        // A shuffled base page that is genuinely empty (ok, 0 hits — possible
        // on narrow /discover filters): fall back to page 1 (spec 1b).
        if (!pp.is_revalidate && pp.page == page_window_base_ &&
            page_window_base_ != 1 && pp.ok && pp.movies.empty()) {
            spdlog::info("[BrowseScreen] shuffled base {} empty; falling back to page 1",
                         page_window_base_);
            shuffle_retry_base1_ = true;
            continue;
        }
        size_t added = 0, dups = 0, owned = 0;
        // MOVIE titles already in (or being fetched into) the owner's
        // library are hidden from the chart grids — the owner asked for
        // discovery surfaces to show only what they DON'T have, and the
        // Library tab is the way back to a hidden movie. TV titles the
        // owner has are NOT hidden: they render with the IN LIBRARY badge,
        // because until 2c-3 lands the TV Library listing the badged chart
        // poster is the ONLY route back to a series' detail page (add next
        // season / remove / download badges). The For You merge still
        // excludes the library for BOTH kinds at merge time — a
        // recommendation row shouldn't recommend what you have; the badge
        // belongs on the charts. Hidden ids still enter loaded_refs_ so a
        // later append page can't resurrect a hidden movie.
        auto owned_by_library = [this](const TmdbSearchHit& m) {
            return m.kind == MediaKind::Movie &&
                   library_refs_.count(media_ref_of(m)) > 0;
        };
        if (pp.page == page_window_base_) {
            // Window-base page — canonical replacement (was hardcoded page 1).
            movies_.clear();
            loaded_refs_.clear();
            movies_.reserve(pp.movies.size());
            for (auto& m : pp.movies) {
                if (!loaded_refs_.insert(media_ref_of(m)).second) {
                    ++dups;
                } else if (owned_by_library(m)) {
                    ++owned;
                } else {
                    movies_.push_back(std::move(m));
                    ++added;
                }
            }
            grid_cursor_ = 0;
            scroll_row_ = 0;
            // A successful base-page swap (normal load, shuffle, or a
            // revalidate that succeeds after an earlier one failed) must
            // unfreeze pagination — the immediate no_more check below can
            // still legitimately re-clear it for a genuinely short list.
            more_available_ = true;
            // Timestamp rule (spec 1a/1b): the grid is fresh whenever its
            // base page lands ok and non-empty — normal load, shuffle, or
            // revalidate alike.
            if (pp.ok && !movies_.empty()) {
                chart_loaded_at_ = std::chrono::steady_clock::now();
            } else if (!pp.ok) {
                // A failed base page (network failure during a normal load
                // or shuffle) must not leave the PREVIOUS grid's timestamp
                // in place — that would read as fresh and block any retry
                // for up to the full TTL. Clear it so the next enter() sees
                // a stale grid and revalidates.
                chart_loaded_at_ = std::chrono::steady_clock::time_point{};
            }
        } else {
            movies_.reserve(movies_.size() + pp.movies.size());
            for (auto& m : pp.movies) {
                if (!loaded_refs_.insert(media_ref_of(m)).second) {
                    ++dups;
                } else if (owned_by_library(m)) {
                    ++owned;
                } else {
                    movies_.push_back(std::move(m));
                    ++added;
                }
            }
        }
        if (pp.no_more) more_available_ = false;
        spdlog::info("[BrowseScreen] applied page {}: +{} movies, {} dup(s), "
                     "{} in-library hidden (total {})",
                     pp.page, added, dups, owned, movies_.size());
    }
    if (applied_any) {
        loading_ = false;
        fetching_more_ = false;
    }
    // Deferred outside the drain loop so we don't mutate pagination state
    // mid-iteration: rerun the shuffle as a plain page-1 load.
    if (shuffle_retry_base1_) {
        shuffle_retry_base1_ = false;
        if (active_chart_filters_active()) load_shuffle_discover(1);
        else load_shuffle(category_, 1);
    }
}

void BrowseScreen::maybe_load_more_pages() {
    // Don't fetch while another fetch is in flight, or after we've
    // confirmed end-of-list, or for nav chips, or while still in the
    // initial loading state. Hard-cap at kMaxLoadedPages.
    if (fetching_more_ || loading_) return;
    if (!more_available_) return;
    if (is_nav_chip(category_)) return;
    if (category_ == Category::ForYou) return;
    // Base-relative window (spec 1b): load [base, base+kMaxLoadedPages-1].
    if (next_page_to_fetch_ > window_last_page(page_window_base_, kMaxLoadedPages)) return;

    const int rows_loaded = movies_.empty()
        ? 0
        : (static_cast<int>(movies_.size()) + kGridCols - 1) / kGridCols;
    const int cursor_row = grid_cursor_ / kGridCols;
    const bool prefetch_second = (next_page_to_fetch_ == page_window_base_ + 1);
    const bool near_end = (rows_loaded > 0) && (cursor_row >= rows_loaded - 1);
    if (!prefetch_second && !near_end) return;

    spdlog::info("[BrowseScreen] auto-fetching page {} ({})",
                 next_page_to_fetch_,
                 prefetch_second ? "second-page prefetch" : "scroll-driven");
    // Route follow-up pages through the same endpoint the window base used
    // (spec 1b fix): scroll-driven pages must not silently fall back to the
    // curated endpoint for a filtered/discover window.
    spawn_page_worker(window_is_discover_ ? Category::Filter : category_,
                       next_page_to_fetch_);
}

void BrowseScreen::update() {
    filter_overlay_.tick();
    // Join+drop finished page/genre workers (instant — see worker_pool.h).
    tmdb_workers_.reap();
    apply_pending();             // TMDB page workers → movies_
    apply_foryou_pending();
    apply_library_pending();     // Radarr library/queue worker → id-sets
    // Genre worker → genres_ (single-flight; see ensure_genres_loaded).
    if (genres_ready_.exchange(false, std::memory_order_acq_rel)) {
        {
            std::lock_guard<std::mutex> lk(genres_mtx_);
            genres_ = std::move(pending_genres_);
        }
        genres_loaded_ = true;
        genres_fetching_.store(false, std::memory_order_release);
        spdlog::info("[BrowseScreen] genres loaded: {} entries",
                     genres_.size());
    }
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
    tmdb_workers_.join_all();
    // Join the Radarr library-refresh worker too, so a thread mid-CURL can't
    // outlive the screen and publish into freed members. Bounded by
    // RadarrClient's per-attempt curl timeout.
    if (lib_refresh_worker_.joinable()) lib_refresh_worker_.join();
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
    // Marquee tab strip — content tabs left, transition tabs right. Display
    // order, left-to-right: Popular · Top Rated · For You · Search ·
    // Library · Queue · Settings (kVisibleTabs, class member — shared with
    // render()). BTN1 (PREV, yellow) walks left; BTN3 (NEXT, green) walks
    // right; movement stops at the ends (no wrap). Library, Search, Queue,
    // and Settings are transition-only — selecting them returns the
    // corresponding Screen enum value to the dispatcher in main.cpp, which
    // swaps the active screen. Now Playing was removed in v1.6.x — it
    // overlapped almost completely with Popular on TMDB's data, so
    // collapsing them removes a confusing-looking duplicate.

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
        // it consumes ROTATE_VERTICAL / SELECT. BTN4 toggles open/close.
        // BTN2 (PLAY_PAUSE) is now owned globally by the exit modal in
        // main.cpp and never reaches this point.
        // ================================================================
        if (filter_overlay_.is_visible()) {
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

        // BTN2 (PLAY_PAUSE, red) — intercepted globally by the exit modal
        // in main.cpp. It never reaches here; no per-screen handler needed.

        // BTN4 (SETTINGS_MENU, black) — toggles the filter overlay on
        // Popular + TopRated tabs (full filter panel) and For You (SHUFFLE-
        // only row). On other tabs it remains a no-op (long-press still
        // exits MB → MainMenu via the dispatcher).
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            if (filter_overlay_.is_visible()) {
                filter_overlay_.on_btn4_close();
            } else if (category_ == Category::Popular ||
                       category_ == Category::TopRated ||
                       category_ == Category::ForYou) {
                FilterTabKind tk =
                    (category_ == Category::Popular)  ? FilterTabKind::Popular :
                    (category_ == Category::TopRated) ? FilterTabKind::TopRated :
                                                        FilterTabKind::ForYou;
                filter_overlay_.open(tk, mode(),
                                     read_filter_state(state_.display_settings,
                                                       mode(), tk));
                filter_overlay_.set_on_commit(
                    [this](const FilterState& fs, FilterTabKind tk2) {
                        this->persist_filter_state(this->mode(), tk2, fs);
                        this->reload_for_category();
                    });
                filter_overlay_.set_on_shuffle(
                    [this](const FilterState& fs, FilterTabKind tk2) {
                        this->persist_filter_state(this->mode(), tk2, fs);
                        this->do_shuffle();
                    });
                filter_overlay_.set_on_mode_toggle(
                    [this](MbMode new_mode, FilterTabKind tk2,
                           const FilterState& staged) {
                        // One tested pure transaction (mb_filter_state's
                        // apply_mode_toggle: staged edits -> the OUTGOING
                        // mode's slot, mb_mode -> incoming, return the
                        // incoming mode's persisted state), then exactly ONE
                        // settings.json write. save_settings is a synchronous
                        // serialize + tmp-write + rename on the render thread;
                        // the earlier two-call shape cost an extra SD write
                        // per press for no benefit.
                        const MbMode outgoing = this->mode();
                        const FilterState restaged = apply_mode_toggle(
                            state_.display_settings, outgoing, new_mode,
                            tk2, staged);
                        ::app::SettingsPersistence::save_settings(state_);
                        this->apply_mode_change();
                        return restaged;
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
                const auto& hit = movies_[grid_cursor_];
                // Movie and TV TMDB id spaces OVERLAP COMPLETELY (1396 is
                // Breaking Bad AND an unrelated film), so the id alone must
                // never choose the destination. The KIND picks it, and the
                // choice is carried in the returned Screen value — the
                // dispatcher needs no second accessor to re-derive it.
                selected_tmdb_id_ = hit.tmdb_id;
                return hit.kind == MediaKind::Tv ? Screen::SeriesDetail
                                                 : Screen::Detail;
            }
        }
    }

    // Page-based scroll: scroll_row_ is always the first row of the current
    // page (a multiple of kPageRows=2). It snaps when cursor crosses a page
    // boundary, not on every row step.
    if (!movies_.empty()) {
        const int cursor_row = grid_cursor_ / kGridCols;
        scroll_row_ = (cursor_row / 2) * 2;
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

    // --- Header: mode-aware title (left) + tab strip (right) ---
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
    // The mode marker lives here, not in a chip label — see
    // marquee_title_for_mode's comment for the measured reason.
    const int content_top = chrome::draw_screen_header(
        r, screen_w, marquee_title_for_mode(tv_mode()), tabs, /*focused_tab=*/-1);

    if (category_ == Category::Library || category_ == Category::Search) {
        // Transition-only tabs — handle_input returns Screen::Library/Search
        // on activation, so render() should never see them. The overlay is
        // drawn even on this belt-and-suspenders path: it is a modal that
        // keeps consuming input while Open, and a return that skips its draw
        // leaves an invisible panel eating the user's presses.
        filter_overlay_.render(r, screen_w, screen_h);
        return;
    }

    const bool filter_available = (category_ == Category::Popular ||
                                   category_ == Category::TopRated);
    const bool shuffle_only = (category_ == Category::ForYou);
    const MediaKind kind = tv_mode() ? MediaKind::Tv : MediaKind::Movie;
    const bool lib_ok = lib_fetch_ok_[static_cast<int>(mode())];

    // ---- State resolution -------------------------------------------------
    // Every "nothing to draw" condition becomes a message here rather than an
    // early return. THIS FUNCTION HAS EXACTLY ONE EXIT BELOW, and it always
    // draws the footer and then the filter overlay. That is structural, not
    // stylistic: the overlay is a modal that swallows ROTATE/SELECT/PREV/NEXT
    // for as long as it is Open, so a return that skips its draw leaves an
    // invisible panel consuming input. Pressing MODE clears movies_ and sets
    // loading_, which used to take the "Loading..." early return and make the
    // panel disappear for the entire 6-12 s reload; a For You activation that
    // lands on EmptyLibrary made it disappear permanently. Do not reintroduce
    // an early return here — add a BrowseGridState instead.
    BrowseStateInputs si;
    si.is_foryou              = (category_ == Category::ForYou);
    si.grid_empty             = movies_.empty();
    si.loading                = loading_;
    si.lib_refresh_done_once  = lib_refresh_done_once_;
    si.lib_fetch_ok           = lib_ok;
    // Allocation-free equivalent of seed_pool(library_refs_, kind).empty():
    // seed_pool reserves + fills a std::vector<int> over BOTH kinds to
    // answer a boolean, which is a per-frame malloc/free on the Pi 4B's
    // tight 1.5 GB envelope. any_of walks library_refs_ without allocating;
    // seeds_empty is true when NO ref of `kind` exists, i.e. !any_of.
    si.seeds_empty            = !std::any_of(
        library_refs_.begin(), library_refs_.end(),
        [kind](const MediaRef& ref) { return ref.kind == kind; });
    si.foryou_failed          = foryou_failed_;
    si.has_api_key            = tmdb_.has_api_key();
    const BrowseGridState grid_state = decide_browse_grid_state(si);

    // Message text itself is decided by the pure, Mac-testable
    // browse_grid_state_message() (browse_logic.h) — this switch only
    // assigns the COLOR per state, since ::ui::Color/Theme can't live in
    // that Renderer-free header.
    const char* state_msg =
        browse_grid_state_message(grid_state, tv_mode(), sonarr_configured_);
    ::ui::Color state_color = th.dim;
    switch (grid_state) {
        case BrowseGridState::Grid:
            break;
        case BrowseGridState::Loading:
            break;
        case BrowseGridState::LibraryUnavailable:
            // Blocking, because For You genuinely REQUIRES its library — the
            // seed sample has no other source. The chart tabs are TMDB-sourced
            // and get the non-blocking line below instead.
            state_color = th.highlight2;
            break;
        case BrowseGridState::RecommendationsFailed:
            state_color = th.highlight2;
            break;
        case BrowseGridState::EmptyLibrary:
            break;
        case BrowseGridState::NoApiKey:
            state_color = th.highlight2;
            break;
        case BrowseGridState::EmptyCategory:
            break;
    }

    // Non-blocking service line. Popular and Top Rated need only TMDB; a dead
    // Radarr (or Sonarr) degrades the in-library hide but must not blank a
    // perfectly good grid — which is exactly what the old screen-wide
    // `if (!services_ok_)` return did, on Radarr's ping alone, even in TV
    // mode where nothing on the path touches Radarr. This also gives the
    // Sonarr-down case a voice: previously the TV hide just stopped working
    // with no message anywhere on screen.
    //
    // For You gets this line too (no `!shuffle_only` gate): its IN LIBRARY
    // badges are drawn from the same lib_ok-gated library_refs_ as the chart
    // tabs, so a cached grid with the library down needs the same staleness
    // signal.
    //
    // Suppressed under LibraryUnavailable, though. That state's condition is a
    // strict SUBSET of this one (both key off lib_refresh_done_once_ && !lib_ok;
    // it just adds is_foryou && grid_empty), so without the guard the two would
    // co-occur 100% of the time — never one without the other. Unlike the chart
    // tabs, where this line pairs with a Loading/EmptyCategory message about a
    // DIFFERENT subject, here it would name the same service twice for the same
    // fact. Worse, it would be inapplicable: it warns that in-library hiding may
    // be stale, and LibraryUnavailable means there are no posters on screen to
    // carry a badge.
    // Same never-configured-vs-outage distinction as browse_grid_state_message
    // above (see BrowseScreen's constructor doc comment / Fix 1): a box that
    // never had Sonarr set up must not be told Sonarr is "offline" every time
    // it lands on a chart tab in TV mode. sonarr_configured_ is irrelevant in
    // Movies mode — Radarr is never mocked in a way that reaches this path.
    const char* service_warning = nullptr;
    if (lib_refresh_done_once_ && !lib_ok &&
        grid_state != BrowseGridState::LibraryUnavailable) {
        if (tv_mode()) {
            service_warning = sonarr_configured_
                ? "Sonarr offline \xE2\x80\x94 in-library hiding may be stale"
                : "TV library not set up \xE2\x80\x94 in-library hiding "
                  "unavailable";
        } else {
            service_warning =
                "Radarr offline \xE2\x80\x94 in-library hiding may be stale";
        }
    }
    constexpr int kWarnFontPx = 14;
    constexpr int kWarnLineH  = 20;
    if (service_warning) {
        r.mb_draw_text(service_warning,
                       static_cast<float>(chrome::kSafeInset_px),
                       static_cast<float>(content_top + kWarnFontPx),
                       kWarnFontPx, th.highlight2);
    }

    if (state_msg) {
        const std::string msg(state_msg);
        const int tw = r.mb_text_width(msg, 18);
        r.mb_draw_text(msg,
                       static_cast<float>((screen_w - tw) / 2),
                       static_cast<float>(screen_h / 2),
                       18, state_color);
    } else {
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
        const int grid_top = content_top + chrome::kPad3 +
                             (service_warning ? kWarnLineH : 0);
        const int grid_left = chrome::kSafeInset_px;

        // Page-based: scroll_row_ is already set to the page-first-row in
        // handle_input(). No smoothing here — just derive the window.
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
                const ::ui::Color tint = stable_tint_for_id(movie.tmdb_id);
                const MediaRef ref = media_ref_of(movie);
                const bool in_library = library_refs_.count(ref) > 0;
                const bool is_downloading = downloading_refs_.count(ref) > 0;
                chrome::draw_poster_card(
                    r, x, y, cell_w, poster_h,
                    movie.title, movie.year,
                    tint, in_library,
                    /*download_pct=*/is_downloading ? 0 : -1,
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

    }

    // --- Footer hints (ONE call — the old draw_baseline_footer lambda and
    // its duplicate hint list are gone with the early returns) ---
    chrome::draw_footer_hints(r, screen_w, screen_h, {
        {chrome::HintIcon::Btn1Yellow,  "Tab \xE2\x86\x90"},
        {chrome::HintIcon::Btn2Red,     "Exit"},
        {chrome::HintIcon::Btn3Green,   "Tab \xE2\x86\x92"},
        {chrome::HintIcon::Btn4Black,
         filter_available ? "Filters" : (shuffle_only ? "Mode/Shuffle" : "\xE2\x80\x94")},
        {chrome::HintIcon::RotaryNav,   "Browse"},
        {chrome::HintIcon::RotaryPress, "Detail"},
    });

    // ALWAYS LAST, ALWAYS REACHED. See the state-resolution comment above.
    filter_overlay_.render(r, screen_w, screen_h);
}

}  // namespace media_browser::ui
