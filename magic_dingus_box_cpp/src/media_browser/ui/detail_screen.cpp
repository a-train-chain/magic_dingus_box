#include "media_browser/ui/detail_screen.h"
#include "media_browser/ui/mb_chrome.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "media_browser/library/watch_store.h"
#include "media_browser/qbittorrent/download_watchdog.h"
#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/radarr/radarr_client.h"
#include "media_browser/tmdb_client.h"
#include "media_browser/ui/mb_ui_utils.h"
#include "ui/text_utf8.h"
#include "ui/renderer.h"
#include "ui/theme.h"
#include "ui/toast.h"

namespace media_browser::ui {

namespace {

// Retro home-menu-inspired layout (target 1280x720). All positions are
// absolute and laid out top-to-bottom around a big poster on the left and
// a metadata column on the right. The visual idiom mirrors the home menu:
//   - "FEATURE PRESENTATION" header in the Zen Dots title font, steel-blue,
//     underlined with a full-width 2px rule (same pattern as "Playlists")
//   - Title in gold (accent) with a 2px gold underline matching its width
//   - Section dividers in steel-blue (accent2)
//   - Gold-outlined chips and buttons (no fill) — matches the home-menu's
//     border-and-text aesthetic instead of filled blocks
//   - Blinking ◂ marker inside the focused action button (same 500ms cycle
//     and color as the playlist-list selection cursor)
// Horizontal safe-area inset. Matches mb_chrome::kSafeInset_px (60) so
// the poster, metadata column, section divider, action button row, and
// footer all stay clear of the 40 px wood-frame overlay drawn around
// every Marquee screen, with 20 px breathing room inside the frame.
// Pre-v1.6.x this was 32, which let content extend under the wood frame.
constexpr float kPaddingX        = 60.0f;     // matches chrome::kSafeInset_px
constexpr float kPaddingY        = 18.0f;

// Top header strip is now drawn by chrome::draw_screen_header — the same
// "title left, sub-info right" treatment Browse / Library / Search /
// Settings use. The legacy kHeaderBaselineY / kHeaderRuleY constants
// were retired in v1.6.x as part of the chrome unification.

// Big poster — slightly smaller than pre-v1.6.x (was 320×480) so the
// whole layout fits between the chrome header (ends at y=120) and the
// chrome footer-hint band (top-edge ~y=646 for 720p) without the action
// row crowding either. 280×420 keeps the same 2:3 aspect.
constexpr float kPosterX         = kPaddingX;
constexpr float kPosterY         = 144.0f;    // chrome header (y=120) + 24 pad
constexpr float kPosterW         = 280.0f;
constexpr float kPosterH         = 420.0f;   // = kPosterW * 1.5 (2:3)
constexpr float kPosterBorderW   = 2.0f;

// Gap between poster and metadata column.
constexpr float kColumnGap       = 24.0f;

// Genre chips.
constexpr float kChipH           = 28.0f;
constexpr float kChipPadX        = 14.0f;
constexpr float kChipGap         = 10.0f;
constexpr float kChipBorderW     = 2.0f;

// Action row.
//   Footer-hint band top edge = h - kFrameInset_px(40) - kPad3(16)
//                              - keyhint_box_h(~18) ≈ h - 74 (= 646 @720p)
//   Buttons end 12 px above that → bottom = h - 86 (= 634)
//   Buttons are 52 px tall → top = h - 138 (= 582 @720p)
//   Section divider sits 12 px above the button row → y = h - 150 (= 570 @720p)
// Constants below are calibrated for 720p (the kiosk's only target).
constexpr float kSectionRuleY    = 570.0f;    // 2px steel-blue divider
constexpr float kActionRowTop    = 582.0f;    // top of buttons
constexpr float kButtonW         = 260.0f;
constexpr float kButtonH         = 52.0f;
constexpr float kButtonGap       = 28.0f;
constexpr float kButtonOutlineW  = 2.0f;
// Right-edge "marker zone" reserved inside each focused button so the
// blinking ◂ cursor never crowds the centered label. The zone exists at
// all times (focused or not) so widths stay stable across focus changes.
constexpr float kButtonMarkerW   = 30.0f;

// Overview wrap is now sized dynamically from the available column space —
// the constant is gone (was kOverviewMaxLines = 4). The right-column flex
// region (synopsis + cast + directors) computes per-block line budgets at
// render time and lets truncate_wrapped add "..." when content actually
// overflows.

// Format runtime as "2h 15m", "95m" (under an hour), "2h" (no minutes), or
// "N/A" when zero/missing.
std::string format_runtime(int minutes) {
    if (minutes <= 0) return "N/A";
    int h = minutes / 60;
    int m = minutes % 60;
    if (h == 0) return std::to_string(m) + "m";
    std::ostringstream os;
    os << h << "h";
    if (m > 0) os << " " << m << "m";
    return os.str();
}

// Format rating as e.g. "7.4" (one decimal). Empty string if unrated.
std::string format_rating(double rating) {
    if (rating <= 0.0) return "";
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", rating);
    return buf;
}

// Compact vote-count formatter: 120 -> "120", 1234 -> "1.2k",
// 15000 -> "15k", 1500000 -> "1M".
std::string format_vote_count(int n) {
    if (n < 1000) return std::to_string(n);
    if (n < 10000) {
        // Two-significant-digit format like "1.2k".
        int whole = n / 1000;
        int tenth = (n % 1000) / 100;
        std::ostringstream os;
        os << whole << "." << tenth << "k";
        return os.str();
    }
    if (n < 1000000) return std::to_string(n / 1000) + "k";
    return std::to_string(n / 1000000) + "M";
}

// Cap an already-wrapped vector of lines at max_lines, appending ellipsis
// to the (now last) line if truncation occurred. Width-aware: tries to
// keep the ellipsis from overflowing the wrap width.
void truncate_wrapped(::ui::Renderer& r, std::vector<std::string>& lines,
                      int font_size, float max_w, int max_lines) {
    if (static_cast<int>(lines.size()) <= max_lines) return;
    lines.resize(max_lines);
    if (lines.empty()) return;
    std::string& last = lines.back();
    while (!last.empty() &&
           r.mb_text_width(last + "...", font_size) > max_w) {
        ::ui::utf8_pop_back(last);
    }
    last += "...";
}

// Join names with " · " (middle dot). Preserves order. Used for cast list.
std::string join_with_bullet(const std::vector<std::string>& items) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += "  \xE2\x80\xA2  ";  // U+2022
        out += items[i];
    }
    return out;
}

}  // namespace

DetailScreen::DetailScreen(RadarrClient& radarr, TmdbClient& tmdb,
                           ProwlarrClient* prowlarr,
                           QbittorrentClient* qbit,
                           library::WatchStore* watch)
    : radarr_(radarr), tmdb_(tmdb), prowlarr_(prowlarr), qbit_(qbit),
      watch_(watch) {
    mode_ = Mode::Loading;
    rebuild_buttons();
}

void DetailScreen::enter() {
    // Clear any transient state from a previous visit.
    remove_pending_ = false;
    banner_.clear();
    focus_ = 0;

    // Skip the refetch when we're re-entering the SAME movie's detail
    // screen with already-loaded data (e.g. user just exited playback
    // and is bouncing back here, or popped Settings). The TMDB call is
    // ~6s over the VPN and frequently times out under load — making the
    // user wait through "Loading..." just to land back on identical
    // content is the worst UX in the app.
    //
    // needs_refresh_ is set ONLY by set_tmdb_id() when the id actually
    // changes (header line 89-94), so this is safe — every legitimate
    // movie-change still triggers a fresh fetch. do_retry() and the
    // post-add path call fetch() directly, bypassing this short-circuit
    // when they explicitly want a reload.
    const bool have_loaded_data = (mode_ != Mode::Loading
                                && mode_ != Mode::Error
                                && mode_ != Mode::NoTmdb);
    if (!needs_refresh_ && have_loaded_data) {
        spdlog::info("[DetailScreen] enter: reusing cached data for tmdb_id={}",
                     tmdb_id_);
        return;
    }

    needs_refresh_ = true;
    mode_ = Mode::Loading;
    fetch();
}

// Non-blocking reaper: join+erase TMDB fetch workers whose per-worker
// done-flag is set (finished). Called at the top of fetch() so the vector
// never grows without bound across a long kiosk uptime. Only reaps
// COMPLETED workers, so each join() is instant — never stalls the render
// thread on a still-running (mid-CURL / stale) worker.
void DetailScreen::reap_finished_workers() {
    // Erase-join only workers whose done-flag is set (finished). A running
    // worker (mid-CURL, possibly a stalled stale one) is left in place —
    // its join would block, which we must never do on the render thread.
    // Completion order isn't push order, so test each entry individually.
    auto it = tmdb_workers_.begin();
    while (it != tmdb_workers_.end()) {
        if (it->done && it->done->load(std::memory_order_acquire)) {
            if (it->thread.joinable()) it->thread.join();  // finished → instant
            it = tmdb_workers_.erase(it);
        } else {
            ++it;
        }
    }
}

void DetailScreen::fetch() {
    // Reap any finished workers before spawning another — bounds the
    // thread vector on a long-lived kiosk (see reap_finished_workers).
    reap_finished_workers();
    // Async dispatch: clear visible state immediately so the user sees
    // the right empty state for the new movie (not stale data from the
    // previous one), set Mode::Loading so render() shows the spinner,
    // bump the generation counter, then spawn a worker. The worker
    // does the slow TMDB + Radarr calls off the render thread; the
    // result is drained by apply_pending_detail() in a future update()
    // tick. The TMDB call alone takes 6+ seconds over the VPN — running
    // it sync froze the kiosk UI for 7-15s every poster tap.
    //
    // (See ProwlarrClient::search_async — it bumps its own generation
    // counter so a stale in-flight Prowlarr search can't pin
    // state==Searching when fetch() runs again. The Prowlarr search is
    // re-kicked from apply_pending_detail() once the TMDB result is in.)
    needs_refresh_ = false;
    movie_.reset();
    tmdb_detail_.reset();
    banner_.clear();

    // Invalidate any in-flight/pending library poll for the OLD movie so
    // it can't publish an InLibraryWithFile flip onto the freshly-Loading
    // new movie. Bumping the generation makes an in-flight worker bail
    // before publishing; clearing the ready flag drops any already-queued
    // result; import_in_progress_ resets the banner sub-state.
    lib_poll_gen_.fetch_add(1);
    lib_poll_ready_.store(false);
    import_in_progress_ = false;

    if (tmdb_id_ == 0) {
        mode_ = Mode::NoTmdb;
        rebuild_buttons();
        return;
    }

    mode_ = Mode::Loading;
    rebuild_buttons();
    const uint64_t my_gen = tmdb_current_gen_.fetch_add(1) + 1;
    spdlog::info("[DetailScreen] fetch: tmdb_id={} (gen={})",
                 tmdb_id_, my_gen);
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread t(&DetailScreen::run_fetch, this, my_gen, tmdb_id_, done);
    tmdb_workers_.push_back(FetchWorker{std::move(t), std::move(done)});
}

namespace {
// RAII: set a worker's done-flag on ANY function exit (the fn has several
// early-return bail paths). The reaper joins only flagged-done threads,
// so this must fire on every path. `done` may be null in unit-test call
// paths that invoke run_fetch directly — guarded.
struct DoneFlag {
    std::shared_ptr<std::atomic<bool>> f;
    ~DoneFlag() { if (f) f->store(true, std::memory_order_release); }
};
}  // namespace

void DetailScreen::run_fetch(uint64_t gen, int tmdb_id,
                             std::shared_ptr<std::atomic<bool>> done) {
    DoneFlag done_flag{std::move(done)};
    // Captured-by-value: do the slow HTTP here. If the user backs out
    // or retries before this returns, tmdb_current_gen_ will bump and
    // our publish will be silently discarded. Each gen-check between
    // calls is an early bail so we don't waste subsequent calls on a
    // result nobody's going to consume.
    DetailFetchResult r;

    // 1) TMDB metadata — the primary source. We used to route through
    // radarr_.lookup() (Radarr's SkyHook proxy to TMDB) but SkyHook on
    // api.radarr.video has flaky transient 503s, which bricked the
    // entire Detail screen even when Radarr itself was healthy.
    r.detail = tmdb_.get_movie(tmdb_id);
    r.detail_ok = r.detail.has_value();
    if (gen != tmdb_current_gen_.load()) {
        spdlog::info("[DetailScreen] gen={} stale after TMDB; discarding",
                     gen);
        return;
    }

    // 2) Radarr library state — best-effort. If Radarr is unreachable
    // we still render the Detail screen with TMDB metadata; only the
    // mutating actions degrade (Add fails with a toast, etc.). The
    // last_error() check matches the original sync path's heuristic:
    // empty library + clean error == legitimately empty, not a failure.
    r.library = radarr_.get_library();
    r.radarr_library_error = radarr_.last_error();
    r.library_ok = r.library.empty() ? r.radarr_library_error.empty() : true;
    if (gen != tmdb_current_gen_.load()) {
        spdlog::info("[DetailScreen] gen={} stale after Radarr library; discarding",
                     gen);
        return;
    }

    // 3) Quality profiles — needed for Add. Cheap; only fetched when
    // library reachability looked OK.
    if (r.library_ok) {
        r.profiles = radarr_.get_quality_profiles();
        r.profiles_ok = true;
    }
    if (gen != tmdb_current_gen_.load()) {
        spdlog::info("[DetailScreen] gen={} stale after Radarr profiles; discarding",
                     gen);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(tmdb_result_mtx_);
        // Recheck under the lock — apply_pending_detail() is the only
        // other reader of tmdb_pending_, and we don't want to clobber a
        // newer worker's published result.
        if (gen != tmdb_current_gen_.load()) return;
        tmdb_pending_ = std::move(r);
    }
    tmdb_result_ready_.store(true);
}

void DetailScreen::apply_pending_detail() {
    if (!tmdb_result_ready_.load()) return;
    DetailFetchResult incoming;
    {
        std::lock_guard<std::mutex> lk(tmdb_result_mtx_);
        incoming = std::move(tmdb_pending_);
    }
    tmdb_result_ready_.store(false);

    // Drain into live state. Order mirrors the original sync fetch so
    // the eventual rebuild_buttons() sees a consistent mode_.
    if (!incoming.detail_ok) {
        // TMDB itself failed — rare. Show an error with Retry.
        mode_ = Mode::Error;
        rebuild_buttons();
        spdlog::info("[DetailScreen] applied pending: TMDB failure");
        return;
    }
    tmdb_detail_ = std::move(incoming.detail);

    const Movie* found = nullptr;
    if (incoming.library_ok) {
        for (const auto& m : incoming.library) {
            if (m.tmdb_id == tmdb_id_) { found = &m; break; }
        }
    }

    if (found) {
        movie_ = *found;
        mode_ = found->has_file ? Mode::InLibraryWithFile
                                : Mode::InLibraryNoFile;
        // Backfill the watchdog with the now-known radarr id. The Add to
        // Library flow registers a watch before Radarr's POST surfaces a
        // movie id; without this upgrade, any subsequent stall event
        // would fall back to opening Detail instead of deep-linking
        // straight to ReleasePicker.
        if (watchdog_ && tmdb_id_ > 0 && found->radarr_id > 0) {
            watchdog_->upgrade_radarr_id(tmdb_id_, found->radarr_id);
        }
        // Seed the quiet-poll clock so the first re-poll is ~9s out (not
        // an immediate redundant Radarr call the same frame). Only matters
        // for the InLibraryNoFile path — maybe_repoll_library() gates on
        // that mode. import_in_progress_ starts clean; the first poll will
        // set it if the queue shows an import underway.
        last_library_poll_at_ = std::chrono::steady_clock::now();
        import_in_progress_ = false;
    } else {
        mode_ = Mode::NotInLibrary;
    }

    if (incoming.profiles_ok) {
        profiles_ = std::move(incoming.profiles);
    }

    if (!incoming.library_ok) {
        show_banner("Radarr service offline — adding to library may fail");
    }

    // Kick off the Prowlarr availability check now that TMDB metadata
    // is in. We skip when the movie is already in library (the queue/
    // library screens cover progress visibility) and when the mode is
    // an error/loading variant (no point searching for a movie we
    // don't have metadata for). The search runs on a separate
    // background thread inside ProwlarrClient; render() polls its
    // result each frame.
    if (prowlarr_ && mode_ == Mode::NotInLibrary && tmdb_detail_.has_value()) {
        const auto& d = *tmdb_detail_;
        prowlarr_->search_async(d.title, d.year);
    }

    rebuild_buttons();
    spdlog::info("[DetailScreen] applied pending: mode={} title='{}'",
                 static_cast<int>(mode_),
                 tmdb_detail_.has_value() ? tmdb_detail_->title : "");
}

// True readiness: Radarr says the movie has a file AND that file maps to
// a host path that actually exists on the mounted SSD. This is the same
// check do_play() enforces at press time, hoisted so rebuild_buttons()
// can gate the Play button on it — closing the "In Library but Play
// fails" gap. Best-effort: a Radarr container-path that doesn't match
// the kiosk's host prefix passes through resolve_host_path unchanged and
// then fails filesystem::exists, which correctly reports not-ready.
bool DetailScreen::play_ready() const {
    if (!movie_.has_value()) return false;
    if (!movie_->has_file) return false;
    if (movie_->file_container_path.empty()) return false;
    const std::string host = radarr_.resolve_host_path(movie_->file_container_path);
    if (host.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(host, ec);
}

// Called each frame from update(). Fires a quiet Radarr-only poll on a
// ~9s cadence while the movie is in-library-without-a-file, so a download
// that finishes + imports while the user watches Detail flips to Play
// without a manual refresh. Gated so it never runs during Loading /
// Error / NotInLibrary, never stacks concurrent polls, and never blocks
// the render thread (the HTTP happens on the worker).
void DetailScreen::maybe_repoll_library() {
    if (mode_ != Mode::InLibraryNoFile) return;
    if (!movie_.has_value() || movie_->radarr_id <= 0) return;
    if (lib_poll_inflight_.load()) return;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - last_library_poll_at_).count();
    if (elapsed < kLibraryPollMs) return;

    last_library_poll_at_ = now;
    lib_poll_inflight_.store(true);
    const uint64_t gen = lib_poll_gen_.fetch_add(1) + 1;
    const int radarr_id = movie_->radarr_id;
    // Reuse the single poll handle. The prior worker is guaranteed
    // finished — lib_poll_inflight_ was false to reach here, and a worker
    // clears inflight only on its LAST action (either the publish or a
    // bail return), so the thread is at/near exit. join() reaps it (near-
    // instant, no render stall) before we reassign.
    if (lib_poll_worker_.joinable()) lib_poll_worker_.join();
    lib_poll_worker_ = std::thread(&DetailScreen::run_library_poll, this,
                                   gen, radarr_id);
}

// Worker body (off the render thread). Re-reads the movie record for the
// current has_file + file path, and scans the queue for an in-progress
// import so the banner can distinguish "importing" from "awaiting
// release". Publishes under lib_poll_mtx_ only if still the current gen.
void DetailScreen::run_library_poll(uint64_t gen, int radarr_id) {
    LibraryPollResult r;
    auto m = radarr_.get_movie(radarr_id);
    if (gen != lib_poll_gen_.load()) { lib_poll_inflight_.store(false); return; }
    if (m.has_value()) {
        r.movie = std::move(m);
        r.ok = true;
    }

    // Best-effort import-state probe: if this movie is still in the queue
    // with tracked_download_state importing/importPending, the file is
    // downloaded and being copied into the library. Failure here is
    // non-fatal — we just fall back to import_active=false (plain
    // MONITORED banner) rather than erroring the poll.
    if (r.ok && r.movie && !r.movie->has_file) {
        for (const auto& qi : radarr_.get_queue()) {
            if (qi.movie_id != radarr_id) continue;
            if (qi.tracked_download_state == "importing" ||
                qi.tracked_download_state == "importPending") {
                r.import_active = true;
            }
            break;
        }
    }
    if (gen != lib_poll_gen_.load()) { lib_poll_inflight_.store(false); return; }

    {
        std::lock_guard<std::mutex> lk(lib_poll_mtx_);
        if (gen != lib_poll_gen_.load()) { lib_poll_inflight_.store(false); return; }
        lib_poll_pending_ = std::move(r);
    }
    lib_poll_ready_.store(true);
    // inflight means ONLY "a worker thread is running" — clear it on the
    // success path too, not just the bail paths. If apply_library_poll()
    // owned this clear instead, a fetch()/navigate that lands between the
    // publish and the drain (which clears lib_poll_ready_) would leave
    // inflight stuck true forever and silently kill all future polling.
    // Re-poll-before-drain is prevented by the 9s cadence timer, not by
    // inflight — a drain happens on the very next frame (~16ms << 9s).
    lib_poll_inflight_.store(false);
}

// Drain a completed poll on the render thread. NEVER sets Mode::Loading
// or touches tmdb_detail_ — that's the whole point (no "Loading" flash
// over on-screen content). Guarded to only act while still InLibraryNoFile
// so a poll that landed after the user navigated away is dropped.
void DetailScreen::apply_library_poll() {
    if (!lib_poll_ready_.load()) return;
    LibraryPollResult r;
    {
        std::lock_guard<std::mutex> lk(lib_poll_mtx_);
        r = std::move(lib_poll_pending_);
    }
    lib_poll_ready_.store(false);
    // NOTE: do NOT clear lib_poll_inflight_ here — the worker owns that
    // flag and clears it on its own exit (success or bail). Clearing it
    // here would double-manage it and reintroduce the stuck-inflight race.

    // Backstop: only apply while awaiting a file. If the user hit Retry or
    // navigated to a different movie (both set Mode::Loading via fetch),
    // this stale result must not clobber the new state.
    if (mode_ != Mode::InLibraryNoFile) return;
    if (!r.ok || !r.movie.has_value()) return;

    if (r.movie->has_file) {
        // The file landed. Swap in the FRESH record (its now-populated
        // file_container_path is required by get_play_target/do_play) and
        // flip to the Play state. Toast so the user knows it's ready even
        // if they glance away.
        movie_ = std::move(r.movie);
        import_in_progress_ = false;
        mode_ = Mode::InLibraryWithFile;
        rebuild_buttons();
        ::ui::Toast::show("Ready to play");
        spdlog::info("[DetailScreen] library poll: file landed for radarr_id={}, "
                     "flipped to InLibraryWithFile", movie_->radarr_id);
    } else {
        // Still no file — refresh the importing sub-state for the banner.
        import_in_progress_ = r.import_active;
    }
}

DetailScreen::~DetailScreen() {
    // Bump gen so any in-flight worker sees its result is stale and
    // bails before publishing. Then join all tracked workers so we
    // don't have a thread holding references to *this after the
    // screen is destroyed. Each worker is bounded by libcurl's per-
    // attempt 25s timeout × up to 3 attempts + ~1.25s backoff
    // (configured in tmdb_client.cpp), so worst-case shutdown wait
    // is ~76s when the network is genuinely unresponsive. In
    // practice (link healthy) workers complete in 1-7s. The longer
    // ceiling is an accepted trade for resilience under transient
    // VPN-egress flakiness.
    tmdb_current_gen_.fetch_add(1);
    // Also invalidate + join the library poll worker (its own single
    // handle, separate from tmdb_workers_).
    lib_poll_gen_.fetch_add(1);
    if (lib_poll_worker_.joinable()) lib_poll_worker_.join();
    // The remove/add workers publish into members — join before they die.
    if (remove_worker_.joinable()) remove_worker_.join();
    if (add_worker_.joinable()) add_worker_.join();
    for (auto& w : tmdb_workers_) {
        if (w.thread.joinable()) w.thread.join();
    }
}

void DetailScreen::rebuild_buttons() {
    buttons_.clear();
    switch (mode_) {
        case Mode::Loading:
        case Mode::NoTmdb:
            // No actions available.
            break;
        case Mode::Error:
            buttons_.push_back({Action::Retry, "Retry"});
            break;
        case Mode::NotInLibrary:
            buttons_.push_back({Action::AddToLibrary, "Add to Library"});
            buttons_.push_back({Action::MoreInfo, "More Info"});
            break;
        case Mode::InLibraryNoFile:
            buttons_.push_back({Action::SearchAgain, "Search Again"});
            // "Pick a source" — gated on the movie being in the library
            // because Radarr's POST /api/v3/release endpoint requires
            // indexerId, which we only get from
            // get_releases_for_movie(radarr_movie_id) — see Task 13. With
            // no radarr_id we have no way to fetch a list of grabbable
            // releases.
            buttons_.push_back({Action::PickSource, "Pick a source"});
            buttons_.push_back(remove_pending_
                               ? Button{Action::ConfirmRemove, "Confirm Remove"}
                               : Button{Action::Remove, "Remove"});
            break;
        case Mode::InLibraryWithFile:
            // Only offer Play when the file is TRULY ready — resolvable +
            // present on the host SSD, not just hasFile=true in Radarr's
            // cache. During the brief import-copy window (or a mount
            // hiccup) hasFile can be true before the file is actually
            // playable; offering Play then lands the user back here with a
            // "File missing" banner. play_ready() is the same predicate
            // do_play() enforces, so the button and the action agree.
            if (play_ready()) {
                buttons_.push_back({Action::Play, "Play"});
            }
            buttons_.push_back({Action::PickSource, "Pick a source"});
            buttons_.push_back(remove_pending_
                               ? Button{Action::ConfirmRemove, "Confirm Remove"}
                               : Button{Action::Remove, "Remove"});
            break;
    }
    if (focus_ < 0) focus_ = 0;
    if (!buttons_.empty() && focus_ >= static_cast<int>(buttons_.size())) {
        focus_ = static_cast<int>(buttons_.size()) - 1;
    }
}

void DetailScreen::update() {
    // Drain a finished async fetch result first so the rest of update()
    // (timer expiry, banner clear) sees the right mode_ for this frame.
    apply_pending_detail();

    // Drain a finished quiet library poll, then maybe kick a new one. Both
    // are cheap atomic-load early-outs on most frames (same shape as
    // apply_pending_detail above). apply first so a just-landed has_file
    // flip is visible this frame; then maybe_repoll re-arms the cadence.
    apply_library_poll();
    maybe_repoll_library();

    auto now = std::chrono::steady_clock::now();
    if (remove_pending_) {
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - remove_pending_at_).count();
        if (elapsed_ms >= kRemovePendingMs) {
            remove_pending_ = false;
            rebuild_buttons();
        }
    }
    if (!banner_.empty()) {
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - banner_at_).count();
        if (elapsed_ms >= kBannerMs) {
            banner_.clear();
        }
    }
}

Screen DetailScreen::handle_input(const std::vector<platform::InputEvent>& events) {
    // Async remove completion — must run before input dispatch so the
    // post-remove navigation/state can never interleave with an action
    // against the removed movie.
    if (Screen s = drain_remove_result(); s != Screen::Detail) {
        return s;
    }
    if (Screen s = drain_add_result(); s != Screen::Detail) {
        return s;
    }
    for (const auto& e : events) {
        // BTN4 (SETTINGS_MENU, black) — short-press returns to the screen
        // that opened this Detail (Browse / Library / Search / Queue) via
        // origin_. This mirrors the role BTN2 played in v1.6.3 — BTN2 now
        // opens the global exit modal instead, so BTN4 takes over "back".
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            return origin_;
        }

        if (e.action == platform::InputAction::ROTATE && e.delta != 0) {
            if (buttons_.empty()) continue;
            int n = static_cast<int>(buttons_.size());
            focus_ = std::clamp(focus_ + e.delta, 0, n - 1);
            // Any navigation cancels a pending remove — so the user can't
            // accidentally hit Enter twice on a different button and blow
            // away the movie.
            if (remove_pending_) {
                remove_pending_ = false;
                rebuild_buttons();
            }
            continue;
        }

        if (e.action == platform::InputAction::SELECT && e.pressed) {
            Screen next = on_activate();
            if (next != Screen::Detail) return next;
            continue;
        }

        // BTN2 (PLAY_PAUSE, red) — intercepted globally by the exit modal
        // in main.cpp. It never reaches here; no per-screen handler needed.
        // "Back" is now BTN4 (SETTINGS_MENU) — see handler above.
    }
    return Screen::Detail;
}

Screen DetailScreen::on_activate() {
    // While a remove runs, every action is a no-op — acting on a movie
    // that is mid-deletion can only produce inconsistent state.
    if (remove_in_flight_.load(std::memory_order_acquire)) {
        show_banner("Removing…");
        return Screen::Detail;
    }
    if (add_in_flight_.load(std::memory_order_acquire)) {
        show_banner("Adding…");
        return Screen::Detail;
    }
    if (buttons_.empty()) return Screen::Detail;
    if (focus_ < 0 || focus_ >= static_cast<int>(buttons_.size())) return Screen::Detail;
    const Action act = buttons_[focus_].action;
    switch (act) {
        case Action::AddToLibrary:   return do_add_to_library();
        case Action::SearchAgain:    return do_search_again();
        case Action::Remove:         return do_remove_stage1();
        case Action::ConfirmRemove:  return do_remove_confirm();
        case Action::Play:           return do_play();
        case Action::Retry:          return do_retry();
        case Action::MoreInfo:       return do_more_info();
        case Action::PickSource:     return do_pick_source();
    }
    return Screen::Detail;
}

int DetailScreen::pick_quality_profile_id() const {
    // Default to "Any" — most permissive profile, accepts whatever the
    // indexer ships. The kiosk has 29 GB of USB storage so disk pressure
    // matters less than getting the movie at all. Power users can switch
    // to HD-1080p / Ultra-HD via Radarr's web UI for movies they want at
    // a specific quality. Without this, popular older / public-domain
    // titles (only available as Bluray-720p on YTS) silently fail to
    // grab even after a successful Add.
    for (const auto& p : profiles_) {
        if (p.name == "Any") return p.id;
    }
    // Fallback search order if "Any" is missing for some reason.
    for (const auto& p : profiles_) {
        if (p.name == "HD - 720p/1080p") return p.id;
    }
    for (const auto& p : profiles_) {
        if (p.name == "HD-1080p") return p.id;
    }
    for (const auto& p : profiles_) {
        if (p.name.find("1080p") != std::string::npos) return p.id;
    }
    if (!profiles_.empty()) return profiles_.front().id;
    return 0;
}

Screen DetailScreen::do_add_to_library() {
    int qp = pick_quality_profile_id();
    if (qp == 0) {
        show_banner("No quality profile available");
        return Screen::Detail;
    }

    // Disk-space pre-flight. The kiosk's library lives on a 29GB-class
    // USB SSD and a typical 1080p Bluray release is 8-20GB. Without
    // this check, the user can queue several large adds simultaneously
    // and silently fill the disk — Radarr's import then fails without
    // surfacing a clear "out of space" signal in the kiosk UI. We
    // probe /mnt/ssd/library (the host-side mount path the kiosk
    // already references via RadarrClient::host_library_prefix) and
    // warn at <15GB free. We don't BLOCK the add — the user might be
    // adding a small WEB-DL release and 10GB is plenty — but we make
    // the situation visible so they can pre-emptively clean up.
    {
        constexpr int64_t kWarnFreeBytes = 15LL * 1024 * 1024 * 1024;  // 15 GB
        std::error_code ec;
        auto info = std::filesystem::space("/mnt/ssd/library", ec);
        if (!ec && info.available > 0) {
            if (static_cast<int64_t>(info.available) < kWarnFreeBytes) {
                int gb_free = static_cast<int>(info.available
                                               / (1024 * 1024 * 1024));
                ::ui::Toast::show(
                    "Warning: only " + std::to_string(gb_free)
                    + " GB free — large releases may fail to import");
            }
        }
        // ec != 0 (mount missing, perms, etc.) is silently ignored.
        // The kiosk doesn't own the mount lifecycle; surfacing every
        // transient stat error would be noisy. Radarr will surface a
        // real error if disk really is full at import time.
    }

    // The add itself runs on a worker: a single Radarr POST, but its 5s
    // timeout froze the render loop — and stacked with any other stall
    // toward the 10s watchdog budget. One add at a time; completion is
    // drained by drain_add_result() at the top of handle_input.
    if (add_in_flight_.load(std::memory_order_acquire)) {
        return Screen::Detail;
    }
    if (add_worker_.joinable()) add_worker_.join();  // finished run
    add_tmdb_id_ = tmdb_id_;
    add_in_flight_.store(true, std::memory_order_release);
    show_banner("Adding…");
    const int tmdb_id = tmdb_id_;
    try {
        add_worker_ = std::thread([this, tmdb_id, qp]() {
            add_ok_ = radarr_.add_movie(tmdb_id, qp, /*monitor=*/true);
            add_done_.store(true, std::memory_order_release);
            add_in_flight_.store(false, std::memory_order_release);
        });
    } catch (const std::system_error&) {
        add_in_flight_.store(false, std::memory_order_release);
        show_banner("Add failed to start; try again");
    }
    return Screen::Detail;
}

Screen DetailScreen::drain_add_result() {
    if (!add_done_.exchange(false, std::memory_order_acq_rel)) {
        return Screen::Detail;
    }
    if (!add_ok_) {
        // Keep the in-screen banner and also surface a top-level toast so
        // the failure is visible outside the action button row context.
        show_banner("Add failed — see Radarr logs");
        ::ui::Toast::show("Add failed — see Radarr logs");
        return Screen::Detail;
    }
    // The user may have opened a different movie while the POST ran —
    // don't register a watch or navigate over the new record.
    if (tmdb_id_ != add_tmdb_id_) {
        ::ui::Toast::show("Added to library — downloading");
        return Screen::Detail;
    }
    // Success: pop a toast confirming the action, refresh the library
    // cache (so Detail shows the new record if the user comes back), and
    // jump to the Queue screen so they see the download start populating
    // in real time.
    ::ui::Toast::show("Added to library — downloading");
    // Register a stall watch so DownloadWatchdog can later surface a
    // "Pick another release" prompt if Radarr's auto-pick fails or
    // the chosen torrent never gets peers. No-op if main.cpp hasn't
    // wired the watchdog (e.g. tests).
    if (watchdog_ && tmdb_id_ > 0) {
        std::string title = tmdb_detail_.has_value()
                              ? tmdb_detail_->title
                              : std::string{};
        // radarr_movie_id may be 0 here: the add_movie() POST to Radarr
        // doesn't return the new id synchronously and we haven't done a
        // re-fetch yet (fetch() below kicks off async, doesn't block).
        // Stale-modal "Pick another" therefore falls back to the Detail
        // route for downloads first registered through Add to Library —
        // the fast path (deep-link straight to the picker) only applies
        // to ReleasePickerScreen-initiated grabs, which already have
        // the radarr id in hand. Acceptable: Add to Library is rarely
        // the path that produces stalls (Radarr's auto-pick usually
        // works for the first attempt).
        const int rid = movie_.has_value() ? movie_->radarr_id : 0;
        watchdog_->watch(tmdb_id_, rid, std::move(title));
    }
    fetch();
    return Screen::Queue;
}

Screen DetailScreen::do_search_again() {
    if (!movie_.has_value()) {
        show_banner("No movie record");
        return Screen::Detail;
    }
    bool ok = radarr_.trigger_search(movie_->radarr_id);
    show_banner(ok ? "Search triggered" : "Search failed");
    return Screen::Detail;
}

Screen DetailScreen::do_remove_stage1() {
    remove_pending_ = true;
    remove_pending_at_ = std::chrono::steady_clock::now();
    rebuild_buttons();
    // Keep focus on the (now re-labeled) confirm button.
    focus_ = static_cast<int>(buttons_.size()) - 1;
    return Screen::Detail;
}

Screen DetailScreen::do_remove_confirm() {
    remove_pending_ = false;
    if (remove_in_flight_.load(std::memory_order_acquire)) {
        return Screen::Detail;  // one remove at a time
    }
    if (!movie_.has_value()) {
        show_banner("No movie record");
        rebuild_buttons();
        return Screen::Detail;
    }

    // The cleanup below chains 3+ sequential HTTP calls (Radarr queue,
    // per-item cancels, history walk, qBit deletes, remove_movie), each
    // with its own timeout. Run on the render thread — as it was until
    // the 2026-07-31 hardening — a hung Radarr blew past WatchdogSec=10
    // and systemd killed the kiosk mid-"Confirm Remove". The worker runs
    // the identical steps; the result is drained at the top of
    // handle_input (called every frame) which applies the state
    // mutations and navigates on the render thread.
    if (remove_worker_.joinable()) remove_worker_.join();  // finished run
    const int radarr_id = movie_->radarr_id;
    remove_radarr_id_ = radarr_id;
    remove_in_flight_.store(true, std::memory_order_release);
    show_banner("Removing…");
    try {
        remove_worker_ = std::thread(&DetailScreen::run_remove, this, radarr_id);
    } catch (const std::system_error&) {
        remove_in_flight_.store(false, std::memory_order_release);
        show_banner("Remove failed to start; try again");
    }
    return Screen::Detail;
}

Screen DetailScreen::drain_remove_result() {
    if (!remove_done_.exchange(false, std::memory_order_acq_rel)) {
        return Screen::Detail;
    }
    if (!remove_ok_) {
        show_banner(remove_error_);
        rebuild_buttons();
        return Screen::Detail;
    }
    // Same-movie guard: the user may have backed out and opened a
    // DIFFERENT movie while the worker ran — never clobber that record's
    // state or yank the user to Library over it.
    if (movie_.has_value() && movie_->radarr_id != remove_radarr_id_) {
        return Screen::Detail;
    }
    // Invalidate our cached movie record + force a refetch on the next
    // entry, exactly as the synchronous flow did (see the comment block
    // in run_remove for why each piece matters).
    movie_.reset();
    needs_refresh_ = true;
    mode_          = Mode::NotInLibrary;
    import_in_progress_ = false;
    lib_poll_gen_.fetch_add(1);
    lib_poll_ready_.store(false);
    rebuild_buttons();
    // After a successful remove, the library view is the natural home.
    return Screen::Library;
}

void DetailScreen::run_remove(int radarr_id) {
    // Four-step cleanup, in this order so each step's prerequisite has
    // happened before it runs:
    //
    //  1. Cancel any in-flight Radarr queue items for this movie. Each
    //     cancel uses removeFromClient=true, which tells Radarr to send
    //     qBittorrent the delete-with-files command. This catches
    //     downloads in progress (state=downloading/queued).
    //
    //  2. Purge any remaining qBit torrents associated with this movie
    //     by walking Radarr's history. Step 1 only covers items in the
    //     active queue — finished+seeding torrents (state=uploading on
    //     qBit, no longer in Radarr's queue) slip through. Without this
    //     step, removing a movie that has already been imported leaves
    //     its torrent seeding forever, pinning disk + upload bandwidth
    //     with no Radarr record to clean it up. We discovered this in
    //     production: a HEVC release we cancelled and re-grabbed left
    //     a 2.58 GB orphan that survived multiple Detail-Remove cycles.
    //
    //  3. Remove the movie record itself from Radarr's library, with
    //     delete_files=true so the imported copy in /library is also
    //     cleaned up. Steps 1-2 cover the qBit-side artifacts; step 3
    //     covers Radarr's side and the host's library disk.
    //
    //  4. Navigate back to the library view (the screen the user
    //     conceptually came from when they decided to remove).
    auto publish = [this](bool ok, std::string err) {
        remove_ok_ = ok;
        remove_error_ = std::move(err);
        remove_done_.store(true, std::memory_order_release);
        remove_in_flight_.store(false, std::memory_order_release);
    };

    int cancelled = 0;
    int cancel_failed = 0;
    auto queue = radarr_.get_queue();
    for (const auto& q : queue) {
        if (q.movie_id == radarr_id) {
            if (radarr_.cancel_queue_item(q.id)) {
                ++cancelled;
            } else {
                ++cancel_failed;
                spdlog::warn(
                    "[detail] failed to cancel queue item {} for movie {}: {}",
                    q.id, radarr_id, radarr_.last_error());
            }
        }
    }
    if (cancelled > 0) {
        spdlog::info("[detail] cancelled {} in-flight queue item(s) "
                     "before removing movie {}", cancelled, radarr_id);
    }

    // If any queue cancel failed, do NOT proceed. Same reasoning as
    // before: orphan-torrent state is worse than a "remove failed"
    // toast. User can fix qBit connectivity and retry.
    if (cancel_failed > 0) {
        publish(false,
                "Cancel failed for " + std::to_string(cancel_failed)
                + " in-flight torrent(s). Movie not removed; "
                  "check qBittorrent connectivity and retry.");
        return;
    }

    // Step 2: history-walk + qBit delete for any historical torrent
    // hashes Radarr remembers for this movie. This is the new path
    // that catches finished+seeding torrents step 1 can't see. We
    // collect every distinct downloadId from grabbed/imported events
    // (typically 1-2 hashes per movie, more if the user re-grabbed)
    // and ask qBit to remove each with deleteFiles=true. qBit's
    // delete is a no-op when the hash isn't present, so this is safe
    // to call even when the cleanup already happened via step 1.
    if (qbit_) {
        auto hashes = radarr_.get_movie_download_hashes(radarr_id);
        int purged = 0;
        for (const auto& h : hashes) {
            if (qbit_->delete_torrent(h, /*delete_files=*/true)) {
                ++purged;
            }
        }
        if (!hashes.empty()) {
            spdlog::info("[detail] purged {} of {} historical qBit "
                         "torrent(s) for movie {}",
                         purged, hashes.size(), radarr_id);
        }
    }

    bool ok = radarr_.remove_movie(radarr_id, /*delete_files=*/true);
    if (!ok) {
        publish(false, "Remove failed: " + radarr_.last_error());
        return;
    }
    // Success. The render-thread state invalidation (movie_.reset(),
    // needs_refresh_, mode_, library-poll gen bump — see the comment in
    // drain_remove_result) is applied by the drain: without it,
    // navigating back into Detail for the same tmdb_id hits the enter()
    // short-circuit and re-renders the stale InLibrary mode for a movie
    // Radarr no longer knows about.
    publish(true, {});
}

DetailScreen::PlayTarget DetailScreen::get_play_target() const {
    PlayTarget pt;
    if (!movie_.has_value()) return pt;
    if (movie_->file_container_path.empty()) return pt;

    pt.host_path = radarr_.resolve_host_path(movie_->file_container_path);
    // Prefer the rich TMDB title if available; fall back to the Radarr title.
    if (tmdb_detail_.has_value() && !tmdb_detail_->title.empty()) {
        pt.title = tmdb_detail_->title;
    } else {
        pt.title = movie_->title;
    }

    // Populate overlay metadata from TMDB detail when available.
    // PlaybackScreen uses this to render the similar-films panel.
    if (tmdb_detail_.has_value()) {
        const auto& d = *tmdb_detail_;
        pt.tmdb_id     = d.tmdb_id;
        pt.year        = d.year;
        pt.runtime_min = d.runtime_minutes;
        pt.synopsis    = d.overview;
        pt.poster_url  = d.poster_path;
        // Format up to 3 genre names joined with " · ".
        for (size_t i = 0; i < d.genres.size() && i < 3; ++i) {
            if (i > 0) pt.genres += " \xC2\xB7 ";  // UTF-8 middle dot
            pt.genres += d.genres[i];
        }
        // Cast: carry top 5 names (overlay truncates further if needed).
        for (size_t i = 0; i < d.cast_top.size() && i < 5; ++i) {
            pt.cast.push_back(d.cast_top[i]);
        }
        // Director: use the first name from the directors list.
        if (!d.directors.empty()) {
            pt.director = d.directors[0];
        }
    } else {
        pt.tmdb_id = tmdb_id_;  // at least carry the id for the prefetch
    }

    // Watch-state handoff (Task 4). Identity is ALWAYS engaged for a
    // playable movie — MediaRef{Movie, tmdb_id_} with season=episode=0 —
    // so the playback session is attributable even when the store is
    // absent (writes then no-op inside main.cpp's degraded WatchStore).
    // Resume point only when the store has a resumable row: past the
    // watched threshold "resume" degrades to "replay from the start".
    pt.watch_identity = WatchIdentity{MediaRef{MediaKind::Movie, tmdb_id_}, 0, 0};
    if (watch_ != nullptr) {
        if (auto row = watch_->movie_watch(tmdb_id_)) {
            if (is_resumable_position(row->position_s, row->duration_s)) {
                pt.resume_position = row->position_s;
            }
        }
    }

    return pt;
}

Screen DetailScreen::do_play() {
    if (!movie_.has_value()) {
        show_banner("No movie record");
        return Screen::Detail;
    }
    auto pt = get_play_target();
    if (pt.host_path.empty()) {
        show_banner("Movie file path unknown");
        return Screen::Detail;
    }
    std::error_code ec;
    if (!std::filesystem::exists(pt.host_path, ec)) {
        show_banner("File missing on disk");
        return Screen::Detail;
    }
    return Screen::Playback;
}

Screen DetailScreen::do_retry() {
    mode_ = Mode::Loading;
    rebuild_buttons();
    fetch();
    return Screen::Detail;
}

Screen DetailScreen::do_more_info() {
    // Placeholder — wires into a future trivia / details sub-screen. For
    // now we just surface a banner so the button feels alive instead of
    // dead. Keeps the user oriented while real content is built.
    show_banner("More Info — coming soon");
    return Screen::Detail;
}

Screen DetailScreen::do_pick_source() {
    // Open the manual release picker so the user can override Radarr's
    // auto-pick. The picker now handles its own async fetch (worker
    // thread + state machine) — this method just forwards keys via
    // the callback and triggers the screen transition. Was synchronous
    // in v1.7.x and blocked the UI for 14-25s; see commit log + the
    // ReleasePickerScreen async-load comment block for the redesign.
    //
    // Picker is gated on the movie being in the library because
    // Radarr's POST /api/v3/release endpoint requires indexerId, which
    // only comes back from /api/v3/release?movieId=X — so without a
    // radarr_id we have nothing to feed the picker's load_async().
    if (!movie_.has_value() || movie_->radarr_id <= 0) {
        ::ui::Toast::show("Add to library first to pick a source");
        return Screen::Detail;
    }
    if (!picker_callback_) {
        // Defensive: if main.cpp didn't wire the callback (shouldn't
        // happen in production), surface a banner instead of silently
        // landing on an empty picker screen.
        show_banner("Picker not wired — internal error");
        return Screen::Detail;
    }

    std::string movie_title = tmdb_detail_.has_value()
                                ? tmdb_detail_->title
                                : movie_->title;
    // Fire-and-forget: callback kicks off the picker's worker thread
    // and the dispatcher transitions us to Screen::ReleasePicker on
    // return. The picker shows its Loading state immediately; rows
    // populate when the worker drains a few frames later.
    picker_callback_(tmdb_id_, movie_->radarr_id, std::move(movie_title));
    return Screen::ReleasePicker;
}

void DetailScreen::show_banner(std::string text) {
    banner_ = std::move(text);
    banner_at_ = std::chrono::steady_clock::now();
}

// ----------------------------------------------------------------------------
// Rendering
// ----------------------------------------------------------------------------

void DetailScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    const ::ui::Theme& th = r.mb_theme();
    r.mb_fill_background();

    const float w = static_cast<float>(screen_w);
    const float h = static_cast<float>(screen_h);

    // 500ms blink cycle, sourced from epoch time so it stays in lockstep
    // with the home-menu's playlist cursor — both blinks visually breathe
    // together when transitioning between screens.
    auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    const bool blink_on = (epoch_ms / 500) % 2 == 0;

    // --- Centered single-message states ------------------------------
    if (mode_ == Mode::Loading) {
        int sz = th.font_large_size;
        std::string msg = "Loading...";
        int mw = r.mb_text_width(msg, sz);
        float x = (w - static_cast<float>(mw)) / 2.0f;
        float y = (h / 2.0f) + static_cast<float>(r.mb_text_baseline(sz));
        r.mb_draw_text(msg, x, y, sz, th.fg, 0.9f);
        return;
    }
    if (mode_ == Mode::NoTmdb) {
        int sz = th.font_large_size;
        std::string msg = "No movie selected";
        int mw = r.mb_text_width(msg, sz);
        float x = (w - static_cast<float>(mw)) / 2.0f;
        float y = (h / 2.0f) + static_cast<float>(r.mb_text_baseline(sz));
        r.mb_draw_text(msg, x, y, sz, th.dim, 0.9f);
        return;
    }

    // --- Pull metadata once (Error state may have nullopt detail) ----
    std::string title, tagline, overview, language;
    int year = 0, runtime = 0, vote_count = 0;
    double rating = 0.0;
    std::vector<std::string> genres, cast_top, directors;
    std::string poster_url;
    if (tmdb_detail_.has_value()) {
        title       = tmdb_detail_->title;
        tagline     = tmdb_detail_->tagline;
        overview    = tmdb_detail_->overview;
        language    = tmdb_detail_->original_language;
        year        = tmdb_detail_->year;
        runtime     = tmdb_detail_->runtime_minutes;
        rating      = tmdb_detail_->rating;
        vote_count  = tmdb_detail_->vote_count;
        genres      = tmdb_detail_->genres;
        cast_top    = tmdb_detail_->cast_top;
        directors   = tmdb_detail_->directors;
        poster_url  = tmdb_detail_->poster_path;
    }
    if (title.empty() && movie_.has_value()) {
        title    = movie_->title;
        year     = movie_->year;
        rating   = movie_->rating;
        runtime  = movie_->runtime_minutes;
        overview = movie_->overview;
        if (poster_url.empty()) poster_url = movie_->poster_url;
    }
    if (title.empty()) title = "Untitled";

    // --- Top header bar (Marquee chrome) -----------------------------
    // "Feature Presentation" title (left, ZenDots) + sub-info on the
    // right showing the year + a back-hint, drawn via the same
    // chrome::draw_screen_header helper Browse / Library / Search /
    // Settings use. No tabs — Detail is a drill-down, not part of the
    // main strip — so we pass an empty tab vector. The chrome header
    // owns its own y-coords (baseline ~y=78, returns body_top_y=120),
    // matching every other Marquee screen pixel-for-pixel.
    //
    // A 2 px steel-blue rule sits directly underneath at y = 120, the
    // same idiom PlaybackScreen uses for its "NOW PLAYING — <title>"
    // banner. Reads as a magazine-style header underline, anchoring
    // the title without competing with the body content the way the
    // old action-row divider did.
    {
        namespace chrome = ::media_browser::ui::chrome;
        // Build a compact sub-info: "1999 · BTN4 back" (or just back-hint
        // if year is missing). Fits in the right side of the header band
        // in 14 px dim mono — same treatment Queue uses for its sub-info.
        std::string sub_info;
        if (year > 0) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d  ·  BTN4 back", year);
            sub_info = buf;
        } else {
            sub_info = "BTN4 back";
        }
        const int header_bottom = chrome::draw_screen_header(
            r, screen_w, "Feature Presentation",
            /*tabs=*/{}, /*focused_tab=*/-1,
            sub_info);
        // Header underline — full-width inside the safe area, 2 px
        // steel-blue at high alpha (matches PlaybackScreen's title rule).
        const float rule_y = static_cast<float>(header_bottom);
        r.mb_draw_line(kPaddingX, rule_y, w - kPaddingX, rule_y,
                       2.0f, th.dim, 0.95f);
    }

    // --- Big poster card (left column) -------------------------------
    {
        ::ui::Color tint = stable_tint_for_id(tmdb_id_);
        r.mb_draw_poster_fit(poster_url,
                             kPosterX, kPosterY,
                             kPosterW, kPosterH,
                             tint, 1.0f);
        // 2px gold border = TV-monitor frame. Matches the home-menu's
        // bordered-overlay style (volume slider, virtual keyboard).
        r.mb_stroke_rect(kPosterX, kPosterY, kPosterW, kPosterH,
                         kPosterBorderW, th.accent, 0.95f);
    }

    // --- Right column: title / meta / chips / tagline / overview ----
    const float col_x = kPosterX + kPosterW + kColumnGap;
    const float col_w = w - col_x - kPaddingX;
    float cursor_y = kPosterY;  // Start the metadata column flush with poster top.

    // Title — Zen Dots, gold (accent), with a 2px gold underline beneath
    // matching the title text width (same idiom as the home menu's product
    // title underline).
    {
        int title_size = th.font_title_size;
        int title_baseline = r.mb_title_text_baseline(title_size);
        std::string title_drawn = title;
        // Use the title font for measuring the truncation bound.
        if (r.mb_title_text_width(title_drawn, title_size)
                > static_cast<int>(col_w)) {
            // Body-font measurement is good enough for the truncation loop —
            // both fonts share roughly the same em width at the same px size,
            // and the overflow check is conservative.
            title_drawn = truncate_to_width(r, title, title_size, col_w);
        }
        cursor_y += static_cast<float>(title_baseline);
        r.mb_draw_title_text(title_drawn, col_x, cursor_y,
                             title_size, th.accent, 1.0f);
        // Gold rule under the title — width matches the actual drawn text.
        int title_w_drawn = r.mb_title_text_width(title_drawn, title_size);
        float underline_y = cursor_y + 8.0f;
        r.mb_draw_line(col_x, underline_y,
                       col_x + static_cast<float>(title_w_drawn), underline_y,
                       2.0f, th.accent, 0.95f);
        cursor_y = underline_y + 14.0f;
    }

    // Meta line: "1999 · 2h 16m · EN"  +  "★ 8.2/10" + "(25k votes)"
    {
        int meta_size = th.font_medium_size;
        int meta_baseline = r.mb_text_baseline(meta_size);

        std::ostringstream meta_os;
        bool first = true;
        if (year > 0) {
            meta_os << year;
            first = false;
        }
        std::string runtime_str = format_runtime(runtime);
        if (runtime_str != "N/A") {
            if (!first) meta_os << "  \xE2\x80\xA2  ";
            meta_os << runtime_str;
            first = false;
        }
        if (!language.empty()) {
            if (!first) meta_os << "  \xE2\x80\xA2  ";
            // Capitalize the 2-letter language code for retro flair (en → EN).
            std::string lang_upper = language;
            for (auto& c : lang_upper) {
                if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
            }
            meta_os << lang_upper;
        }
        std::string meta = meta_os.str();

        cursor_y += static_cast<float>(meta_baseline);
        if (!meta.empty()) {
            r.mb_draw_text(meta, col_x, cursor_y,
                           meta_size, th.fg, 0.92f);
        }

        std::string rating_str = format_rating(rating);
        if (!rating_str.empty()) {
            std::string rating_text = rating_str + "/10";
            int rw = r.mb_text_width(rating_text, meta_size);
            const float star_r   = static_cast<float>(meta_size) * 0.40f;
            const float star_gap = 6.0f;
            float text_x = col_x + col_w - static_cast<float>(rw);
            float star_cx = text_x - star_gap - star_r;
            float star_cy = cursor_y - static_cast<float>(meta_size) * 0.33f;
            r.mb_fill_star(star_cx, star_cy, star_r, th.accent, 1.0f);
            r.mb_draw_text(rating_text, text_x, cursor_y,
                           meta_size, th.accent, 1.0f);

            if (vote_count > 0) {
                int sm = th.font_small_size;
                int sm_baseline = r.mb_text_baseline(sm);
                std::string votes = "(" + format_vote_count(vote_count) + " votes)";
                int vw = r.mb_text_width(votes, sm);
                float vx = col_x + col_w - static_cast<float>(vw);
                float vy = cursor_y - static_cast<float>(meta_baseline)
                         + static_cast<float>(meta_size) + 4.0f
                         + static_cast<float>(sm_baseline);
                r.mb_draw_text(votes, vx, vy, sm, th.dim, 0.85f);
            }
        }
        // Reserve vertical space for the meta row + (optional) votes line.
        cursor_y += static_cast<float>(meta_size) * 0.5f;
        if (vote_count > 0 && !rating_str.empty()) {
            cursor_y += static_cast<float>(th.font_small_size) + 4.0f;
        }
    }

    // AVAILABILITY readout — shown only on the Add path. Tells the user
    // up front whether sources exist before they spend a slot in their
    // library on something with no seeders. Three visual states:
    //   - Searching:    dim "Checking sources..." (italic feel via dim)
    //   - No sources:   red "No sources found" + sub-line explaining
    //                   Radarr will keep watching once added.
    //   - Sources:      green "X seeders available across N releases"
    // We never show this for in-library movies (the awaiting-release
    // banner below covers that case) or while loading TMDB metadata.
    if (mode_ == Mode::NotInLibrary && prowlarr_) {
        int sz = th.font_small_size;
        int baseline = r.mb_text_baseline(sz);
        cursor_y += 12.0f;
        const float banner_h = static_cast<float>(sz)
                             + 2.0f * ::ui::overlay::kBannerPadY;

        std::string label = "AVAILABILITY";
        ::ui::Color border_col = th.dim;
        ::ui::Color text_col   = th.dim;
        std::string body;
        float text_alpha = 0.85f;

        switch (prowlarr_->state()) {
            case ProwlarrClient::State::Idle:
            case ProwlarrClient::State::Searching: {
                body = "Checking sources...";
                border_col = th.dim;
                text_col   = th.dim;
                break;
            }
            case ProwlarrClient::State::Failed: {
                body = "Sources unavailable: " + prowlarr_->peek_error();
                border_col = th.highlight2;
                text_col   = th.highlight2;
                text_alpha = 0.95f;
                break;
            }
            case ProwlarrClient::State::Ready: {
                auto sum = prowlarr_->peek_result();
                if (!sum || sum->total_releases == 0) {
                    body = "No sources found  \xE2\x80\xA2  "
                           "Add anyway and Radarr will keep watching";
                    border_col = th.highlight2;
                    text_col   = th.highlight2;
                    text_alpha = 0.95f;
                } else {
                    char buf[160];
                    snprintf(buf, sizeof(buf),
                             "%d seeders (best)  \xE2\x80\xA2  "
                             "%d releases  \xE2\x80\xA2  "
                             "%d total seeders",
                             sum->best_seeders,
                             sum->total_releases,
                             sum->total_seeders);
                    body = buf;
                    border_col = th.highlight1;  // green
                    text_col   = th.highlight1;
                    text_alpha = 0.95f;
                }
                break;
            }
        }

        r.mb_stroke_rect(col_x, cursor_y, col_w, banner_h,
                         2.0f, border_col, 0.9f);
        // Label on the left in dim cream — same pattern Detail uses for
        // small-caps section labels.
        std::string drawn = truncate_to_width(r, label + "  " + body,
                                              sz, col_w - 2.0f * ::ui::overlay::kBannerPadX);
        r.mb_draw_text(drawn, col_x + static_cast<float>(::ui::overlay::kBannerPadX),
                       cursor_y + (banner_h - static_cast<float>(sz)) / 2.0f
                                + static_cast<float>(baseline),
                       sz, text_col, text_alpha);
        cursor_y += banner_h + 6.0f;
    }

    // VPN-down banner — surfaced on every Mode that involves a download
    // path (NotInLibrary = Add, InLibraryNoFile = re-search). When the
    // tunnel is unhealthy, Radarr will still accept Add/Search but qBit
    // can't reach peers, so torrents will sit at 0% indefinitely.
    // Telling the user up front avoids the "I added it 10 minutes ago,
    // why is nothing happening?" puzzle.
    if (vpn_healthy_provider_ && !vpn_healthy_provider_() &&
        (mode_ == Mode::NotInLibrary || mode_ == Mode::InLibraryNoFile)) {
        int sz = th.font_small_size;
        int baseline = r.mb_text_baseline(sz);
        cursor_y += 12.0f;
        const float banner_h = static_cast<float>(sz)
                             + 2.0f * ::ui::overlay::kBannerPadY;
        r.mb_stroke_rect(col_x, cursor_y, col_w, banner_h,
                         2.0f, th.highlight2, 0.95f);
        std::string txt =
            "VPN TUNNEL DOWN  \xE2\x80\xA2  Adds will queue but torrents "
            "won't transfer until the tunnel comes back";
        std::string drawn = truncate_to_width(r, txt, sz, col_w - 2.0f * ::ui::overlay::kBannerPadX);
        r.mb_draw_text(drawn, col_x + static_cast<float>(::ui::overlay::kBannerPadX),
                       cursor_y + (banner_h - static_cast<float>(sz)) / 2.0f
                                + static_cast<float>(baseline),
                       sz, th.highlight2, 0.95f);
        cursor_y += banner_h + 6.0f;
    }

    // Show the "Monitored — awaiting release" banner when the movie's in
    // the user's library but Radarr hasn't grabbed a release yet. Tells
    // the user the system is working in the background and they don't
    // need to do anything — Radarr re-checks every ~30 minutes and will
    // auto-download when seeders appear.
    if (mode_ == Mode::InLibraryNoFile) {
        int sz = th.font_small_size;
        int baseline = r.mb_text_baseline(sz);
        cursor_y += 12.0f;
        const float banner_h = static_cast<float>(sz)
                             + 2.0f * ::ui::overlay::kBannerPadY;
        // Three sub-states share this slot:
        //   import_in_progress_ : the download FINISHED and Radarr is
        //     copying the file into the library right now — ready in
        //     seconds. Green (highlight1) so it reads as imminent success.
        //   pre-release (Radarr status tba/announced/inCinemas) : no home
        //     release exists yet, so anything indexers surface right now
        //     is a scam upload. Red/orange warning so the user doesn't
        //     chase fake grabs (observed live: "The Odyssey 2026" fakes
        //     the day after theatrical premiere).
        //   else (plain monitored) : no release grabbed yet; Radarr is
        //     still watching indexers. Dim steel-blue, informational.
        // The quiet library poll (maybe_repoll_library) drives the flip
        // and, when the file lands, transitions the whole screen to
        // InLibraryWithFile so Play appears — this banner is only the
        // interim signal.
        const bool prerelease =
            movie_.has_value() && likely_prerelease_fakes_only(*movie_);
        ::ui::Color state_col = import_in_progress_ ? th.highlight1
                              : prerelease          ? th.highlight2
                                                    : th.dim;
        std::string txt = import_in_progress_
            ? std::string("DOWNLOADED  \xE2\x80\xA2  Importing to library — "
                          "ready to play in a few seconds")
            : prerelease
            ? std::string("IN THEATERS  \xE2\x80\xA2  No digital release "
                          "exists yet — downloads found now are almost "
                          "always fakes; the real one lands automatically")
            : std::string("MONITORED  \xE2\x80\xA2  Radarr re-checks indexers "
                          "every 30 minutes and will auto-download when "
                          "seeders appear");
        r.mb_stroke_rect(col_x, cursor_y, col_w, banner_h,
                         2.0f, state_col, 0.9f);
        std::string drawn = truncate_to_width(r, txt, sz, col_w - 2.0f * ::ui::overlay::kBannerPadX);
        r.mb_draw_text(drawn, col_x + static_cast<float>(::ui::overlay::kBannerPadX),
                       cursor_y + (banner_h - static_cast<float>(sz)) / 2.0f
                                + static_cast<float>(baseline),
                       sz, state_col, 0.95f);
        cursor_y += banner_h + 6.0f;
    }

    // Runtime-mismatch warning: the imported file's measured duration is
    // wildly off the movie's expected runtime — a renamed trailer/junk
    // video wearing a legit release name (the fake-upload long tail that
    // slips past pre-grab scoring). Tells the user what to do about it.
    if (mode_ == Mode::InLibraryWithFile && movie_.has_value() &&
        file_runtime_suspicious(*movie_)) {
        int sz = th.font_small_size;
        int baseline = r.mb_text_baseline(sz);
        cursor_y += 12.0f;
        const float banner_h = static_cast<float>(sz)
                             + 2.0f * ::ui::overlay::kBannerPadY;
        std::string txt = "FILE LOOKS WRONG  \xE2\x80\xA2  " +
            std::to_string(movie_->file_runtime_minutes) + " min file vs " +
            std::to_string(movie_->runtime_minutes) + " min expected — "
            "probably not the real movie (use Remove, then re-add)";
        r.mb_stroke_rect(col_x, cursor_y, col_w, banner_h,
                         2.0f, th.highlight2, 0.9f);
        std::string drawn = truncate_to_width(r, txt, sz, col_w - 2.0f * ::ui::overlay::kBannerPadX);
        r.mb_draw_text(drawn, col_x + static_cast<float>(::ui::overlay::kBannerPadX),
                       cursor_y + (banner_h - static_cast<float>(sz)) / 2.0f
                                + static_cast<float>(baseline),
                       sz, th.highlight2, 0.95f);
        cursor_y += banner_h + 6.0f;
    }

    // Genre chips — gold outline, no fill, accent text. Pure border-and-text
    // styling that mirrors the home menu's outlined overlays.
    if (!genres.empty()) {
        int chip_size = th.font_small_size;
        int chip_baseline = r.mb_text_baseline(chip_size);
        cursor_y += 12.0f;
        float chip_y = cursor_y;
        float chip_x = col_x;
        for (const auto& g : genres) {
            int tw = r.mb_text_width(g, chip_size);
            float chip_w = static_cast<float>(tw) + 2.0f * kChipPadX;
            if (chip_x + chip_w > col_x + col_w) {
                chip_x = col_x;
                chip_y += kChipH + kChipGap;
            }
            r.mb_stroke_rect(chip_x, chip_y, chip_w, kChipH,
                             kChipBorderW, th.accent, 0.95f);
            float tx = chip_x + kChipPadX;
            float ty = chip_y + (kChipH - static_cast<float>(chip_size)) / 2.0f
                     + static_cast<float>(chip_baseline);
            r.mb_draw_text(g, tx, ty, chip_size, th.accent, 1.0f);
            chip_x += chip_w + kChipGap;
        }
        cursor_y = chip_y + kChipH;
    }

    // Tagline — smart-quoted, dim, smaller. Reads as a movie marquee blurb.
    if (!tagline.empty()) {
        int tg_size = th.font_medium_size;
        int tg_baseline = r.mb_text_baseline(tg_size);
        cursor_y += 16.0f + static_cast<float>(tg_baseline);
        std::string tagline_q = std::string("\xE2\x80\x9C")
                              + tagline + "\xE2\x80\x9D";
        std::string drawn = truncate_to_width(r, tagline_q, tg_size, col_w);
        r.mb_draw_text(drawn, col_x, cursor_y, tg_size, th.dim, 0.95f);
        cursor_y += static_cast<float>(tg_size) * 0.45f;
    }

    // --- Flex content: synopsis / cast / directors --------------------
    //
    // We have a fixed window from the current cursor_y down to just above
    // the section divider at kSectionRuleY. Three blocks compete for that
    // space: the overview synopsis, the CAST list, and the DIRECTED BY
    // list. The user wants ALL of them visible, wrapping over multiple
    // lines if needed, with "..." truncation only triggering when the
    // total content genuinely overflows.
    //
    // Allocation strategy: synopsis is the biggest block, so it gets to
    // expand into whatever space remains AFTER reserving a small minimum
    // (label + 1 wrapped line) for each cast/directors block that has
    // content. Cast then takes everything left over minus a min reserve
    // for directors. Directors gets the final remainder. Each block's
    // wrapped lines are capped at the computed line budget; truncate_wrapped
    // adds the "..." when the natural wrap exceeds that cap.
    const float content_bottom    = kSectionRuleY - 16.0f;
    const float kSectionTopPad    = 14.0f;
    const float line_h_medium     = static_cast<float>(th.font_medium_size) * 1.4f;
    // Approximate vertical cost of a small-caps label above a body block:
    // baseline pad + label glyph height + bottom pad before the body line.
    const float label_block_h     = static_cast<float>(th.font_small_size) * 1.6f;
    const float min_section_h     = kSectionTopPad + label_block_h + line_h_medium;

    // Lay out a single wrapped-text block (with optional small-caps label
    // above it). Returns the number of lines drawn. Updates cursor_y.
    auto lay_out_block = [&](const std::string& label,
                             const std::string& body,
                             int body_font_size,
                             ::ui::Color body_color,
                             float top_pad,
                             int max_lines) -> int {
        if (body.empty() || max_lines <= 0) return 0;
        int body_baseline = r.mb_text_baseline(body_font_size);
        float line_h = static_cast<float>(body_font_size) * 1.4f;

        auto lines = chrome::wrap_text(r, body, body_font_size, col_w);
        truncate_wrapped(r, lines, body_font_size, col_w, max_lines);
        if (lines.empty()) return 0;

        cursor_y += top_pad;
        if (!label.empty()) {
            int label_size = th.font_small_size;
            int label_baseline = r.mb_text_baseline(label_size);
            cursor_y += static_cast<float>(label_baseline);
            r.mb_draw_text(label, col_x, cursor_y,
                           label_size, th.dim, 0.95f);
            cursor_y += static_cast<float>(label_size) * 0.6f;
        }
        cursor_y += static_cast<float>(body_baseline);
        for (size_t i = 0; i < lines.size(); ++i) {
            r.mb_draw_text(lines[i], col_x,
                           cursor_y + static_cast<float>(i) * line_h,
                           body_font_size, body_color, 0.95f);
        }
        cursor_y += line_h * static_cast<float>(lines.size())
                  - static_cast<float>(body_baseline);
        return static_cast<int>(lines.size());
    };

    // Synopsis allocation: total flex space minus reserved minimums for
    // any cast / directors block that has data, minus its own top pad.
    if (mode_ != Mode::Error && !overview.empty()) {
        float space = content_bottom - cursor_y;
        if (!cast_top.empty())  space -= min_section_h;
        if (!directors.empty()) space -= min_section_h;
        space -= kSectionTopPad;  // synopsis's own top pad
        int max_lines = std::max(1,
            static_cast<int>(space / line_h_medium));
        lay_out_block("", overview, th.font_medium_size, th.fg,
                      kSectionTopPad, max_lines);
    } else if (mode_ != Mode::Error) {
        int sz = th.font_medium_size;
        int sz_baseline = r.mb_text_baseline(sz);
        cursor_y += kSectionTopPad + static_cast<float>(sz_baseline);
        r.mb_draw_text("No synopsis available.", col_x, cursor_y,
                       sz, th.dim, 0.7f);
    }

    if (mode_ == Mode::Error) {
        int sz = th.font_large_size;
        int sz_baseline = r.mb_text_baseline(sz);
        // A keyless box reaches this screen through Search (which goes
        // via Radarr's SkyHook proxy and works without a TMDB key), then
        // fails here because get_movie() is TMDB-direct. "Couldn't
        // fetch" reads as a network or service fault, so name the real
        // cause when there's simply no key.
        std::string msg = tmdb_.has_api_key()
            ? "Couldn't fetch movie info from TMDB."
            : "No TMDB key — add one in the Content Manager, "
              "Media Browser tab.";
        cursor_y += kSectionTopPad + static_cast<float>(sz_baseline);
        r.mb_draw_text(msg, col_x, cursor_y,
                       sz, th.highlight2, 0.95f);
    }

    // CAST: take whatever space is left, minus a min reserve for
    // directors if it'll be drawn. Wraps to as many lines as fit.
    if (!cast_top.empty()) {
        float space = content_bottom - cursor_y;
        if (!directors.empty()) space -= min_section_h;
        space -= kSectionTopPad + label_block_h;  // own header overhead
        int max_lines = std::max(1,
            static_cast<int>(space / line_h_medium));
        lay_out_block("CAST", join_with_bullet(cast_top),
                      th.font_medium_size, th.fg,
                      kSectionTopPad, max_lines);
    }

    // DIRECTED BY: claims the final remainder.
    if (!directors.empty()) {
        std::string label = directors.size() == 1 ? "DIRECTED BY" : "DIRECTORS";
        float space = content_bottom - cursor_y;
        space -= kSectionTopPad + label_block_h;
        int max_lines = std::max(1,
            static_cast<int>(space / line_h_medium));
        lay_out_block(label, join_with_bullet(directors),
                      th.font_medium_size, th.fg,
                      kSectionTopPad, max_lines);
    }

    // --- Section divider above action row ----------------------------
    // Retired in v1.6.x — the steel-blue rule moved up to sit directly
    // under the "Feature Presentation" chrome header (matching
    // PlaybackScreen's title-underline idiom). The action row no longer
    // needs a separator from the body since the buttons themselves
    // (color-coded bordered chrome::draw_button() calls) read as their
    // own visual zone. kSectionRuleY is kept as a logical anchor for
    // the content_bottom calculation in the body block above.

    // --- Action button row (Marquee design) --------------------------
    // Color-coded bordered buttons via chrome::draw_button():
    //   Play / Add to Library  → Ok (green)
    //   Search Releases / Search Again / Retry → Action (steel blue)
    //   Remove / Confirm Remove → Warn (red)
    //   More Info → Neutral (dim)
    // Focused button gets the standard 2 px gold focus ring at +2 px
    // offset (same as poster cells, settings rows, etc.).
    if (!buttons_.empty()) {
        namespace chrome = ::media_browser::ui::chrome;

        // Pre-measure each button's width so we can right-align or
        // center the row. chrome::draw_button measures internally; here
        // we approximate with mb_text_width + the same padding.
        constexpr int kBtnFontPx  = 18;
        constexpr int kBtnPadX    = 18;
        constexpr int kBtnGap     = 16;
        std::vector<int> widths;
        widths.reserve(buttons_.size());
        int total_w = 0;
        for (size_t i = 0; i < buttons_.size(); ++i) {
            const int tw = r.mb_text_width(buttons_[i].label, kBtnFontPx);
            widths.push_back(tw + 2 * kBtnPadX);
            total_w += widths.back();
        }
        total_w += static_cast<int>(buttons_.size() - 1) * kBtnGap;

        int x_cursor = static_cast<int>((w - static_cast<float>(total_w)) / 2.0f);
        const int row_y = static_cast<int>(kActionRowTop);

        for (size_t i = 0; i < buttons_.size(); ++i) {
            const auto& btn = buttons_[i];
            chrome::ButtonKind kind = chrome::ButtonKind::Neutral;
            switch (btn.action) {
                case Action::Play:
                case Action::AddToLibrary:
                    kind = chrome::ButtonKind::Ok;
                    break;
                case Action::SearchAgain:
                case Action::Retry:
                case Action::PickSource:
                    kind = chrome::ButtonKind::Action;
                    break;
                case Action::Remove:
                case Action::ConfirmRemove:
                    kind = chrome::ButtonKind::Warn;
                    break;
                case Action::MoreInfo:
                default:
                    kind = chrome::ButtonKind::Neutral;
                    break;
            }
            const bool focused = (static_cast<int>(i) == focus_);
            chrome::draw_button(r, x_cursor, row_y, btn.label, kind, focused);
            x_cursor += widths[i] + kBtnGap;
        }
    }

    // --- Bottom hint -------------------------------------------------
    // Marquee design: bordered-key glyphs + dim labels, drawn via the
    // shared mb_chrome helper so all Marquee screens share the same
    // footer style. Replaces the previous centered text hint.
    {
        namespace mc = ::media_browser::ui::chrome;
        mc::draw_footer_hints(r, screen_w, screen_h, {
            {mc::HintIcon::Btn1Yellow,  "\xE2\x80\x94"},
            {mc::HintIcon::Btn2Red,     "Exit"},
            {mc::HintIcon::Btn3Green,   "\xE2\x80\x94"},
            {mc::HintIcon::Btn4Black,   "Back"},
            {mc::HintIcon::RotaryNav,   "Action"},
            {mc::HintIcon::RotaryPress, "Confirm"},
        });
    }

    // --- Transient banner --------------------------------------------
    // Insets from the shared overlay contract (padding audit 2026-08-09:
    // the old 16/8 pads left the text 8 px off the top/bottom borders,
    // and this banner and MBSettingsScreen's copy of it had drifted to
    // different pad values — both now draw from ui::overlay).
    if (!banner_.empty()) {
        namespace ov = ::ui::overlay;
        int sz = th.font_medium_size;
        int baseline = r.mb_text_baseline(sz);
        const float max_text_w =
            w - 2.0f * (ov::kScreenEdgeMargin + ov::kBannerPadX);
        const std::string drawn = truncate_to_width(r, banner_, sz, max_text_w);
        int bw = r.mb_text_width(drawn, sz);
        float box_w = static_cast<float>(bw) + 2.0f * ov::kBannerPadX;
        float box_h = static_cast<float>(sz) + 2.0f * ov::kBannerPadY;
        float box_x = (w - box_w) / 2.0f;
        float box_y = kSectionRuleY - box_h - 12.0f;
        // Clamp upper bound to just below the chrome header band
        // (header ends at y = kSafeInset + kHeaderHeight = 120) so the
        // banner never overlaps the title strip.
        constexpr float kBannerMinY = 128.0f;
        if (box_y < kBannerMinY) box_y = kBannerMinY;
        r.mb_fill_rect(box_x, box_y, box_w, box_h, th.bg, 0.92f);
        r.mb_stroke_rect(box_x, box_y, box_w, box_h, 2.0f, th.accent, 1.0f);
        float tx = box_x + static_cast<float>(ov::kBannerPadX);
        float ty = box_y + (box_h / 2.0f) - static_cast<float>(sz) / 2.0f
                 + static_cast<float>(baseline);
        r.mb_draw_text(drawn, tx, ty, sz, th.fg, 1.0f);
    }
}

}  // namespace media_browser::ui
