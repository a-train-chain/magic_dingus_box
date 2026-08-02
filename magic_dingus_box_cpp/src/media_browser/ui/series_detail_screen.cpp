#include "media_browser/ui/series_detail_screen.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/sonarr/sonarr_client.h"
#include "media_browser/ui/mb_chrome.h"
#include "media_browser/ui/mb_ui_utils.h"
#include "platform/input_manager.h"
#include "spdlog/spdlog.h"
#include "ui/renderer.h"
#include "ui/theme.h"
#include "ui/toast.h"

namespace media_browser::ui {

namespace {
// Sets the done flag on every exit path of a worker (DetailScreen idiom).
struct DoneFlag {
    std::shared_ptr<std::atomic<bool>> flag;
    ~DoneFlag() {
        if (flag) flag->store(true, std::memory_order_release);
    }
};
constexpr int kBodyFontPx = 16;
constexpr int kRowFontPx = 18;
constexpr int kRowH = 34;
// Mirrors mb_chrome.cpp's anonymous-namespace kTitleFontPx (32): the header
// title is drawn with mb_draw_title_text in the Zen Dots face, which is a
// DIFFERENT metric from mb_text_width. Measuring the title with the body
// font would under-cut it and let a long series name run under the frame.
constexpr int kHeaderTitlePx = 32;
// Vertical budget reserved out of the season list, once, in render():
// the action-row buttons (chrome::draw_button is 18 px label + 10 px
// vertical padding + 2 px border, so 52) and the paging indicator line.
constexpr int kButtonRowH = 52;
constexpr int kIndicatorRowH = 24;
// chrome::draw_button's OWN geometry (mb_chrome.cpp's kBtnFontPx /
// kBtnPadX), mirrored here so the action row can predict a button's width
// BEFORE drawing it and stop at the safe inset. draw_button reports its rect
// only after it has already painted, which is too late to decline.
constexpr int kButtonFontPx = 18;
constexpr int kButtonPadX_px = 18;
}  // namespace

SeriesDetailScreen::SeriesDetailScreen(SonarrClient& sonarr, TmdbClient& tmdb,
                                       QbittorrentClient* qbit,
                                       bool sonarr_configured)
    : sonarr_(sonarr), tmdb_(tmdb), qbit_(qbit),
      sonarr_configured_(sonarr_configured) {}

SeriesDetailScreen::~SeriesDetailScreen() {
    // Invalidate every in-flight publish BEFORE joining anything: a worker
    // that finishes between the bump and its join sees a stale generation
    // and drops its result instead of writing into a half-destroyed object.
    // BOTH generations must move — fetch_gen_ gates the two load workers,
    // poll_gen_ gates the re-poll, and they publish into the SAME pending_.
    fetch_gen_.fetch_add(1);
    poll_gen_.fetch_add(1);
    if (mut_worker_.joinable()) mut_worker_.join();
    if (poll_worker_.joinable()) poll_worker_.join();
    for (auto& w : workers_) {
        if (w.thread.joinable()) w.thread.join();
    }
    // Worst-case shutdown latency is the mutation worker's: a whole-series
    // add is add_series (~13.5 s ceiling: add_settle_timeout_ms +
    // add_settle_poll_ms + timeout_secs) plus N season PUTs, a search and a
    // GET, each bounded by cfg_.timeout_secs (5 s). This screen is destroyed
    // only at kiosk shutdown, where nothing is watchdogged. Detaching
    // instead would be strictly worse — a detached worker writes into freed
    // members.
}

void SeriesDetailScreen::set_tmdb_id(int tmdb_id) {
    if (tmdb_id == tmdb_id_) return;
    tmdb_id_ = tmdb_id;
    needs_refresh_ = true;
}

void SeriesDetailScreen::enter() {
    if (needs_refresh_) {
        needs_refresh_ = false;
        fetch();
    }
}

void SeriesDetailScreen::reap_finished_workers() {
    for (auto it = workers_.begin(); it != workers_.end();) {
        if (it->done->load(std::memory_order_acquire) && it->thread.joinable()) {
            it->thread.join();
            it = workers_.erase(it);
        } else {
            ++it;
        }
    }
}

void SeriesDetailScreen::fetch() {
    reap_finished_workers();
    // Bump BOTH generations FIRST, then clear. A load worker or a re-poll
    // that finishes between here and the clear must find a stale generation
    // and discard: otherwise it publishes series A's Series / in_library into
    // the pending_ that series B is about to drain, and Remove would target
    // A's sonarr_id under B's header.
    const uint64_t gen = fetch_gen_.fetch_add(1) + 1;
    poll_gen_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        pending_ = PendingLoad{};
        pending_ready_.store(false, std::memory_order_release);
    }
    // Reset render-thread state to a clean Loading page.
    detail_.reset();
    series_.reset();
    rows_.clear();
    downloading_seasons_.clear();
    tmdb_done_ = tmdb_ok_ = false;
    sonarr_done_ = sonarr_ok_ = in_library_ = false;
    series_settled_ = true;
    season_page_ = 0;
    season_page_count_ = 1;
    // With buttons_ cleared, a SELECT during Loading is a STRUCTURAL no-op —
    // there is nothing to dispatch. Before this, the previous series' stale
    // (and invisible) buttons still accepted SELECT and fired a silent add
    // with an empty title fallback.
    buttons_.clear();
    focus_ = 0;
    whole_armed_ = false;
    remove_pending_ = false;
    navigate_back_ = false;
    // Task 8's poll gate must not inherit series A's timestamp — it would
    // delay series B's first poll by a full interval.
    last_poll_at_ = {};
    const int id = tmdb_id_;
    if (id <= 0) {
        tmdb_done_ = true;  // resolver -> TmdbError; nothing to fetch
        sonarr_done_ = true;
        return;
    }
    // Two independent workers; each publishes into pending_ under the
    // mutex and flips pending_ready_. An unconfigured box spawns only
    // the TMDB half — the Sonarr flags stay at their "never asked"
    // defaults and the resolver routes to NotConfigured.
    try {
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::thread t(&SeriesDetailScreen::run_tmdb_fetch, this, gen, id, done);
        workers_.push_back(FetchWorker{std::move(t), std::move(done)});
    } catch (const std::system_error& e) {
        spdlog::warn("[SeriesDetail] tmdb worker spawn failed: {}", e.what());
        tmdb_done_ = true;  // TmdbError; user can back out and retry
    }
    if (sonarr_configured_) {
        try {
            auto done = std::make_shared<std::atomic<bool>>(false);
            std::thread t(&SeriesDetailScreen::run_sonarr_fetch, this, gen, id,
                          done);
            workers_.push_back(FetchWorker{std::move(t), std::move(done)});
        } catch (const std::system_error& e) {
            spdlog::warn("[SeriesDetail] sonarr worker spawn failed: {}",
                         e.what());
            sonarr_done_ = true;  // SonarrUnreachable; page stays read-only
        }
    } else {
        sonarr_done_ = true;  // resolver: NotConfigured before this is read
    }
}

void SeriesDetailScreen::run_tmdb_fetch(uint64_t gen, int tmdb_id,
                                        std::shared_ptr<std::atomic<bool>> done) {
    DoneFlag df{done};
    auto detail = tmdb_.get_tv_detail(tmdb_id);
    std::lock_guard<std::mutex> lk(pending_mtx_);
    // Recheck under the lock — a worker that passed a pre-lock check could
    // be descheduled across fetch()'s bump-and-clear and publish stale data
    // into the new series' pending_.
    if (gen != fetch_gen_.load()) return;  // preempted — discard
    pending_.tmdb_done = true;
    pending_.tmdb_ok = detail.has_value();
    pending_.detail = std::move(detail);
    pending_ready_.store(true, std::memory_order_release);
}

void SeriesDetailScreen::run_sonarr_fetch(uint64_t gen, int tmdb_id,
                                          std::shared_ptr<std::atomic<bool>> done) {
    DoneFlag df{done};
    // In-library detection: the reachability-honest checked variant plus a
    // tmdb_id scan. NOT lookup_by_tmdb -> find_series_by_tvdb — that pair
    // cannot distinguish "not mapped" from "not answering", costs an extra
    // round-trip, and this shape needs no TMDB->TVDB hop at all.
    auto lib = sonarr_.get_library_checked();
    std::optional<Series> match;
    if (lib.has_value()) {
        for (const auto& s : *lib) {
            if (s.tmdb_id == tmdb_id) {
                match = s;
                break;
            }
        }
    }
    // Quality definitions ride along on the same worker: one Sonarr
    // round-trip's latency, and the estimate needs them before any add
    // affordance renders. Empty on failure -> pick_preferred falls back.
    std::vector<QualityDefinition> defs;
    if (lib.has_value()) defs = sonarr_.get_quality_definitions();
    std::lock_guard<std::mutex> lk(pending_mtx_);
    // Recheck under the lock — a worker that passed a pre-lock check could
    // be descheduled across fetch()'s bump-and-clear and publish stale data
    // into the new series' pending_.
    if (gen != fetch_gen_.load()) return;  // preempted — discard
    pending_.sonarr_done = true;
    pending_.sonarr_ok = lib.has_value();
    pending_.in_library = match.has_value();
    if (match.has_value()) {
        pending_.settled = record_refreshed(*match);
        pending_.has_settled = true;
    }
    pending_.series = std::move(match);
    pending_.quality_defs = std::move(defs);
    pending_ready_.store(true, std::memory_order_release);
}

void SeriesDetailScreen::apply_pending() {
    if (!pending_ready_.load(std::memory_order_acquire)) return;
    PendingLoad p;
    {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        // Move-and-reset, not copy: each half publishes once. tmdb_done_ /
        // detail_ / sonarr_done_ / series_ etc. are accumulated on `this`
        // (not re-derived from pending_ each frame), so a half applied on
        // an earlier drain is never lost when the other half's later
        // publish resets pending_ to fresh defaults.
        p = std::move(pending_);
        pending_ = PendingLoad{};
        pending_ready_.store(false, std::memory_order_release);
    }
    if (p.tmdb_done) {
        tmdb_done_ = true;
        tmdb_ok_ = p.tmdb_ok;
        if (p.detail.has_value()) detail_ = std::move(p.detail);
    }
    if (p.sonarr_done) {
        sonarr_done_ = true;
        sonarr_ok_ = p.sonarr_ok;
        in_library_ = p.in_library;
        if (p.series.has_value()) series_ = std::move(p.series);
        if (p.has_settled) series_settled_ = p.settled;
        if (!p.quality_defs.empty())
            mb_per_min_ = pick_preferred_mb_per_min(p.quality_defs);
    }
    if (p.has_downloading) downloading_seasons_ = std::move(p.downloading);
    rebuild_rows();
    rebuild_buttons();
}

void SeriesDetailScreen::rebuild_rows() {
    if (!detail_.has_value()) {
        rows_.clear();
        return;
    }
    rows_ = merge_season_rows(detail_->seasons,
                              series_.has_value() ? &*series_ : nullptr,
                              downloading_seasons_);
}

void SeriesDetailScreen::rebuild_buttons() {
    // Thin caller: every decision (which buttons, their labels, and where
    // focus lands) is decide_action_row's, in series_detail_logic.h, under
    // Mac table tests. This function only marshals render-thread state into
    // ActionRowInputs and copies the answer back — the focus algebra it used
    // to inline was the exact thing that once fired the wrong mutation.
    ActionRowInputs in;
    in.state = decide_series_detail_state(
        SeriesDetailInputs{tmdb_done_, tmdb_ok_, sonarr_configured_,
                           sonarr_done_, sonarr_ok_, in_library_});
    in.series_settled = series_settled_;
    in.next_unmonitored = next_unmonitored_season(rows_);
    in.remove_pending = remove_pending_;
    in.whole_armed = whole_armed_;
    in.whole_estimate_bytes = whole_estimate_bytes_;
    if (focus_ >= 0 && focus_ < static_cast<int>(buttons_.size()))
        in.prev_focus_action = buttons_[static_cast<size_t>(focus_)].action;

    ActionRow row = decide_action_row(in);
    buttons_ = std::move(row.buttons);
    focus_ = row.focus;
}

void SeriesDetailScreen::spawn_mutation(std::function<void()> body) {
    if (mut_in_flight_.load()) return;               // one at a time
    if (mut_worker_.joinable()) mut_worker_.join();  // reap the finished one
    // A poll already in flight is stale by definition once a mutation
    // starts: if it published between this mutation's drain and the next
    // apply_pending, its PRE-mutation snapshot would overwrite the
    // mutation's result — for remove, permanently (the page would show a
    // removed series as in-library and never recover). One bump closes it.
    poll_gen_.fetch_add(1);
    mut_tmdb_id_ = tmdb_id_;
    // Stamp the LOAD as well as the id. The id alone cannot see A→B→A: back
    // on series A, a refetch has already republished A's PRE-mutation
    // library snapshot, and an id-only gate lets the drain apply the
    // mutation's result on top of — or be overwritten by — that stale load.
    // The observed symptom was "Add Season 1" on a series that had just been
    // added, sticky until the user visited a different series.
    mut_fetch_gen_ = fetch_gen_.load();
    mut_in_flight_.store(true);
    mut_done_.store(false);
    const std::string title =
        detail_.has_value() ? detail_->title : std::string("This series");
    try {
        mut_worker_ = std::thread([this, body = std::move(body), title]() {
            // Flips mut_done_ on EVERY exit path, exception included. Without
            // it a throw out of body() leaves mut_in_flight_ stuck true and
            // the action row inert for the rest of the session — and an
            // uncaught throw out of the thread is std::terminate.
            struct DoneGuard {
                std::atomic<bool>& flag;
                ~DoneGuard() { flag.store(true, std::memory_order_release); }
            } guard{mut_done_};
            try {
                body();
            } catch (const std::exception& e) {
                spdlog::warn("[SeriesDetail] mutation threw: {}", e.what());
                std::lock_guard<std::mutex> lk(mut_mtx_);
                mut_toast_ = title +
                             ": something went wrong \xE2\x80\x94 try again";
            }
        });
    } catch (const std::system_error& e) {
        spdlog::warn("[SeriesDetail] mutation spawn failed: {}", e.what());
        mut_in_flight_.store(false);
        ::ui::Toast::show("Couldn't start the operation \xE2\x80\x94 try again");
        return;
    }
    rebuild_buttons();  // row persists; render() dims it while in flight
}

void SeriesDetailScreen::drain_mutation() {
    if (!mut_done_.load(std::memory_order_acquire)) return;
    // ALWAYS clear the in-flight state, whatever the outcome and whichever
    // series is on screen now. A drain that bailed early on an identity
    // mismatch used to leave mut_in_flight_ latched forever.
    mut_done_.store(false);
    mut_in_flight_.store(false);

    std::string toast;
    std::optional<Series> fresh;
    bool fresh_settled = true;
    bool removed = false;
    bool have_verdict = false;
    DiskVerdict verdict = DiskVerdict::Block;
    int64_t estimate = 0;
    {
        std::lock_guard<std::mutex> lk(mut_mtx_);
        toast = std::move(mut_toast_);
        mut_toast_.clear();
        fresh = std::move(mut_series_);
        mut_series_.reset();
        fresh_settled = mut_settled_;
        mut_settled_ = true;
        removed = mut_removed_;
        mut_removed_ = false;
        have_verdict = mut_have_verdict_;
        mut_have_verdict_ = false;
        verdict = mut_verdict_;
        estimate = mut_estimate_;
        mut_estimate_ = 0;
    }

    // The toast shows REGARDLESS of which page we are on: every worker
    // composes it title-prefixed ("Breaking Bad: Season 1 search started"),
    // so an outcome that lands after the user moved on is still meaningful
    // instead of silently lost.
    if (!toast.empty()) ::ui::Toast::show(toast);

    // Everything that MUTATES THIS PAGE is gated on identity: the SAME
    // series AND the same load of it (A→B→A refetches A, so the id matches
    // again while the page state underneath is a fresh pre-mutation
    // snapshot).
    if (mut_tmdb_id_ != tmdb_id_ || mut_fetch_gen_ != fetch_gen_.load()) {
        // The toast above already told the user what happened; the page
        // state deliberately does not move. Leave a trace so a "my add
        // didn't stick" report has something to read: the alternative is a
        // silently dropped application with no record anywhere.
        spdlog::info("[SeriesDetail] dropping mutation result for tmdb:{} "
                     "(gen {}) — page now tmdb:{} (gen {})",
                     mut_tmdb_id_, mut_fetch_gen_, tmdb_id_,
                     fetch_gen_.load());
        // A dropped `removed` is the ONE outcome that outlives the page it
        // was started from: the Sonarr record is gone for good. Without this
        // flag, re-entering that same tmdb_id hits set_tmdb_id's same-id
        // no-op and enter()'s short-circuit, and the page repaints its cached
        // pre-remove snapshot — a "Remove" button aimed at a record Sonarr no
        // longer knows about. DetailScreen's drain_remove_result sets exactly
        // this for exactly this reason.
        if (removed) needs_refresh_ = true;
        rebuild_buttons();
        return;
    }
    if (have_verdict && verdict != DiskVerdict::Block) {
        // Armed HERE, on the render thread — the 4 s window starts when the
        // label appears, not when a worker finished computing free space.
        whole_estimate_bytes_ = estimate;
        whole_armed_ = true;
        whole_armed_at_ = std::chrono::steady_clock::now();
    }
    if (fresh.has_value()) {
        series_ = std::move(fresh);
        series_settled_ = fresh_settled;
        in_library_ = true;
        sonarr_done_ = sonarr_ok_ = true;
        rebuild_rows();
    }
    if (removed) {
        series_.reset();
        // Empty today; Task 8's queue poll makes it load-bearing — a stale
        // downloading set would paint Downloading badges on the rows of a
        // series that no longer exists during the frame before navigate_back_
        // is consumed.
        downloading_seasons_.clear();
        in_library_ = false;
        series_settled_ = true;
        rebuild_rows();
        navigate_back_ = true;
    }
    last_poll_at_ = {};  // Task 8: refresh badges next frame, not in 9 s
    rebuild_buttons();
    // Deliberately NO focus_on(WholeSeries) here: the identity-preserving
    // rebuild already keeps focus on the button the user pressed, and if
    // they rotated away during the free-space fetch, yanking focus back
    // would be the one place it moves without being asked. The armed label
    // + Warn color are the signal.
}

void SeriesDetailScreen::expire_confirms() {
    const auto now = std::chrono::steady_clock::now();
    bool changed = false;
    if (whole_armed_ &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - whole_armed_at_).count() > kWholeConfirmMs) {
        whole_armed_ = false;
        changed = true;
    }
    if (remove_pending_ &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - remove_pending_at_).count() > kRemovePendingMs) {
        remove_pending_ = false;
        changed = true;
    }
    if (changed) rebuild_buttons();
}

void SeriesDetailScreen::dispatch_action(Action a) {
    // Pressing anything OTHER than the armed control disarms it first.
    if (a != Action::WholeSeries) whole_armed_ = false;
    if (a != Action::Remove && a != Action::ConfirmRemove) remove_pending_ = false;
    switch (a) {
        case Action::AddSeason1: {
            const int id = tmdb_id_;
            const std::string title =
                detail_.has_value() ? detail_->title : std::string("This series");
            spawn_mutation([this, id, title]() {
                // Quality profile BY NAME ("Any" is this box's profile; the
                // id is not portable) — DetailScreen::pick_quality_profile_id's
                // policy minus its movie-only fallbacks.
                const auto profiles = sonarr_.get_quality_profiles();
                int qp_id = 0;
                for (const auto& qp : profiles) {
                    if (qp_id == 0) qp_id = qp.id;
                    if (qp.name == "Any") { qp_id = qp.id; break; }
                }
                if (qp_id == 0) {
                    // Distinguish "Sonarr answered, no profiles" from "we
                    // never reached Sonarr" — last_error() is set on every
                    // transport failure, so an empty vector alone is
                    // ambiguous and blaming the config would be wrong.
                    const std::string err = sonarr_.last_error();
                    std::lock_guard<std::mutex> lk(mut_mtx_);
                    mut_toast_ = err.empty()
                        ? title + ": Sonarr has no quality profile \xE2\x80\x94 not added"
                        : title + ": couldn't reach Sonarr \xE2\x80\x94 " + err;
                    return;
                }
                // monitor=true => addOptions.monitor="firstSeason" +
                // searchForMissingEpisodes=true: exactly the spec's
                // season-at-a-time default, applied by Sonarr itself.
                auto res = sonarr_.add_series(id, qp_id, /*monitor=*/true, title);
                if (!res.ok) {
                    const std::string err = sonarr_.last_error();
                    std::lock_guard<std::mutex> lk(mut_mtx_);
                    mut_toast_ = title + ": add failed \xE2\x80\x94 " + err;
                    return;
                }
                if (!res.settled) {
                    // settled==false: seasons[] is EMPTY by contract. The
                    // page keeps rendering TMDB rows and hides the add
                    // controls until the Task-8 poll settles the record.
                    std::lock_guard<std::mutex> lk(mut_mtx_);
                    mut_series_ = res.series;
                    mut_settled_ = false;
                    mut_toast_ =
                        title + ": added \xE2\x80\x94 syncing seasons\xE2\x80\xA6";
                    return;
                }
                // ---- settled: did the add actually DO anything? ----
                // add_series returns ok=true from its find-existing branch
                // without applying addOptions, monitoring anything or
                // searching. That branch dedupes by tvdbId while this screen
                // detects in-library by tmdbId, so a library record with
                // tmdb_id == 0 still offers "Add Season 1" — and the press
                // would toast "Season 1 search started" having done nothing
                // at all. Read the outcome off the returned record instead
                // of trusting ok=true.
                //
                // The season we check is the LOWEST NON-SPECIAL season
                // NUMBER, never the literal 1: addOptions.monitor =
                // "firstSeason" monitors the first AIRED season, which is
                // not always numbered 1 (and season 0 is specials).
                const int sid = res.series.sonarr_id;
                int first_season = 0;
                bool first_monitored = false;
                int first_files = 0;
                for (const auto& s : res.series.seasons) {
                    if (s.season_number <= 0) continue;
                    if (first_season != 0 && s.season_number >= first_season)
                        continue;
                    first_season = s.season_number;
                    first_monitored = s.monitored;
                    first_files = s.episode_file_count;
                }
                std::string toast;
                std::optional<Series> fresh;
                if (first_season == 0 || sid <= 0) {
                    // A settled record with no ordinary season, or with no
                    // id to act on: nothing to verify, so claim nothing.
                    toast = title + ": added to your TV library";
                } else if (!first_monitored) {
                    // The idempotent branch (or any drift): the add did NOT
                    // apply monitoring, so do it explicitly and report THOSE
                    // outcomes — same shape as the NextSeason flow.
                    if (!sonarr_.set_season_monitored(sid, first_season, true)) {
                        const std::string err = sonarr_.last_error();
                        std::lock_guard<std::mutex> lk(mut_mtx_);
                        mut_toast_ = title + ": couldn't monitor season " +
                                     std::to_string(first_season) +
                                     " \xE2\x80\x94 " + err;
                        return;
                    }
                    const bool searched =
                        sonarr_.trigger_season_search(sid, first_season);
                    fresh = sonarr_.get_series(sid);
                    toast = searched
                        ? title + ": Season " + std::to_string(first_season) +
                              " search started"
                        : title + ": Season " + std::to_string(first_season) +
                              " monitored, but the search didn't start "
                              "\xE2\x80\x94 Sonarr will pick it up on RSS";
                } else if (first_files > 0) {
                    // Monitored AND already has files: the box already had
                    // this series and nothing was started, so say that
                    // rather than promising a search.
                    toast = title + ": already in your TV library";
                } else {
                    // Monitored with nothing on disk yet — the add path
                    // genuinely acted (or a prior add did) and
                    // searchForMissingEpisodes rode in with monitor=true.
                    toast = title + ": Season " + std::to_string(first_season) +
                            " search started";
                }
                std::lock_guard<std::mutex> lk(mut_mtx_);
                if (fresh.has_value()) {
                    mut_settled_ = record_refreshed(*fresh);
                    mut_series_ = std::move(fresh);
                } else {
                    mut_series_ = res.series;
                    mut_settled_ = true;
                }
                mut_toast_ = std::move(toast);
            });
            break;
        }
        case Action::NextSeason: {
            if (!series_.has_value() || series_->sonarr_id <= 0) break;
            const auto next = next_unmonitored_season(rows_);
            if (!next.has_value()) break;
            const int sid = series_->sonarr_id;
            const int season = *next;
            const std::string title =
                detail_.has_value() ? detail_->title : std::string("This series");
            spawn_mutation([this, sid, season, title]() {
                if (!sonarr_.set_season_monitored(sid, season, true)) {
                    const std::string err = sonarr_.last_error();
                    std::lock_guard<std::mutex> lk(mut_mtx_);
                    mut_toast_ = title + ": couldn't monitor season " +
                                 std::to_string(season) + " \xE2\x80\x94 " + err;
                    return;
                }
                const bool searched = sonarr_.trigger_season_search(sid, season);
                auto fresh = sonarr_.get_series(sid);
                std::lock_guard<std::mutex> lk(mut_mtx_);
                mut_toast_ = searched
                    ? title + ": Season " + std::to_string(season) +
                          " search started"
                    : title + ": Season " + std::to_string(season) +
                          " monitored, but the search didn't start "
                          "\xE2\x80\x94 Sonarr will pick it up on RSS";
                if (fresh.has_value()) {
                    mut_settled_ = record_refreshed(*fresh);
                    mut_series_ = std::move(fresh);
                }
            });
            break;
        }
        case Action::WholeSeries: {
            if (whole_armed_) {
                // ---- press 2: execute ----
                whole_armed_ = false;
                const bool pre_add = !in_library_;
                const int id = tmdb_id_;
                const std::string title =
                    detail_.has_value() ? detail_->title
                                        : std::string("This series");
                const int sid = series_.has_value() ? series_->sonarr_id : 0;
                std::vector<int> to_monitor;
                for (const auto& row : rows_) {
                    if (!row.monitored) to_monitor.push_back(row.season_number);
                }
                spawn_mutation([this, pre_add, id, title, sid, to_monitor]() {
                    int series_id = sid;
                    std::optional<Series> added;
                    if (pre_add) {
                        const auto profiles = sonarr_.get_quality_profiles();
                        int qp_id = 0;
                        for (const auto& qp : profiles) {
                            if (qp_id == 0) qp_id = qp.id;
                            if (qp.name == "Any") { qp_id = qp.id; break; }
                        }
                        if (qp_id == 0) {
                            const std::string err = sonarr_.last_error();
                            std::lock_guard<std::mutex> lk(mut_mtx_);
                            mut_toast_ = err.empty()
                                ? title + ": Sonarr has no quality profile "
                                          "\xE2\x80\x94 not added"
                                : title + ": couldn't reach Sonarr \xE2\x80\x94 " + err;
                            return;
                        }
                        // *** monitor=true is REQUIRED here. *** add_series
                        // writes the SERIES-LEVEL monitored flag from this
                        // same parameter (series["monitored"] = monitor) and
                        // no client method exists to flip it afterwards.
                        // "none" would leave the series permanently
                        // unmonitored, and Sonarr's
                        // MonitoredEpisodeSpecification rejects every release
                        // for an unmonitored series: seasons monitored,
                        // search runs, NOTHING ever downloads — invisibly.
                        //
                        // The firstSeason-vs-our-PUTs race this used to fear
                        // is provably over when settled==true: add_settled
                        // (monitor=true) requires the applied monitoring
                        // state to have been OBSERVED. The race exists only
                        // on the timeout path — which is why the season PUTs
                        // below are gated on res.settled.
                        auto res = sonarr_.add_series(id, qp_id,
                                                      /*monitor=*/true, title);
                        if (!res.ok) {
                            const std::string err = sonarr_.last_error();
                            std::lock_guard<std::mutex> lk(mut_mtx_);
                            mut_toast_ = title + ": add failed \xE2\x80\x94 " + err;
                            return;
                        }
                        if (!res.settled) {
                            // firstSeason has NOT been applied yet; seasons
                            // PUT now would be unmonitored behind us when it
                            // lands. Stop honestly: the row shows [Remove]
                            // only until the poll settles, then "Whole
                            // series…" reappears and the retry takes the
                            // in-library path — idempotent by design.
                            std::lock_guard<std::mutex> lk(mut_mtx_);
                            mut_series_ = res.series;
                            mut_settled_ = false;
                            mut_toast_ = title + ": added \xE2\x80\x94 syncing seasons; "
                                         "the whole-series option returns when "
                                         "Sonarr finishes";
                            return;
                        }
                        // Settled: S1 is already monitored and the add-time
                        // search already covers it (searchForMissingEpisodes
                        // rode in with monitor=true); S1's entry in
                        // to_monitor is a harmless idempotent PUT, and the
                        // series search below may re-query S1 — redundant,
                        // not harmful.
                        series_id = res.series.sonarr_id;
                        added = res.series;
                    }
                    if (series_id <= 0) {
                        std::lock_guard<std::mutex> lk(mut_mtx_);
                        if (added.has_value()) {
                            mut_series_ = std::move(added);
                            mut_settled_ = false;
                        }
                        mut_toast_ = title + ": added, but Sonarr hasn't assigned "
                                     "an id yet \xE2\x80\x94 try Whole series again "
                                     "in a moment";
                        return;
                    }
                    // For an ANNOUNCED series Sonarr may not know every
                    // season yet, so some of these PUTs legitimately fail.
                    // They are counted, not hidden, and the toast below is
                    // composed from the real count.
                    int failed = 0;
                    for (int season : to_monitor) {
                        if (!sonarr_.set_season_monitored(series_id, season, true))
                            ++failed;
                    }
                    const bool searched = sonarr_.trigger_series_search(series_id);
                    auto fresh = sonarr_.get_series(series_id);
                    const int total = static_cast<int>(to_monitor.size());
                    std::lock_guard<std::mutex> lk(mut_mtx_);
                    if (fresh.has_value()) {
                        mut_settled_ = record_refreshed(*fresh);
                        mut_series_ = std::move(fresh);
                    } else if (added.has_value()) {
                        mut_series_ = std::move(added);
                        mut_settled_ = false;
                    }
                    // Composed from BOTH signals — the failure count AND the
                    // search outcome. Never claim "Monitored" or promise RSS
                    // when every PUT failed (Sonarr stopped mid-flow): the
                    // series is in the library with nothing monitored and
                    // nothing will ever arrive.
                    if (total > 0 && failed == total) {
                        mut_toast_ = title + ": couldn't monitor seasons "
                                     "\xE2\x80\x94 is Sonarr running?";
                    } else if (!searched) {
                        mut_toast_ = title + ": seasons monitored, but the search "
                                     "didn't start \xE2\x80\x94 Sonarr will pick "
                                     "them up on RSS";
                    } else if (failed > 0) {
                        mut_toast_ = title + ": search started (" +
                                     std::to_string(failed) + " of " +
                                     std::to_string(total) +
                                     " seasons couldn't be monitored)";
                    } else {
                        mut_toast_ = title + ": whole-series search started";
                    }
                });
                break;
            }
            // ---- press 1: estimate + free space + verdict, off-thread ----
            // The multiplicand: in-library uses Sonarr's real per-episode
            // runtime; PRE-ADD there is no record, so estimate_remaining_bytes
            // falls back to 45 minutes. We deliberately do NOT call
            // lookup_by_tmdb to fetch the real runtime first: that would give
            // find_series_by_tvdb's mock family its first indirect kiosk
            // surface AND add a round-trip to a gesture that already waits.
            // The armed label says "(est)" precisely because of this.
            const int runtime = (in_library_ && series_.has_value())
                                    ? series_->runtime_minutes : 0;
            const int64_t estimate =
                estimate_remaining_bytes(rows_, runtime, mb_per_min_);
            const std::string title =
                detail_.has_value() ? detail_->title : std::string("This series");
            // Immediate feedback: the free-space fetch can take the full 5 s
            // HTTP timeout, and the Allow path's only signal is the armed
            // label appearing — a user who glances away would otherwise read
            // press-1 as "the button did nothing".
            ::ui::Toast::show(title + ": checking free space\xE2\x80\xA6");
            spawn_mutation([this, estimate, title]() {
                // Free space, two sources with DIFFERENT zero semantics:
                //
                //  - Sonarr's root folder: freeSpace is parsed with a 0
                //    default, so an ABSENT/null field (Sonarr can't stat the
                //    folder — the stale-container-bind state that
                //    magic-dingus-storage-attach.service exists for) is
                //    indistinguishable from a genuinely full disk. A 0 here
                //    is therefore AMBIGUOUS and must fall through, or a
                //    healthy box with 400 GB free gets a false "0 GB free"
                //    Block with a wrong diagnosis.
                //  - std::filesystem::space on the host path: failure is a
                //    distinct error code, so a returned 0 is a REAL full
                //    disk and whole_series_verdict correctly Blocks on it.
                //
                // Do not "simplify" the two guards into one.
                std::optional<int64_t> free_bytes;
                for (const auto& rf : sonarr_.get_root_folders()) {
                    if (rf.path.find("/tv") != std::string::npos &&
                        rf.free_space_bytes > 0) {
                        free_bytes = rf.free_space_bytes;
                        break;
                    }
                }
                if (!free_bytes.has_value()) {
                    std::error_code ec;
                    auto info = std::filesystem::space("/mnt/ssd/library/tv", ec);
                    if (!ec) free_bytes = static_cast<int64_t>(info.available);
                }
                const DiskVerdict v = whole_series_verdict(estimate, free_bytes);
                const auto gb = [](int64_t b) {
                    return std::to_string(b / (1024LL * 1024 * 1024)) + " GB";
                };
                std::lock_guard<std::mutex> lk(mut_mtx_);
                mut_have_verdict_ = true;
                mut_verdict_ = v;
                mut_estimate_ = estimate;
                switch (v) {
                    case DiskVerdict::Block:
                        // The codebase's FIRST blocking preflight: publish a
                        // Block verdict so drain_mutation does NOT arm, and
                        // show both numbers plus the floor.
                        mut_toast_ = title + ": not enough space \xE2\x80\x94 needs ~" +
                                     gb(estimate) + " (est), " +
                                     gb(free_bytes.value_or(0)) +
                                     " free (20 GB floor)";
                        break;
                    case DiskVerdict::WarnOnly:
                        mut_toast_ = title + ": couldn't check free space "
                                     "\xE2\x80\x94 confirm to proceed anyway";
                        break;
                    case DiskVerdict::Allow:
                        break;  // the armed label IS the feedback
                }
            });
            break;
        }
        case Action::Remove:
            remove_pending_ = true;
            remove_pending_at_ = std::chrono::steady_clock::now();
            rebuild_buttons();
            break;
        case Action::ConfirmRemove: {
            remove_pending_ = false;
            const std::string title =
                detail_.has_value() ? detail_->title : std::string("This series");
            if (!series_.has_value() || series_->sonarr_id <= 0) {
                // Without this the label stayed on "Confirm Remove" with
                // nothing behind it: every further press silently fell out of
                // the switch, which reads as a dead button. Repaint the row
                // (remove_pending_ is already cleared, so it reverts to
                // "Remove") and say why, title-prefixed like every sibling
                // toast on this screen.
                rebuild_buttons();
                ::ui::Toast::show(title + ": series id unknown \xE2\x80\x94 "
                                  "try again once syncing finishes");
                break;
            }
            const int sid = series_->sonarr_id;
            spawn_mutation([this, sid, title]() {
                // Sonarr-shaped mirror of DetailScreen::run_remove:
                //  1. Cancel in-flight downloads — ONCE PER DOWNLOAD, not
                //     once per queue row (cancel_ids_for_series does that
                //     dedupe; it is pure and Mac-tested).
                //  2. Purge every torrent the series' history knows about
                //     (catches finished+seeding ones step 1 misses).
                //  3. remove_series(delete_files=true).
                //  4. Back to origin (drained on the render thread).
                const std::vector<int> cancel_ids =
                    cancel_ids_for_series(sonarr_.get_queue(), sid);
                int cancel_failed = 0;
                int cancel_ok = 0;
                std::string cancel_err;
                for (int qid : cancel_ids) {
                    if (sonarr_.cancel_queue_item(qid)) {
                        ++cancel_ok;
                        continue;
                    }
                    ++cancel_failed;
                    // Captured AT the failing iteration. last_error() is
                    // cleared on entry to every client call, so a LATER
                    // success wipes the diagnosis and the abort toast below
                    // degrades to "NOT removed" with no reason attached.
                    if (cancel_err.empty()) cancel_err = sonarr_.last_error();
                }
                if (cancel_failed > 0) {
                    // A genuinely failed cancel still aborts before anything
                    // is deleted — the house rule that keeps a half-removed
                    // series from orphaning a torrent. But cancels use
                    // removeFromClient=true, so any that SUCCEEDED before the
                    // failure have already taken their downloads with them:
                    // saying "couldn't cancel" flat would imply nothing
                    // happened, and the user would not know data is gone.
                    std::lock_guard<std::mutex> lk(mut_mtx_);
                    mut_toast_ =
                        (cancel_ok > 0
                             ? title + ": cancelled " + std::to_string(cancel_ok) +
                                   " download(s), then failed \xE2\x80\x94 series "
                                   "NOT removed; retry is safe"
                             : title + ": couldn't cancel " +
                                   std::to_string(cancel_failed) +
                                   " download(s) \xE2\x80\x94 NOT removed") +
                        (cancel_err.empty() ? std::string()
                                            : " (" + cancel_err + ")");
                    return;
                }
                if (qbit_ != nullptr) {
                    // The history walk runs BEFORE the decision to proceed,
                    // because a FAILED walk is indistinguishable from "no
                    // history" by an unchecked return alone. Removing on a
                    // failed walk is the exact production failure
                    // DetailScreen documents — every seeding torrent orphaned
                    // forever, under a toast that said "removed".
                    //
                    // CHECKED variant, deliberately not the raw one + a
                    // follow-up last_error() read: Task 8 adds a ~9 s
                    // background re-poll on its own thread that shares this
                    // SonarrClient (and its one last_error_ member). A
                    // decision split across two calls — get the hashes, THEN
                    // separately ask last_error() — could read whatever the
                    // poll thread set or cleared in the gap, not this call's
                    // own outcome. nullopt IS the whole answer here; nothing
                    // else needs to be read to make the call.
                    //
                    // Aborting here is retry-safe: the cancel stage above is
                    // idempotent — an already-cancelled download leaves no
                    // queue row, so its ids simply dedupe to nothing next time.
                    //
                    // Still gated on qbit_ as before: a box with no qBittorrent
                    // client cannot purge anything at all, so it keeps the old
                    // semantics and accepts the orphan risk BY CONSTRUCTION.
                    const std::optional<std::vector<std::string>> hashes =
                        sonarr_.get_series_download_hashes_checked(sid);
                    if (!hashes.has_value()) {
                        std::lock_guard<std::mutex> lk(mut_mtx_);
                        // Cancel-stage context: any downloads cancelled
                        // before this abort ARE gone (removeFromClient=true
                        // in the cancel loop above), so a flat "couldn't
                        // check, NOT removed" would understate what already
                        // happened — same honesty rule as the cancel-failure
                        // toast just above.
                        mut_toast_ =
                            (cancel_ok > 0
                                 ? title + ": cancelled " +
                                       std::to_string(cancel_ok) +
                                       " download(s), then couldn't check "
                                       "for seeding torrents \xE2\x80\x94 "
                                       "series NOT removed"
                                 : title + ": couldn't check for seeding "
                                           "torrents \xE2\x80\x94 series NOT "
                                           "removed") +
                            " \xE2\x80\x94 Sonarr didn't answer";
                        return;
                    }
                    for (const auto& h : *hashes) {
                        if (!qbit_->delete_torrent(h, /*delete_files=*/true)) {
                            spdlog::warn("[SeriesDetail] qbit delete failed for {}",
                                         h);
                        }
                    }
                }
                if (!sonarr_.remove_series(sid, /*delete_files=*/true)) {
                    const std::string err = sonarr_.last_error();
                    std::lock_guard<std::mutex> lk(mut_mtx_);
                    mut_toast_ = title + ": remove failed \xE2\x80\x94 " + err;
                    return;
                }
                std::lock_guard<std::mutex> lk(mut_mtx_);
                mut_removed_ = true;
                mut_toast_ = title + ": removed from the TV library";
            });
            break;
        }
    }
}

Screen SeriesDetailScreen::handle_input(
        const std::vector<platform::InputEvent>& events) {
    // Async completion relay, FIRST — DetailScreen's drain_remove_result
    // shape. Consuming (and clearing) the flag here is what keeps one remove
    // from bricking the screen: a latched flag would return origin_ forever.
    if (navigate_back_) {
        navigate_back_ = false;
        return origin_;
    }
    for (const auto& e : events) {
        // BTN4 (SETTINGS_MENU, black) — back to whoever opened us.
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            return origin_;
        }
        // Rotary twist. platform::InputEvent has no `value` field — the
        // direction/magnitude is `delta`, and rotary events carry
        // pressed=false, so delta is the ONLY correct gate.
        if ((e.action == platform::InputAction::ROTATE ||
             e.action == platform::InputAction::ROTATE_VERTICAL) &&
            e.delta != 0) {
            // Focus is FROZEN while THIS series' mutation runs: render()
            // hides the focus ring for exactly that window, so any movement
            // would be invisible, and the post-drain rebuild would then land
            // focus on a button the user never saw themselves select.
            const bool busy = mut_in_flight_.load() && mut_tmdb_id_ == tmdb_id_;
            if (buttons_.empty() || busy) continue;
            const int n = static_cast<int>(buttons_.size());
            // Clamp, do not wrap — DetailScreen's exact idiom, so the ends
            // of the row feel like ends rather than teleporting focus.
            focus_ = std::clamp(focus_ + e.delta, 0, n - 1);
            // Any navigation cancels BOTH pending confirms, so the user can
            // never press-move-press their way into a mutation they were not
            // looking at.
            if (whole_armed_ || remove_pending_) {
                whole_armed_ = false;
                remove_pending_ = false;
                rebuild_buttons();
            }
            continue;
        }
        // BTN1 / BTN3 page the season list when it overflows.
        if (e.action == platform::InputAction::PREV && e.pressed) {
            if (season_page_ > 0) --season_page_;
            continue;
        }
        if (e.action == platform::InputAction::NEXT && e.pressed) {
            if (season_page_ + 1 < season_page_count_) ++season_page_;
            continue;
        }
        if (e.action == platform::InputAction::SELECT && e.pressed) {
            if (buttons_.empty()) continue;
            // The gate is GLOBAL (one mutation worker) but the dim is
            // series-scoped, so series B's row looks perfectly live while
            // series A's mutation finishes — up to ~14 s of presses landing
            // on nothing. A silent no-op reads as a dead button; say it.
            if (mut_in_flight_.load()) {
                ::ui::Toast::show("Still finishing the last action\xE2\x80\xA6");
                continue;
            }
            if (focus_ >= 0 && focus_ < static_cast<int>(buttons_.size()))
                dispatch_action(buttons_[static_cast<size_t>(focus_)].action);
            continue;
        }
        // BTN2 (PLAY_PAUSE, red) — intercepted globally by the exit modal in
        // main.cpp. It never reaches here; no per-screen handler needed.
    }
    return Screen::SeriesDetail;
}

void SeriesDetailScreen::maybe_repoll_series() {
    if (!in_library_ || !sonarr_ok_) return;
    if (!series_.has_value() || series_->sonarr_id <= 0) return;
    if (mut_in_flight_.load() || poll_inflight_.load()) return;
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_poll_at_).count() < kSeriesPollMs) {
        return;
    }
    last_poll_at_ = now;
    if (poll_worker_.joinable()) poll_worker_.join();
    poll_inflight_.store(true);
    const uint64_t gen = poll_gen_.fetch_add(1) + 1;
    try {
        poll_worker_ = std::thread(&SeriesDetailScreen::run_series_poll, this,
                                   gen, series_->sonarr_id, sonarr_ok_,
                                   in_library_);
    } catch (const std::system_error& e) {
        spdlog::warn("[SeriesDetail] poll spawn failed: {}", e.what());
        poll_inflight_.store(false);
    }
}

void SeriesDetailScreen::run_series_poll(uint64_t gen, int sonarr_id,
                                         bool prev_sonarr_ok,
                                         bool prev_in_library) {
    auto fresh = sonarr_.get_series(sonarr_id);
    std::unordered_set<int> downloading;
    for (const auto& q : sonarr_.get_queue()) {
        if (q.series_id == sonarr_id) downloading.insert(q.season_number);
    }
    if (gen == poll_gen_.load()) {
        std::lock_guard<std::mutex> lk(pending_mtx_);
        pending_.sonarr_done = true;
        // A poll is ADVISORY: a transient blip must not flip the page to
        // SonarrUnreachable under the user. Success proves reachability;
        // failure leaves the original fetch's verdict standing. Both priors
        // arrive by value so nothing here reads render-thread state.
        pending_.sonarr_ok = fresh.has_value() ? true : prev_sonarr_ok;
        if (fresh.has_value()) {
            pending_.in_library = true;
            // The settle signal for an existing record: has Sonarr ever
            // actually refreshed it? This is what un-hides the add controls
            // ~9 s after an unsettled add, with no user action.
            pending_.settled = record_refreshed(*fresh);
            pending_.has_settled = true;
            pending_.series = std::move(fresh);
        } else {
            pending_.in_library = prev_in_library;
        }
        pending_.downloading = std::move(downloading);
        pending_.has_downloading = true;
        pending_ready_.store(true, std::memory_order_release);
    }
    poll_inflight_.store(false, std::memory_order_release);
}

void SeriesDetailScreen::update() {
    drain_mutation();
    expire_confirms();
    apply_pending();
    maybe_repoll_series();
}

void SeriesDetailScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    const auto& th = r.mb_theme();
    r.mb_fill_rect(0, 0, static_cast<float>(screen_w),
                   static_cast<float>(screen_h), th.bg);
    // Recomputed by the body path below; body-less states have one page.
    season_page_count_ = 1;
    bool overflow = false;

    // Header: EMPTY tab strip, exactly like DetailScreen (detail_screen.cpp
    // passes /*tabs=*/{}). This screen is a drill-down, not a strip member,
    // and the 7-chip strip drawn under an arbitrary series title overlaps it.
    // The title is measured with the TITLE font's own measurer — the
    // Renderer overload of truncate_to_width measures the body font, which
    // would under-cut a Zen Dots title.
    const std::string raw_title =
        detail_.has_value() ? detail_->title : std::string("Series");
    const std::string title = truncate_to_width(
        raw_title, kHeaderTitlePx,
        static_cast<float>(screen_w - 2 * chrome::kSafeInset_px),
        [&r](const std::string& s, int px) {
            return static_cast<float>(r.mb_title_text_width(s, px));
        });
    const int content_top = chrome::draw_screen_header(
        r, screen_w, title, /*tabs=*/{}, /*focused_tab=*/-1);

    const SeriesDetailInputs in{tmdb_done_, tmdb_ok_, sonarr_configured_,
                                sonarr_done_, sonarr_ok_, in_library_};
    const SeriesDetailState st = decide_series_detail_state(in);
    const char* msg = series_detail_state_message(st);

    const bool body_less =
        (st == SeriesDetailState::Loading || st == SeriesDetailState::TmdbError);
    if (body_less) {
        // Centered message; falls THROUGH to the footer below. No return.
        if (msg != nullptr) {
            const std::string m(msg);
            const int tw = r.mb_text_width(m, kRowFontPx);
            r.mb_draw_text(m, static_cast<float>((screen_w - tw) / 2),
                           static_cast<float>(screen_h / 2), kRowFontPx,
                           st == SeriesDetailState::TmdbError ? th.highlight2
                                                              : th.dim);
        }
    } else {
        // ---- read-only body: poster left, overview right, seasons below ----
        const int body_x = chrome::kSafeInset_px;
        int y = content_top + chrome::kPad3;
        // NotConfigured / SonarrUnreachable warning line above the body.
        if (msg != nullptr) {
            r.mb_draw_text(msg, static_cast<float>(body_x),
                           static_cast<float>(y + 14), 14, th.highlight2);
            y += 22;
        }
        const int poster_w = 160, poster_h = 240;
        chrome::draw_poster_card(r, body_x, y, poster_w, poster_h,
                                 detail_->title, detail_->year,
                                 th.dim, in_library_, /*download_pct=*/-1,
                                 detail_->poster_path);
        const int text_x = body_x + poster_w + chrome::kPad4;
        const float text_w = static_cast<float>(screen_w - text_x -
                                                chrome::kSafeInset_px);
        // Meta line: year · seasons · episodes · status [· syncing…].
        {
            std::string meta = std::to_string(detail_->year);
            meta += " \xC2\xB7 " + std::to_string(detail_->number_of_seasons) +
                    " season" + (detail_->number_of_seasons == 1 ? "" : "s");
            meta += " \xC2\xB7 " + std::to_string(detail_->number_of_episodes) +
                    " episodes";
            if (!detail_->status.empty()) meta += " \xC2\xB7 " + detail_->status;
            // Honest label for the window where Sonarr holds the record but
            // has never refreshed it: the rows below are TMDB's, not Sonarr's.
            if (in_library_ && !series_settled_)
                meta += " \xC2\xB7 syncing\xE2\x80\xA6";
            r.mb_draw_text(truncate_to_width(r, meta, kBodyFontPx, text_w),
                           static_cast<float>(text_x),
                           static_cast<float>(y + 16), kBodyFontPx, th.dim);
        }
        // Overview: wrapped by the promoted chrome helper, max 5 lines.
        {
            const auto lines =
                chrome::wrap_text(r, detail_->overview, kBodyFontPx, text_w);
            int line_y = y + 44;
            for (size_t i = 0; i < lines.size() && i < 5; ++i) {
                r.mb_draw_text(lines[i], static_cast<float>(text_x),
                               static_cast<float>(line_y), kBodyFontPx, th.fg);
                line_y += kBodyFontPx + 6;
            }
        }
        // ---- season list (paged) ----
        // The action row AND the paging indicator are reserved out of the
        // list's budget HERE, once. Nothing below may draw past list_top +
        // per_page*kRowH, so the "+N more" overlap of the button row that
        // the previous revision had is structurally impossible.
        const int list_top = y + poster_h + chrome::kPad3;
        const int list_bottom = screen_h - chrome::kFooterHeight_px -
                                chrome::kPad3;
        const int list_avail =
            list_bottom - kButtonRowH - kIndicatorRowH - list_top;
        // CRT_NATIVE (640x480 logical) leaves no room below the poster —
        // list_avail goes NEGATIVE there, and a forced one-row minimum drew
        // into the reserved button band. Clamp to zero rows: the totals
        // line, action row, and footer still render; per-season detail is a
        // 720p+ affordance.
        const int per_page = std::max(0, list_avail / kRowH);
        const int total_rows = static_cast<int>(rows_.size());
        // per_page can be 0 on the 640x480 canvas (the clamp above) —
        // dividing by it is UB, and the indicator would land back in the
        // poster region. Zero rows means one page and no paging affordance.
        season_page_count_ = per_page > 0
            ? std::max(1, (total_rows + per_page - 1) / per_page) : 1;
        if (season_page_ >= season_page_count_)
            season_page_ = season_page_count_ - 1;
        if (season_page_ < 0) season_page_ = 0;
        overflow = per_page > 0 && total_rows > per_page;
        const int first = season_page_ * per_page;
        const int last = std::min(total_rows, first + per_page);

        int list_y = list_top;
        for (int i = first; i < last; ++i) {
            const auto& row = rows_[static_cast<size_t>(i)];
            std::string label = "Season " + std::to_string(row.season_number);
            std::string counts =
                std::to_string(row.episode_file_count) + "/" +
                std::to_string(row.episode_count) + " eps";
            const char* state_txt = nullptr;
            ::ui::Color state_col = th.dim;
            switch (row.state) {
                case SeasonState::None:
                    state_txt = row.monitored ? "monitored" : "\xE2\x80\x94";
                    break;
                case SeasonState::Downloading:
                    state_txt = "downloading";
                    state_col = th.highlight2;
                    break;
                case SeasonState::Partial:
                    state_txt = "partial";
                    state_col = th.accent;
                    break;
                case SeasonState::Complete:
                    state_txt = "complete";
                    state_col = th.highlight1;
                    break;
            }
            r.mb_draw_text(label, static_cast<float>(body_x),
                           static_cast<float>(list_y + 22), kRowFontPx, th.fg);
            r.mb_draw_text(counts, static_cast<float>(body_x + 220),
                           static_cast<float>(list_y + 22), kBodyFontPx,
                           th.dim);
            r.mb_draw_text(state_txt, static_cast<float>(body_x + 360),
                           static_cast<float>(list_y + 22), kBodyFontPx,
                           state_col);
            list_y += kRowH;
        }
        // Paging indicator — inside the reserved band, only when it earns
        // its line. A 21-season show is the headline case for this screen;
        // without paging its later seasons were simply unreachable.
        if (overflow) {
            const std::string ind =
                "Seasons " + std::to_string(first + 1) + "\xE2\x80\x93" +
                std::to_string(last) + " of " + std::to_string(total_rows) +
                " \xC2\xB7 [BTN1/BTN3]";
            r.mb_draw_text(ind, static_cast<float>(body_x),
                           static_cast<float>(list_bottom - kButtonRowH - 8),
                           kBodyFontPx, th.dim);
        }
        // Action row. Its height was already reserved out of the season
        // list's budget above (kButtonRowH), so it can never overlap a row.
        if (!buttons_.empty()) {
            // SERIES-SCOPED: another series' still-finishing mutation must
            // not gray THIS page's row. SELECT stays globally gated (one
            // worker), so a press in that window is ignored briefly (≤14 s
            // worst case) — never misapplied.
            const bool busy = mut_in_flight_.load() && mut_tmdb_id_ == tmdb_id_;
            int bx = body_x;
            const int brow_y = list_bottom - kButtonRowH;
            const int row_right = screen_w - chrome::kSafeInset_px;
            int drawn = 0;
            for (size_t i = 0; i < buttons_.size(); ++i) {
                // Defensive clamp: stop before a button would cross the safe
                // inset rather than running it under the cabinet art. The
                // motivating canvas is 640x480 (CRT_NATIVE), where three
                // buttons plus a long "Confirm ~N GB (est)" label are not
                // guaranteed to fit; Task 9's acceptance eyeballs the row on
                // the real box. Width is predicted with draw_button's own
                // geometry (kBtnFontPx 18 + kBtnPadX 18 a side, mb_chrome.cpp)
                // — worst case a drift there clamps one button early, which
                // is still strictly better than drawing it where nobody can
                // see it.
                const int bw = r.mb_text_width(buttons_[i].label,
                                               kButtonFontPx) +
                               2 * kButtonPadX_px;
                if (drawn > 0 && bx + bw > row_right) break;
                chrome::ButtonKind kind = chrome::ButtonKind::Ok;
                if (buttons_[i].action == Action::Remove ||
                    buttons_[i].action == Action::ConfirmRemove ||
                    (buttons_[i].action == Action::WholeSeries && whole_armed_)) {
                    kind = chrome::ButtonKind::Warn;
                } else if (buttons_[i].action == Action::WholeSeries) {
                    kind = chrome::ButtonKind::Action;
                }
                // While a mutation runs the row stays put with its labels
                // unchanged and simply loses its focus ring — it reads as
                // "busy" without destroying focus identity. SELECT is
                // already gated on !mut_in_flight_ in handle_input.
                const auto rect = chrome::draw_button(
                    r, bx, brow_y, buttons_[i].label, kind,
                    /*focused=*/!busy && static_cast<int>(i) == focus_);
                bx = rect.x + rect.w + chrome::kPad3;
                ++drawn;
            }
            // A focused button that was never drawn is an invisible
            // affordance: SELECT would fire something the user cannot see.
            // Clamp onto the last button that actually made it onto the
            // screen (render() already clamps season_page_ the same way).
            if (focus_ >= drawn) focus_ = drawn > 0 ? drawn - 1 : 0;
        }
    }

    // ALWAYS LAST on every path. The Toast is NOT drawn here: main.cpp owns
    // the single app-wide Toast::render in the correct projection.
    chrome::draw_footer_hints(r, screen_w, screen_h, {
        {chrome::HintIcon::Btn1Yellow,
         overflow ? "Seasons \xE2\x86\x90" : "\xE2\x80\x94"},
        {chrome::HintIcon::Btn2Red, "Exit"},
        {chrome::HintIcon::Btn3Green,
         overflow ? "Seasons \xE2\x86\x92" : "\xE2\x80\x94"},
        {chrome::HintIcon::Btn4Black, "Back"},
        {chrome::HintIcon::RotaryNav,
         buttons_.empty() ? "\xE2\x80\x94" : "Choose"},
        {chrome::HintIcon::RotaryPress,
         buttons_.empty() ? "\xE2\x80\x94" : "Select"},
    });
}

}  // namespace media_browser::ui
