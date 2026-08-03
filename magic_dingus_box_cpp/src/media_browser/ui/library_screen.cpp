#include "media_browser/ui/library_screen.h"
#include <filesystem>
#include <system_error>
#include <cstdio>
#include "media_browser/ui/library_view.h"
#include "media_browser/ui/mb_chrome.h"
#include "media_browser/ui/mb_ui_utils.h"
#include "ui/text_utf8.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <unordered_map>

#include "app/settings_persistence.h"
#include "media_browser/library/watch_store.h"
#include "media_browser/radarr/radarr_client.h"
#include "media_browser/sonarr/sonarr_client.h"
#include "ui/renderer.h"
#include "ui/theme.h"
#include "utils/time_format.h"

namespace media_browser::ui {

namespace {

// Retro home-menu-inspired layout (target 1280x720), shared with
// detail_screen.cpp. The visual idiom mirrors the home menu:
//   - "LIBRARY" header in the Zen Dots title font, steel-blue (accent2),
//     underlined with a full-width 2px rule (same pattern as "Playlists")
//   - Section dividers in steel-blue (accent2)
//   - Gold-outlined chips (no fill) — matches the home-menu's
//     border-and-text aesthetic instead of filled blocks
//   - Blinking ◂ marker on the focused chip / poster — same 500ms cycle
//     and color (accent2) as the playlist-list selection cursor
//   - Status dots in green / gold / red corner badges (same palette as
//     the home menu's now-playing / missing indicators)
//
// All chrome dimensions (header rule Y, padding X, footer hint Y) MUST
// match detail_screen.cpp so the screens read as a single coherent UI
// when the user moves between them.
constexpr float kPaddingX        = 32.0f;
constexpr float kHeaderBaselineY = 38.0f;     // baseline of "LIBRARY"
constexpr float kHeaderRuleY     = 58.0f;     // 2px steel-blue rule below header

// Filter chip strip — sits just below the header rule. Outlined-only.
// kChipPadX is bumped to 18px (from 14) so each chip's body breathes a bit
// more — the strip only ever holds 4 fixed chips, so we have plenty of
// horizontal slack to spend on legibility.
constexpr float kStripTop        = 76.0f;     // top of chip strip
constexpr float kChipH           = 32.0f;
constexpr float kChipPadX        = 18.0f;
constexpr float kChipMinGap      = 14.0f;     // floor for the dynamic gap
constexpr float kChipBorderW     = 2.0f;

// "Marker zone" reserved at each chip's right edge for the blinking ◂
// focus cursor. The zone exists ALWAYS (focused or not) so chip widths
// don't shift on focus — the label is centered inside the chip's body
// MINUS this zone, which keeps the cursor from drawing over the last
// letter of long labels like "Missing Upgrades".
constexpr float kMarkerZoneW     = 22.0f;

// Section divider beneath the filter strip. Same 2px steel-blue rule
// idiom as kHeaderRuleY — bookends the chip strip on top and bottom.
constexpr float kStripRuleY      = 124.0f;

// Poster grid begins below the strip rule.
constexpr float kGridTop         = 144.0f;
constexpr float kCellGapX        = 20.0f;
constexpr float kCellGapY        = 24.0f;
constexpr float kLabelAreaH      = 52.0f;     // title + year below the poster
constexpr float kPosterFocusW    = 3.0f;      // focused gold outline thickness

// Status dot in the top-right corner of each poster. Drawn as a small
// filled square (matching the home menu's pixel-art status badges) with
// a dark halo so it stays legible regardless of poster artwork.
constexpr float kDotSize         = 14.0f;
constexpr float kDotInset        = 8.0f;

// Footer hint band — 12px from the bottom of the screen, mirroring
// detail_screen.cpp.
constexpr float kFooterMargin    = 12.0f;
constexpr float kFooterReserve   = 36.0f;     // vertical room reserved for hint

// NOTE: a poster_tint_for_tmdb() and a truncate_to_width() used to sit here,
// both uncalled and both warning -Wunused-function on every Pi build. 4f17b61
// deleted them as dead code and noted that identical copies still lived in the
// sibling screens; that is no longer true — this screen no longer defines ANY
// local UI helper. The tint, the truncator, the cubic easings and the byte
// formatter are all one shared implementation now, in mb_ui_utils.h, included at
// the top of this file.

// Slide-in overlay (v1.6.x).
//   Panel sits 20 px inside the wood-frame's right inner edge so the
//   right gold rule reads as a closed 4-sided box (right edge x=1220,
//   bezel inner edge x=1240).
//   Width 480 px → left edge at x=740 when fully open.
//   Vertical extent: y=120 (chrome header bottom) → y=634 (chrome
//   footer hint band top), 514 px tall.
constexpr int kOverlayPanelW         = 480;
constexpr int kOverlayPanelRightGap  = 20;      // Gap between right edge and bezel inner edge
constexpr int kOverlayPanelOpenX     = 1280 - 40 - kOverlayPanelRightGap - kOverlayPanelW;  // 740
constexpr int kOverlayPanelClosedX   = 1280;    // Left edge when fully closed (off-screen right)
constexpr int kOverlayPanelTopY      = 120;
constexpr int kOverlayPanelBottomY   = 634;
constexpr int kOverlayPanelH         = kOverlayPanelBottomY - kOverlayPanelTopY;
constexpr int kOverlayPanelInnerPadX = 24;
constexpr int kOverlayPanelInnerPadY = 16;

// Block layout inside the panel — three logical sections, top to bottom.
constexpr int kOverlaySectionGap     = 18;   // Vertical gap between section blocks
constexpr int kOverlayRowHeight      = 32;   // Per row in Sort + Filter sections
constexpr int kOverlaySectionHeaderH = 28;   // Gold ZenDots heading + gap

// True if the case-insensitive prefix of `s` matches `prefix`. Used by
// the file-state classifier (preserved verbatim from the previous
// revision — quality-string parsing is data-model logic, not visual).
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

LibraryScreen::LibraryScreen(RadarrClient& radarr, SonarrClient* sonarr,
                             library::WatchStore* watch_store,
                             ::app::AppState& state)
    : radarr_(radarr), sonarr_(sonarr), watch_store_(watch_store),
      state_(state) {}

LibraryScreen::~LibraryScreen() {
    // Join the async refresh worker so a thread mid-CURL can't outlive
    // the screen and publish into freed members. Bounded by libcurl's
    // per-attempt timeout (same rationale as QueueScreen/DetailScreen).
    if (refresh_worker_.joinable()) refresh_worker_.join();
}

// ---------------------------------------------------------------------------
// Slide-in overlay state machine (Task 4 of v1.6.x library-overlay plan).
// Render + input wiring land in Tasks 5 + 6; for now these helpers exist so
// the state machine ticks every frame and the open/close transitions are
// callable from input handling once it's wired up.
// ---------------------------------------------------------------------------

void LibraryScreen::start_open_overlay() {
    overlay_state_ = OverlayState::SlidingIn;
    overlay_anim_started_at_ = std::chrono::steady_clock::now();
    // Cursor lands on the currently-active sort row so a single A
    // re-confirms the existing choice (zero-effort cancel).
    using S = ::app::AppState::DisplaySettings::MbLibrarySort;
    switch (state_.display_settings.mb_library_sort) {
        case S::Recent: overlay_focus_row_ = 0; break;
        case S::Title:  overlay_focus_row_ = 1; break;
        case S::Year:   overlay_focus_row_ = 2; break;
        case S::Size:   overlay_focus_row_ = 3; break;
    }
}

void LibraryScreen::start_close_overlay() {
    overlay_state_ = OverlayState::SlidingOut;
    overlay_anim_started_at_ = std::chrono::steady_clock::now();
}

void LibraryScreen::tick_overlay_animation() {
    if (overlay_state_ == OverlayState::Closed ||
        overlay_state_ == OverlayState::Open) return;
    const auto elapsed_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - overlay_anim_started_at_)
        .count();
    if (overlay_state_ == OverlayState::SlidingIn && elapsed_ms >= kOverlaySlideInMs) {
        overlay_state_ = OverlayState::Open;
    } else if (overlay_state_ == OverlayState::SlidingOut && elapsed_ms >= kOverlaySlideOutMs) {
        overlay_state_ = OverlayState::Closed;
    }
}

int LibraryScreen::compute_overlay_left_x(
        OverlayState state,
        std::chrono::steady_clock::time_point anim_started) {
    if (state == OverlayState::Closed)  return kOverlayPanelClosedX;
    if (state == OverlayState::Open)    return kOverlayPanelOpenX;
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(now - anim_started).count();
    if (state == OverlayState::SlidingIn) {
        const float t = std::min(1.0f,
            static_cast<float>(elapsed_ms) / static_cast<float>(kOverlaySlideInMs));
        const float eased = ease_out_cubic(t);
        return static_cast<int>(
            kOverlayPanelClosedX + eased *
                (kOverlayPanelOpenX - kOverlayPanelClosedX));
    }
    // SlidingOut.
    const float t = std::min(1.0f,
        static_cast<float>(elapsed_ms) / static_cast<float>(kOverlaySlideOutMs));
    const float eased = ease_in_cubic(t);
    return static_cast<int>(
        kOverlayPanelOpenX + eased *
            (kOverlayPanelClosedX - kOverlayPanelOpenX));
}

void LibraryScreen::enter() {
    // Kick off a non-blocking refresh on (re-)entry so the grid reflects
    // any adds/removes from DetailScreen — WITHOUT stalling the render
    // thread on two Radarr HTTP round-trips. Stale data from the previous
    // visit stays visible until apply_pending() lands (~50-500ms), same
    // tradeoff as QueueScreen. The very first entry has no prior data, so
    // render() shows its "Loading library..." state until loaded_ flips.
    refresh_async();
}

void LibraryScreen::update() {
    // Drain a finished refresh, then re-arm the cadence so a download that
    // completes / imports while the user lingers on the grid updates its
    // badge live (previously badges only refreshed on leave→re-enter).
    apply_pending();
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - last_refresh_at_).count();
    if (elapsed >= kRefreshIntervalMs) {
        refresh_async();
    }
}

void LibraryScreen::refresh_async() {
    // CAS guard: at most one refresh in flight. A second call while one is
    // running is a no-op — the next update() tick catches up. Stamp the
    // clock here (not on completion) so the cadence measures request→request
    // and a slow Radarr can't cause back-to-back spawns.
    bool expected = false;
    if (!refresh_in_flight_.compare_exchange_strong(expected, true)) return;
    last_refresh_at_ = std::chrono::steady_clock::now();
    // Join a previous finished-but-not-joined worker before reusing the
    // handle (its result was already drained; this just reaps the thread).
    if (refresh_worker_.joinable()) refresh_worker_.join();
    refresh_worker_ = std::thread(&LibraryScreen::run_refresh, this);
}

void LibraryScreen::run_refresh() {
    // Worker thread: do the blocking Radarr/Sonarr calls here, build the
    // result locally, publish under the mutex. Touches NO live member
    // except pending_/result_ready_/refresh_in_flight_ — and NEVER
    // watch_store_, which is main/render-thread-only by its own contract
    // (apply_pending reads it at drain time).
    PendingResult r;
    r.library = radarr_.get_library();

    std::unordered_map<int, int> radarr_to_tmdb;
    for (const auto& m : r.library) {
        if (m.tmdb_id > 0) radarr_to_tmdb[m.radarr_id] = m.tmdb_id;
    }
    // Classify each queue item into one of three badge buckets. Key the
    // importing bucket off tracked_download_state ALONE (not state) — an
    // importing item's primary state may still read "downloading" or
    // "completed". Precedence when building: importBlocked → stuck;
    // importing/importPending → importing; everything else → downloading.
    for (const auto& qi : radarr_.get_queue()) {
        auto it = radarr_to_tmdb.find(qi.movie_id);
        if (it == radarr_to_tmdb.end()) continue;
        const MediaRef ref{MediaKind::Movie, it->second};
        const std::string& tds = qi.tracked_download_state;
        if (tds == "importBlocked" || tds == "importFailed") {
            r.stuck.insert(ref);
        } else if (tds == "importing" || tds == "importPending") {
            r.importing.insert(ref);
        } else {
            r.downloading.insert(ref);
        }
    }

    // ---- TV library + downloading refs (Phase 3 Task 7) ----
    if (sonarr_) {
        // get_library_checked(), NOT get_library(): Sonarr rides Gluetun's
        // netns, so a routine re-link can fail this call mid-refresh, and
        // {} must not read as "the user's shows all vanished". nullopt →
        // sonarr_ok stays false: TV entries are absent this cycle and
        // render() shows the warning line; movies are unaffected.
        if (auto tv_checked = sonarr_->get_library_checked()) {
            r.tv_library = std::move(*tv_checked);
            r.sonarr_ok = true;

            // TV downloading refs, from the queue mapped series_id →
            // tmdb_id via the series vector just fetched (Series carries
            // both ids). Without this, the inclusion rule has no source
            // for a freshly-started 0-file season download — it would
            // never appear in the Library and no TV tile could ever show
            // DOWNLOADING. A nullopt queue is treated as empty, same as
            // the movie path's bare get_queue() above: the library
            // answered, so the grid still shows; only the badge/inclusion
            // signal for in-flight grabs is missing this cycle.
            std::unordered_map<int, int> sonarr_to_tmdb;
            sonarr_to_tmdb.reserve(r.tv_library.size());
            for (const auto& s : r.tv_library) {
                if (s.tmdb_id > 0) sonarr_to_tmdb[s.sonarr_id] = s.tmdb_id;
            }
            if (auto tv_queue = sonarr_->get_queue_checked()) {
                for (const auto& qi : *tv_queue) {
                    auto it = sonarr_to_tmdb.find(qi.series_id);
                    if (it == sonarr_to_tmdb.end()) continue;
                    r.downloading.insert(MediaRef{MediaKind::Tv, it->second});
                }
            }
        }
    } else {
        // No Sonarr configured (keyless box holding the mock): an empty TV
        // library is the genuine answer, not an outage — no warning line.
        r.sonarr_ok = true;
    }

    {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        pending_ = std::move(r);
    }
    result_ready_.store(true);
    refresh_in_flight_.store(false);
}

void LibraryScreen::apply_pending() {
    if (!result_ready_.load()) return;
    PendingResult r;
    {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        r = std::move(pending_);
    }
    result_ready_.store(false);

    // Swap into live state + rebuild entries and the filtered view. This
    // MUST all run here on the render thread — entries_ holds raw
    // Movie*/Series* into library_/tv_library_, view_ holds raw
    // LibraryEntry* into entries_, and rebuild_view() reads
    // state_.display_settings and mutates grid_cursor_/scroll_row_ — so
    // all four containers are swapped+rebuilt atomically on one thread.
    library_          = std::move(r.library);
    tv_library_       = std::move(r.tv_library);
    downloading_refs_ = std::move(r.downloading);
    stuck_refs_       = std::move(r.stuck);
    importing_refs_   = std::move(r.importing);
    sonarr_ok_        = r.sonarr_ok;
    loaded_ = true;

    // Watch data is read HERE, at drain time on the render thread —
    // WatchStore is main/render-thread-only by construction, so the worker
    // in run_refresh() must never touch it. Null/degraded store → empty
    // snapshots → nothing reads as watched, which is the honest fallback.
    std::unordered_set<int> watched_movie_ids;
    std::unordered_map<int, int> tv_watched_counts;
    if (watch_store_) {
        watched_movie_ids = watch_store_->watched_movie_ids();
        tv_watched_counts = watch_store_->tv_watched_counts();
    }
    entries_ = build_library_entries(library_, tv_library_, watched_movie_ids,
                                     tv_watched_counts, downloading_refs_);
    rebuild_view();
}

void LibraryScreen::rebuild_view() {
    // Which rows survive and in what order is decided by the pure
    // build_library_view() in media_browser/ui/library_view.h. Everything
    // that stays here is the part that cannot be pure: reading the clock for
    // the cutoff, the latched warn when that read fails, assigning view_, and
    // clamping the cursor. library_view.cpp names no ::ui::Renderer, so unlike
    // this file it compiles into test_media_browser_unit — which is the point
    // of the split. The filter and sort semantics had no coverage at all while
    // they lived here, and that is how the cutoff bug described below shipped.

    // Driven by display_settings.mb_library_filter (the v1.6.x persisted
    // overlay choice), NOT the legacy Filter enum member filter_ which
    // is now dead config. The legacy enum stays in the header for any
    // ABI compat we haven't audited but does not influence the view.
    using F = ::app::AppState::DisplaySettings::MbLibraryFilter;
    const F filter = state_.display_settings.mb_library_filter;
    // "Recently added" cutoff: 30 days ago, formatted as an ISO-8601
    // string so the filter can compare lexicographically against
    // Movie.added_at (which Radarr emits as ISO-8601, e.g.
    // "2024-12-31T08:15:42Z"). ISO-8601 strings sort chronologically as
    // plain strings — no parsing required.
    //
    // utils::iso8601_utc returns "" if the conversion fails. That MUST be
    // branched on rather than compared: "" is less than every non-empty string,
    // so `added_at >= ""` is true for every row and the date filter would
    // silently become a pass-everything filter. (The hand-rolled version this
    // replaced ignored gmtime_r's return and formatted a zero-initialized tm
    // into "1900-01-00T00:00:00Z" — a different string with the same
    // pass-everything effect, and no way to tell from the UI that it happened.)
    //
    // Falling back to show-all is the deliberate choice over showing nothing:
    // an empty grid reads as "your library is empty", which is a scarier and
    // more misleading failure on an appliance than an unfiltered one. The log
    // line is what makes it diagnosable instead of silent, and
    // library_row_kept() takes the validity as its own argument so the fallback
    // is a branch a test can reach rather than one that needs a broken clock.
    std::string thirty_days_ago_iso;
    bool recent_cutoff_valid = false;
    if (filter == F::RecentlyAdded) {
        const auto now = std::chrono::system_clock::now();
        const auto cutoff = now - std::chrono::hours(24 * 30);
        thirty_days_ago_iso =
            utils::iso8601_utc(std::chrono::system_clock::to_time_t(cutoff));
        recent_cutoff_valid = !thirty_days_ago_iso.empty();
        // Latched, and gated on the filter above, because rebuild_view() runs
        // about every 2s for as long as this screen is open (update() re-arms
        // refresh_async() on kRefreshIntervalMs and each landing result rebuilds).
        // Un-latched this would emit ~30 identical lines a minute, and
        // un-gated it would say "'Recently added' will show the whole library"
        // while the user is on a different filter entirely.
        if (!recent_cutoff_valid && !warned_recent_cutoff_) {
            warned_recent_cutoff_ = true;
            spdlog::warn("[LibraryScreen] could not format the 30-day cutoff; "
                         "'Recently added' will show the whole library");
        }
    }
    // ---- Filter + sort ----
    // view_ holds raw LibraryEntry* into entries_ (whose entries in turn
    // borrow library_/tv_library_), so this MUST stay on the render thread
    // and immediately follow the swaps in apply_pending() — any
    // reallocation of those containers invalidates every pointer built
    // here. (The overlay's SELECT branch also calls this; entries_ is
    // untouched there, so the aliasing holds.)
    view_ = build_library_view(entries_, filter,
                               state_.display_settings.mb_library_sort,
                               thirty_days_ago_iso, recent_cutoff_valid);

    // Clamp the grid cursor + scroll row to the new view's bounds.
    // After a sort/filter change, the cursor may land far outside the
    // new view's row range — clamp grid_cursor_ to [0, n-1] first, then
    // ensure scroll_row_ keeps the cursor visible (i.e. scroll_row_ ≤
    // cursor_row). Without the visibility clamp, an operator scrolled
    // down before applying a filter that shrinks the view would see an
    // empty grid until the next render's render-side clamp catches up.
    const int n = static_cast<int>(view_.size());
    if (grid_cursor_ >= n) grid_cursor_ = std::max(0, n - 1);
    // Page-based: snap scroll to the page that contains the (possibly clamped) cursor.
    // 2 = kPageRows (same value as kVisibleRows in render()).
    const int cursor_row = (n == 0) ? 0 : grid_cursor_ / kGridCols;
    scroll_row_ = (cursor_row / 2) * 2;
    const int max_scroll_row = std::max(0, (n - 1) / kGridCols);
    if (scroll_row_ > max_scroll_row) scroll_row_ = max_scroll_row;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

Screen LibraryScreen::handle_input(const std::vector<platform::InputEvent>& events) {
    // LibraryScreen sits at strip position 2 in the Marquee 5-tab strip:
    //   Popular(0) · Top Rated(1) · Library(2) · Search(3) · Settings(4)
    // BTN1 (PREV, yellow) returns to BrowseScreen — BrowseScreen retains
    // its category_ across transitions, so the user resumes on whatever
    // content tab they were on before navigating to Library (typically
    // TopRated, the immediate left neighbour). BTN3 (NEXT, green)
    // transitions to SearchScreen.
    for (const auto& e : events) {
        // ============================================================
        // Overlay input capture: when the overlay is open or animating,
        // it consumes ROTATE / SELECT / SETTINGS_MENU inputs. The
        // underlying grid does NOT see these events while the panel is
        // visible. SETTINGS_MENU is handled separately below as a
        // toggle (open/close). BTN2 (PLAY_PAUSE) is owned globally by
        // the exit modal in main.cpp and never arrives here.
        // ============================================================
        const bool overlay_active = (overlay_state_ != OverlayState::Closed);
        if (overlay_active) {
            // Rotary CW/CCW — walk through the 8 focusable rows.
            if (e.action == platform::InputAction::ROTATE) {
                if (e.delta == 0) continue;
                overlay_focus_row_ = std::clamp(
                    overlay_focus_row_ + e.delta,
                    0, kOverlayFocusableRows - 1);
                continue;
            }
            // D-pad UP / DOWN — also walks through the 8 focusable rows
            // so the operator can use either rotary OR D-pad to move
            // around the overlay. Without this clause the underlying
            // grid's ROTATE_VERTICAL handler would silently mutate
            // grid_cursor_ while the overlay is occluding the grid.
            if (e.action == platform::InputAction::ROTATE_VERTICAL) {
                if (e.delta == 0) continue;
                overlay_focus_row_ = std::clamp(
                    overlay_focus_row_ + e.delta,
                    0, kOverlayFocusableRows - 1);
                continue;
            }
            // SELECT — apply sort/filter, persist, close overlay.
            if (e.action == platform::InputAction::SELECT && e.pressed) {
                if (overlay_state_ != OverlayState::Open) continue;
                // Map focused row → sort or filter mutation.
                using S = ::app::AppState::DisplaySettings::MbLibrarySort;
                using F = ::app::AppState::DisplaySettings::MbLibraryFilter;
                bool changed = false;
                switch (overlay_focus_row_) {
                    case 0: state_.display_settings.mb_library_sort = S::Recent;        changed = true; break;
                    case 1: state_.display_settings.mb_library_sort = S::Title;         changed = true; break;
                    case 2: state_.display_settings.mb_library_sort = S::Year;          changed = true; break;
                    case 3: state_.display_settings.mb_library_sort = S::Size;          changed = true; break;
                    case 4: state_.display_settings.mb_library_filter = F::All;           changed = true; break;
                    case 5: state_.display_settings.mb_library_filter = F::Unwatched;     changed = true; break;
                    case 6: state_.display_settings.mb_library_filter = F::MissingFiles;  changed = true; break;
                    case 7: state_.display_settings.mb_library_filter = F::RecentlyAdded; changed = true; break;
                }
                if (changed) {
                    ::app::SettingsPersistence::save_settings(state_);
                    rebuild_view();
                    start_close_overlay();
                }
                continue;
            }
            // BTN1 / BTN3 are no-ops while overlay is open (prevents
            // tab-jumping the underlying grid).
            if (e.action == platform::InputAction::PREV ||
                e.action == platform::InputAction::NEXT) {
                continue;
            }
            // SETTINGS_MENU is handled by the toggle branch below; let
            // it through.
        }

        // BTN4 (SETTINGS_MENU, black) — toggles the slide-in overlay.
        // Re-pressing closes it. While SlidingIn or SlidingOut, ignore —
        // operator will wait ~200 ms for the animation to settle. Prevents
        // mid-animation state thrash.
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            if (overlay_state_ == OverlayState::Closed) {
                start_open_overlay();
            } else if (overlay_state_ == OverlayState::Open) {
                start_close_overlay();
            }
            continue;
        }

        // BTN1 (PREV, yellow) — previous tab. Library is at position 3 in
        // the 6-tab strip (Popular | Top Rated | Search | Library | Queue
        // | Settings), so PREV lands on Search at position 2.
        if (e.action == platform::InputAction::PREV && e.pressed) {
            return Screen::Search;
        }

        // BTN3 (NEXT, green) — next tab. Library is at position 3, NEXT
        // goes to Queue at position 4.
        if (e.action == platform::InputAction::NEXT && e.pressed) {
            return Screen::Queue;
        }

        // BTN2 (PLAY_PAUSE, red) — intercepted globally by the exit modal
        // in main.cpp. It never reaches here; no per-screen handler needed.

        // ROTATE (rotary CW/CCW + D-pad LEFT/RIGHT) — walk one cell at
        // a time, row-major.
        if (e.action == platform::InputAction::ROTATE) {
            if (view_.empty()) continue;
            const int n = static_cast<int>(view_.size());
            grid_cursor_ = std::clamp(grid_cursor_ + e.delta, 0, n - 1);
            continue;
        }

        // ROTATE_VERTICAL (D-pad UP/DOWN) — walk one row at a time.
        if (e.action == platform::InputAction::ROTATE_VERTICAL) {
            if (view_.empty()) continue;
            const int row = grid_cursor_ / kGridCols;
            const int col = grid_cursor_ % kGridCols;
            const int max_row = (static_cast<int>(view_.size()) - 1) / kGridCols;
            const int new_row = std::clamp(row + e.delta, 0, max_row);
            const int new_idx = new_row * kGridCols + col;
            if (new_idx < static_cast<int>(view_.size())) {
                grid_cursor_ = new_idx;
            } else if (!view_.empty()) {
                grid_cursor_ = static_cast<int>(view_.size()) - 1;
            }
            continue;
        }

        // SELECT (rotary click + gamepad A) — open detail. The KIND picks
        // the destination and rides in the returned Screen value, exactly
        // like BrowseScreen's TV handoff: movie/TV TMDB id spaces overlap
        // completely, so the id alone must never choose the screen.
        if (e.action == platform::InputAction::SELECT && e.pressed) {
            if (!view_.empty() && grid_cursor_ >= 0 &&
                grid_cursor_ < static_cast<int>(view_.size())) {
                selected_ref_ = view_[grid_cursor_]->ref;
                return selected_ref_.kind == MediaKind::Tv
                           ? Screen::SeriesDetail
                           : Screen::Detail;
            }
        }
    }

    // Page-based scroll: snap scroll_row_ to the page boundary that contains
    // the cursor row. Each page is 2 rows (= kVisibleRows in render()).
    if (!view_.empty()) {
        const int cursor_row = grid_cursor_ / kGridCols;
        scroll_row_ = (cursor_row / 2) * 2;
    }

    return Screen::Library;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void LibraryScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    tick_overlay_animation();

    namespace chrome = ::media_browser::ui::chrome;
    const ::ui::Theme& th = r.mb_theme();

    r.mb_fill_background();

    // --- Marquee header (the shared strip, Library active here) ---
    // Built from chrome::marquee_tabs() rather than a local copy: this
    // screen used to carry its own six-label array, which silently lost
    // the "For You" chip when that tab was added to Browse.
    const std::vector<chrome::TabSpec> tabs = chrome::marquee_tabs("Library");
    const int content_top = chrome::draw_screen_header(
        r, screen_w, "Library", tabs, /*focused_tab=*/-1);

    // --- Stats line: "N titles · X.X GB · Y.Y GB free" ---
    // Cached: this used to run EVERY FRAME — an O(N) sum over the whole
    // library, a statvfs() kernel syscall (std::filesystem::space) against
    // the SSD mount, and several heap-allocating string concats, 60×/sec,
    // to draw one label. Refresh every 5s (disk free drifts slowly; the
    // title count also updates on the next tick after apply_pending()
    // swaps in a fresh library — well within the 2s refresh cadence).
    {
        const auto now = std::chrono::steady_clock::now();
        if (stats_line_.empty() ||
            now - last_stats_refresh_ >= std::chrono::seconds(5)) {
            last_stats_refresh_ = now;
            // Mixed basis: entries_ is the grid's truth (movies + included
            // TV), so the count and the used-bytes sum both read it. Movie
            // bytes come from the file, TV bytes from Sonarr's
            // sizeOnDisk (which includes S0 specials — correct for a DISK
            // stat, unlike the watched math).
            int64_t used_bytes = 0;
            for (const LibraryEntry& e : entries_) {
                if (e.movie) {
                    if (e.movie->has_file) used_bytes += e.movie->file_size_bytes;
                } else if (e.series) {
                    used_bytes += e.series->size_on_disk_bytes;
                }
            }
            int64_t free_bytes = 0;
            std::error_code ec;
            auto info = std::filesystem::space("/mnt/ssd/library", ec);
            if (!ec) free_bytes = static_cast<int64_t>(info.available);
            stats_line_ =
                std::to_string(entries_.size()) + " titles  ·  "
                + (used_bytes > 0 ? format_bytes(used_bytes)
                                  : std::string("0 B")) + " used  ·  "
                + (free_bytes > 0 ? format_bytes(free_bytes) + " free" : "");
        }
    }
    const std::string& stats = stats_line_;
    const int stats_y = content_top + chrome::kPad2;
    r.mb_draw_text(stats,
                   static_cast<float>(chrome::kSafeInset_px),
                   static_cast<float>(stats_y + 14),
                   14, th.dim);

    // Sonarr-offline warning line (queue_screen idiom: accent-colored
    // sub-line, drawn only when Sonarr was configured but didn't answer
    // the last library fetch). TV entries are simply ABSENT that cycle —
    // this line is what keeps that from reading as "my shows vanished".
    // Right-aligned on the stats row so the layout below never shifts.
    // Drawn before the empty-state early-returns on purpose: a
    // movies-empty box mid-outage still needs the explanation.
    if (sonarr_ && !sonarr_ok_) {
        const std::string warn_text =
            "Sonarr offline \xE2\x80\x94 TV shows hidden";
        const int warn_w = r.mb_text_width(warn_text, 14);
        r.mb_draw_text(warn_text,
                       static_cast<float>(screen_w - chrome::kSafeInset_px
                                          - warn_w),
                       static_cast<float>(stats_y + 14),
                       14, th.accent, 0.95f);
    }

    // Legacy sort sub-tab strip (Recent / Title / Year / Size) was
    // dropped in v1.6.4 — the slide-in overlay (BTN4) is the canonical
    // place to switch sort + filter now, and the active selection is
    // surfaced by the small "Sort: … · Filter: …" indicator drawn at
    // the bottom-left of the body, just above the chrome footer hints.

    // --- Empty state ---
    if (!loaded_) {
        const std::string msg = "Loading library...";
        const int tw = r.mb_text_width(msg, 18);
        r.mb_draw_text(msg,
                       static_cast<float>((screen_w - tw) / 2),
                       static_cast<float>(screen_h / 2),
                       18, th.dim);
        chrome::draw_footer_hints(r, screen_w, screen_h, {
            {chrome::HintIcon::Btn1Yellow,  "Tab \xE2\x86\x90"},
            {chrome::HintIcon::Btn2Red,     "Exit"},
            {chrome::HintIcon::Btn3Green,   "Tab \xE2\x86\x92"},
            {chrome::HintIcon::Btn4Black,   "Sort+Filter"},
            {chrome::HintIcon::RotaryNav,   "Browse"},
            {chrome::HintIcon::RotaryPress, "Detail"},
        });
        return;
    }
    if (view_.empty()) {
        const std::string msg = entries_.empty()
            ? "Library is empty — add movies from Browse"
            : "No matches for the current filter";
        const int tw = r.mb_text_width(msg, 18);
        r.mb_draw_text(msg,
                       static_cast<float>((screen_w - tw) / 2),
                       static_cast<float>(screen_h / 2),
                       18, th.dim);
        chrome::draw_footer_hints(r, screen_w, screen_h, {
            {chrome::HintIcon::Btn1Yellow,  "Tab \xE2\x86\x90"},
            {chrome::HintIcon::Btn2Red,     "Exit"},
            {chrome::HintIcon::Btn3Green,   "Tab \xE2\x86\x92"},
            {chrome::HintIcon::Btn4Black,   "Sort+Filter"},
            {chrome::HintIcon::RotaryNav,   "Browse"},
            {chrome::HintIcon::RotaryPress, "Detail"},
        });
        return;
    }

    // --- 9-column poster grid ---
    // Same layout as BrowseScreen: meta area fits 2 lines so long
    // titles can wrap; year lives inside the poster card (bottom-right
    // pill) so the meta line shows the title only.
    constexpr int kCellGap       = 8;
    constexpr int kRowGap        = 22;
    constexpr int kVisibleRows   = 2;
    constexpr int kMetaFontPx    = 14;
    constexpr int kMetaLineGap   = 2;
    constexpr int kMetaTotalH    = kMetaFontPx + kMetaLineGap + kMetaFontPx;
    constexpr int kMetaGap       = 4;
    const int content_w = screen_w - 2 * chrome::kSafeInset_px;
    const int cell_w   = (content_w - (kGridCols - 1) * kCellGap) / kGridCols;
    const int poster_h = static_cast<int>(static_cast<float>(cell_w) * 1.5f);
    const int cell_h   = poster_h + kMetaGap + kMetaTotalH;
    const int grid_top = stats_y + 14 + chrome::kPad3;
    const int grid_left = chrome::kSafeInset_px;

    // Page-based: scroll_row_ is set in handle_input(). No smooth adjustment.
    const int total_rows =
        (static_cast<int>(view_.size()) + kGridCols - 1) / kGridCols;
    const int last_visible_row =
        std::min(scroll_row_ + kVisibleRows, total_rows);

    for (int row = scroll_row_; row < last_visible_row; ++row) {
        for (int col = 0; col < kGridCols; ++col) {
            const int idx = row * kGridCols + col;
            if (idx >= static_cast<int>(view_.size())) break;
            const LibraryEntry* en = view_[idx];

            const int x = grid_left + col * (cell_w + kCellGap);
            const int y = grid_top + (row - scroll_row_) * (cell_h + kRowGap);

            // Poster CARD via shared chrome helper — real TMDB image
            // when loaded, deterministic tint placeholder while it
            // fetches, plus year pill + IN LIBRARY badge (or
            // DOWNLOADING / BAD RELEASE badge when the title is in the
            // queue or stuck on importBlocked). Tint keyed on the bare id
            // is fine — a movie/TV id collision sharing a placeholder
            // color is cosmetic, unlike sharing a key.
            const ::ui::Color tint = stable_tint_for_id(en->ref.id);
            // Badge precedence mirrors draw_poster_card: stuck > importing
            // > downloading > in_library. Compute each exclusive of the
            // higher-priority ones so at most one badge is requested.
            // A movie file whose measured duration is wildly off the
            // expected runtime (renamed trailer/junk upload) earns the
            // same BAD RELEASE badge as an importBlocked queue item —
            // Detail explains the specifics. (Movie-only signal; the
            // stuck/importing sets also carry only Movie refs today.)
            const bool is_stuck =
                stuck_refs_.count(en->ref) > 0 ||
                (en->movie && file_runtime_suspicious(*en->movie));
            const bool is_importing =
                !is_stuck && importing_refs_.count(en->ref) > 0;
            const bool is_downloading =
                !is_stuck && !is_importing && en->downloading;
            chrome::draw_poster_card(
                r, x, y, cell_w, poster_h,
                en->title, en->year,
                tint, /*in_library=*/true,
                /*download_pct=*/is_downloading ? 0 : -1,
                /*poster_url=*/en->poster_url,
                /*is_stuck=*/is_stuck,
                /*is_importing=*/is_importing);

            // TV chip — a small kind marker so mixed rows stay scannable.
            // Same drawing idiom as the chrome badges (solid bg quad so it
            // reads over any poster art, bordered, small font), accent
            // colored, top-left — stacked BELOW the status badge, which
            // always occupies the corner itself (in_library is
            // unconditionally true here, so draw_poster_card always drew
            // one of its four badges above).
            if (en->ref.kind == MediaKind::Tv) {
                constexpr int kTvChipFontPx = 12;
                constexpr int kTvChipPadX   = 6;
                constexpr int kTvChipPadY   = 2;
                const std::string tv_label = "TV";
                const int chip_x = x + chrome::kPad1;
                const int chip_y = y + chrome::kPad1 + chrome::kBadgeBoxH
                                 + chrome::kPad1;
                const int text_w = r.mb_text_width(tv_label, kTvChipFontPx);
                const int box_w  = text_w + 2 * kTvChipPadX;
                const int box_h  = kTvChipFontPx + 2 * kTvChipPadY;
                r.mb_fill_rect(static_cast<float>(chip_x),
                               static_cast<float>(chip_y),
                               static_cast<float>(box_w),
                               static_cast<float>(box_h), th.bg);
                r.mb_stroke_rect(static_cast<float>(chip_x),
                                 static_cast<float>(chip_y),
                                 static_cast<float>(box_w),
                                 static_cast<float>(box_h),
                                 2.0f, th.accent);
                r.mb_draw_text(tv_label,
                               static_cast<float>(chip_x + kTvChipPadX),
                               static_cast<float>(chip_y + kTvChipPadY
                                                  + kTvChipFontPx - 2),
                               kTvChipFontPx, th.accent);
            }

            // Meta line below poster: title only, wrapped to 2 lines
            // when needed. Year now lives inside the poster card.
            // Line 1 = longest leading word chunk that fits on one
            // line; line 2 = remainder, truncated with ellipsis if it
            // also overflows. Same logic BrowseScreen uses — kept
            // inline (rather than factored into mb_chrome) because the
            // truncation pattern depends on the renderer instance.
            const std::string& title = en->title;
            const float max_w_f = static_cast<float>(cell_w);
            std::string line1, line2;
            if (r.mb_text_width(title, kMetaFontPx) <= max_w_f) {
                line1 = title;
            } else {
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
                    // No space to break at — character-level truncate
                    // with trailing ellipsis.
                    std::string candidate = title;
                    while (!candidate.empty()
                           && r.mb_text_width(candidate + "...", kMetaFontPx)
                              > max_w_f) {
                        ::ui::utf8_pop_back(candidate);
                    }
                    line1 = candidate.empty() ? title : (candidate + "...");
                } else {
                    line1 = title.substr(0, split);
                    std::string rem = title.substr(split + 1);
                    if (r.mb_text_width(rem, kMetaFontPx) <= max_w_f) {
                        line2 = rem;
                    } else {
                        std::string candidate = rem;
                        while (!candidate.empty()
                               && r.mb_text_width(candidate + "...", kMetaFontPx)
                                  > max_w_f) {
                            ::ui::utf8_pop_back(candidate);
                        }
                        line2 = candidate.empty() ? rem : (candidate + "...");
                    }
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

            if (idx == grid_cursor_) {
                chrome::draw_focus_ring(r, x, y, cell_w, poster_h);
            }
        }
    }

    // --- Active filter indicator (bottom-left of body, above footer) ---
    // Replaces the legacy sort sub-tab strip — surfaces what's currently
    // in effect from the slide-in overlay so the operator never has to
    // open the panel just to check.
    {
        using S = ::app::AppState::DisplaySettings::MbLibrarySort;
        using F = ::app::AppState::DisplaySettings::MbLibraryFilter;
        const char* sort_label = "Recent";
        switch (state_.display_settings.mb_library_sort) {
            case S::Recent: sort_label = "Recent"; break;
            case S::Title:  sort_label = "Title";  break;
            case S::Year:   sort_label = "Year";   break;
            case S::Size:   sort_label = "Size";   break;
        }
        const char* filter_label = "All";
        switch (state_.display_settings.mb_library_filter) {
            case F::All:           filter_label = "All";            break;
            case F::Unwatched:     filter_label = "Unwatched";      break;
            case F::MissingFiles:  filter_label = "Missing files";  break;
            case F::RecentlyAdded: filter_label = "Recently added"; break;
        }
        std::string indicator = std::string("Sort: ") + sort_label
                              + "  \xC2\xB7  Filter: " + filter_label;
        // Sit ~24 px above the footer-hint baseline so we don't crash
        // into the icons when long filter labels render. The chrome
        // footer baseline = screen_h - kFrameInset_px - kPad3 = h-64;
        // putting our line at h-64-24 = h-88 keeps a clean gap.
        const int indicator_y = screen_h - chrome::kFrameInset_px
                              - chrome::kPad3 - 24;
        r.mb_draw_text(indicator,
                       static_cast<float>(chrome::kSafeInset_px),
                       static_cast<float>(indicator_y),
                       th.font_small_size, th.dim);
    }

    // --- Footer hints ---
    chrome::draw_footer_hints(r, screen_w, screen_h, {
        {chrome::HintIcon::Btn1Yellow,  "Tab \xE2\x86\x90"},
        {chrome::HintIcon::Btn2Red,     "Exit"},
        {chrome::HintIcon::Btn3Green,   "Tab \xE2\x86\x92"},
        {chrome::HintIcon::Btn4Black,   "Sort+Filter"},
        {chrome::HintIcon::RotaryNav,   "Browse"},
        {chrome::HintIcon::RotaryPress, "Detail"},
    });

    // ============================================================
    // Slide-in overlay (v1.6.x)
    // ============================================================
    if (overlay_state_ != OverlayState::Closed) {
        const auto& th = r.mb_theme();

        const int panel_x = compute_overlay_left_x(
            overlay_state_, overlay_anim_started_at_);
        const float fpanel_x = static_cast<float>(panel_x);
        const float fpanel_y = static_cast<float>(kOverlayPanelTopY);
        const float fpanel_w = static_cast<float>(kOverlayPanelW);
        const float fpanel_h = static_cast<float>(kOverlayPanelH);

        // Panel background — bg_lift, fully opaque.
        r.mb_fill_rect(fpanel_x, fpanel_y, fpanel_w, fpanel_h, th.bg_lift);
        // Top edge: 2 px gold rule.
        r.mb_fill_rect(fpanel_x, fpanel_y, fpanel_w, 2.0f, th.accent);
        // Bottom edge: 2 px gold rule.
        r.mb_fill_rect(fpanel_x, fpanel_y + fpanel_h - 2.0f,
                       fpanel_w, 2.0f, th.accent);
        // Left edge: 2 px gold rule.
        r.mb_fill_rect(fpanel_x, fpanel_y, 2.0f, fpanel_h, th.accent);
        // Right edge: 2 px gold rule (panel sits 20 px in from bezel so it's visible).
        r.mb_fill_rect(fpanel_x + fpanel_w - 2.0f, fpanel_y,
                       2.0f, fpanel_h, th.accent);

        // Panel content x — inset from left rule by panel inner pad.
        const int content_x = panel_x + kOverlayPanelInnerPadX;

        // ---- Stats block ----
        // Gold ZenDots heading: "LIBRARY"
        constexpr int kPanelTitleFontPx = 22;
        const int title_baseline = kOverlayPanelTopY + kOverlayPanelInnerPadY +
                                   kPanelTitleFontPx - 2;
        r.mb_draw_title_text("LIBRARY",
                             static_cast<float>(content_x),
                             static_cast<float>(title_baseline),
                             kPanelTitleFontPx, th.accent);

        // Three stat lines in cream body font, 14 px each.
        constexpr int kStatFontPx = 14;
        const int stat_y0 = title_baseline + 24;
        char buf_titles[64], buf_used[64], buf_free[64];
        std::snprintf(buf_titles, sizeof(buf_titles), "%zu titles",
                      entries_.size());
        int64_t used_bytes_ov = 0;
        for (const LibraryEntry& en : entries_) {
            if (en.movie) {
                if (en.movie->has_file) used_bytes_ov += en.movie->file_size_bytes;
            } else if (en.series) {
                used_bytes_ov += en.series->size_on_disk_bytes;
            }
        }
        // Shared format_bytes() covers bytes > 0 only; an empty library is a
        // real zero and keeps the "0 B" the deleted local copy produced.
        const std::string used_str = used_bytes_ov > 0
                                         ? format_bytes(used_bytes_ov)
                                         : std::string("0 B");
        std::snprintf(buf_used, sizeof(buf_used), "%s used",
                      used_str.c_str());
        int64_t free_bytes_ov = 0;
        {
            std::error_code ec;
            auto info = std::filesystem::space("/mnt/ssd/library", ec);
            if (!ec) free_bytes_ov = static_cast<int64_t>(info.available);
        }
        std::snprintf(buf_free, sizeof(buf_free), "%s free",
                      free_bytes_ov > 0 ? format_bytes(free_bytes_ov).c_str() : "—");

        r.mb_draw_text(buf_titles,
                       static_cast<float>(content_x),
                       static_cast<float>(stat_y0),
                       kStatFontPx, th.fg);
        r.mb_draw_text(buf_used,
                       static_cast<float>(content_x),
                       static_cast<float>(stat_y0 + 18),
                       kStatFontPx, th.fg);
        r.mb_draw_text(buf_free,
                       static_cast<float>(content_x),
                       static_cast<float>(stat_y0 + 36),
                       kStatFontPx, th.fg);

        // 1 px dim divider under stats.
        const int divider1_y = stat_y0 + 56;
        r.mb_draw_line(static_cast<float>(content_x),
                       static_cast<float>(divider1_y),
                       static_cast<float>(panel_x + kOverlayPanelW - kOverlayPanelInnerPadX),
                       static_cast<float>(divider1_y),
                       1.0f, th.dim, 0.5f);

        // ---- Sort by section ----
        constexpr int kSectionHeadingFontPx = 14;
        constexpr int kRowFontPx = 16;
        const int sort_heading_y = divider1_y + kOverlaySectionGap;
        r.mb_draw_title_text("SORT BY",
                             static_cast<float>(content_x),
                             static_cast<float>(sort_heading_y),
                             kSectionHeadingFontPx, th.accent);

        struct Row { const char* label; bool is_active; bool is_focused; };
        using S = ::app::AppState::DisplaySettings::MbLibrarySort;
        const S active_sort = state_.display_settings.mb_library_sort;
        const Row sort_rows[4] = {
            {"Recent", active_sort == S::Recent, overlay_focus_row_ == 0},
            {"Title",  active_sort == S::Title,  overlay_focus_row_ == 1},
            {"Year",   active_sort == S::Year,   overlay_focus_row_ == 2},
            {"Size",   active_sort == S::Size,   overlay_focus_row_ == 3},
        };

        const int sort_rows_y0 = sort_heading_y + kOverlaySectionHeaderH;
        for (int i = 0; i < 4; ++i) {
            const int row_y = sort_rows_y0 + i * kOverlayRowHeight;
            const ::ui::Color label_color =
                (sort_rows[i].is_active || sort_rows[i].is_focused)
                    ? th.accent : th.dim;
            // Focused row gets a blinking gold ▶ marker 18 px to the
            // left of the label x. Reuses the same draw-cursor-marker
            // pattern from MbSettingsScreen (right-pointing variant).
            if (sort_rows[i].is_focused) {
                const float marker_x = static_cast<float>(content_x);
                const float marker_cy = static_cast<float>(row_y + kOverlayRowHeight / 2);
                constexpr float kMarkerHalfH = 6.0f;
                constexpr float kMarkerW = 7.2f;
                r.mb_fill_triangle(
                    marker_x,           marker_cy - kMarkerHalfH,
                    marker_x,           marker_cy + kMarkerHalfH,
                    marker_x + kMarkerW, marker_cy,
                    th.accent, 1.0f);
            }
            r.mb_draw_text(sort_rows[i].label,
                           static_cast<float>(content_x + 18),
                           static_cast<float>(row_y + kOverlayRowHeight - 10),
                           kRowFontPx, label_color);
        }

        // 1 px divider under sort.
        const int divider2_y = sort_rows_y0 + 4 * kOverlayRowHeight + 6;
        r.mb_draw_line(static_cast<float>(content_x),
                       static_cast<float>(divider2_y),
                       static_cast<float>(panel_x + kOverlayPanelW - kOverlayPanelInnerPadX),
                       static_cast<float>(divider2_y),
                       1.0f, th.dim, 0.5f);

        // ---- Filter section ----
        const int filter_heading_y = divider2_y + kOverlaySectionGap;
        r.mb_draw_title_text("FILTER",
                             static_cast<float>(content_x),
                             static_cast<float>(filter_heading_y),
                             kSectionHeadingFontPx, th.accent);

        using F = ::app::AppState::DisplaySettings::MbLibraryFilter;
        const F active_filter = state_.display_settings.mb_library_filter;
        // Unwatched is a real filter as of Phase 3 (WatchStore-fed
        // entry.watched) — the "(soon)" dimming this block used to carry
        // is gone with the placeholder.
        struct FilterRow { const char* label; bool is_active; bool is_focused; };
        const FilterRow filter_rows[4] = {
            {"All",            active_filter == F::All,            overlay_focus_row_ == 4},
            {"Unwatched",      active_filter == F::Unwatched,      overlay_focus_row_ == 5},
            {"Missing files",  active_filter == F::MissingFiles,   overlay_focus_row_ == 6},
            {"Recently added", active_filter == F::RecentlyAdded,  overlay_focus_row_ == 7},
        };
        const int filter_rows_y0 = filter_heading_y + kOverlaySectionHeaderH;
        for (int i = 0; i < 4; ++i) {
            const int row_y = filter_rows_y0 + i * kOverlayRowHeight;
            const ::ui::Color label_color =
                (filter_rows[i].is_active || filter_rows[i].is_focused)
                    ? th.accent : th.dim;
            if (filter_rows[i].is_focused) {
                const float marker_x = static_cast<float>(content_x);
                const float marker_cy = static_cast<float>(row_y + kOverlayRowHeight / 2);
                constexpr float kMarkerHalfH = 6.0f;
                constexpr float kMarkerW = 7.2f;
                r.mb_fill_triangle(
                    marker_x,           marker_cy - kMarkerHalfH,
                    marker_x,           marker_cy + kMarkerHalfH,
                    marker_x + kMarkerW, marker_cy,
                    th.accent, 1.0f);
            }
            r.mb_draw_text(filter_rows[i].label,
                           static_cast<float>(content_x + 18),
                           static_cast<float>(row_y + kOverlayRowHeight - 10),
                           kRowFontPx, label_color);
        }

        // ---- Footer hint inside the panel ----
        // Match the chrome footer's icon vocabulary so the operator
        // sees the same visual language inside the overlay. Six-input
        // row, but constrained to the panel's content_x so it never
        // bleeds outside the gold-framed area.
        const int hint_y = kOverlayPanelBottomY - kOverlayPanelInnerPadY;
        chrome::draw_hint_row(r, content_x, hint_y, {
            {chrome::HintIcon::Btn1Yellow,  "\xE2\x80\x94"},
            // BTN2 never reaches this overlay (owned by main.cpp's exit
            // modal — see the input-handler comment above); label the truth.
            {chrome::HintIcon::Btn2Red,     "Exit"},
            {chrome::HintIcon::Btn3Green,   "\xE2\x80\x94"},
            {chrome::HintIcon::Btn4Black,   "Close"},
            {chrome::HintIcon::RotaryNav,   "Rows"},
            {chrome::HintIcon::RotaryPress, "Apply"},
        });
    }
}

}  // namespace media_browser::ui
