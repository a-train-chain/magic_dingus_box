#include "media_browser/ui/series_detail_screen.h"
#include "media_browser/ui/worker_pool.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "app/redraw_gate.h"
#include "media_browser/library/watch_store.h"
#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/season_delete.h"
#include "media_browser/series_mutations.h"
#include "media_browser/service_gate.h"
#include "media_browser/sonarr/sonarr_client.h"
#include "media_browser/ui/mb_chrome.h"
#include "media_browser/ui/mb_ui_utils.h"
#include "media_browser/ui/season_choice.h"
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
                                       bool sonarr_configured,
                                       library::WatchStore* watch)
    : sonarr_(sonarr), tmdb_(tmdb), qbit_(qbit), watch_(watch),
      sonarr_configured_(sonarr_configured) {}

SeriesDetailScreen::~SeriesDetailScreen() {
    // Invalidate every in-flight publish BEFORE joining anything: a worker
    // that finishes between the bump and its join sees a stale generation
    // and drops its result instead of writing into a half-destroyed object.
    // BOTH generations must move — fetch_gen_ gates the two load workers,
    // poll_gen_ gates the re-poll, and they publish into the SAME pending_.
    fetch_gen_.fetch_add(1);
    poll_gen_.fetch_add(1);
    // A season-end gate worker may be mid-wait (up to 90 s): cancel it so
    // the workers_ join below costs at most one in-flight ping.
    if (deferred_start_.has_value() && deferred_start_->cancel)
        deferred_start_->cancel->store(true, std::memory_order_release);
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
    //
    // Season delete (2026-08-13) raised this ceiling further: its
    // AutoRedownloadGuard adds one config GET plus, on a defeated restore,
    // up to 4 more PUTs (the disable + 3 retry attempts inside
    // AutoRedownloadGuard::restore) — up to ~25 s beyond the calls the
    // stage itself already makes, each still bounded by timeout_secs. The
    // guard's destructor is a same-object backstop, not a second retry
    // round: once run_delete_season's own restore_guard() call has resolved
    // (succeeded or exhausted its retries), restore()'s restored_ latch
    // makes the destructor's call a no-op, so this worker cannot spend two
    // full retry rounds back to back.
}

void SeriesDetailScreen::set_tmdb_id(int tmdb_id) {
    if (tmdb_id == tmdb_id_) return;
    tmdb_id_ = tmdb_id;
    needs_refresh_ = true;
}

void SeriesDetailScreen::enter() {
    // Take the season-end intent BEFORE a possible reload: fetch() clears
    // it, and the only setter (Playback -> SeriesDetail) never changes the
    // series, so a same-series reload here must not silently swallow it.
    // The deferral re-validates everything against fresh Sonarr data anyway.
    const std::optional<int> intent = pending_intent_next_season_;
    pending_intent_next_season_.reset();
    if (needs_refresh_) {
        needs_refresh_ = false;
        fetch();
    } else if (tmdb_id_ > 0) {
        // UNCONDITIONAL watch-state re-join on same-id re-entry. The
        // Playback->SeriesDetail return never calls set_tmdb_id, so
        // needs_refresh_ stays false and no fetch fires — without this
        // join the page would show pre-playback ✓/▶ glyphs and a stale
        // PlayNextUp label until the next 9 s poll drains. Cheap indexed
        // read; WatchStore is main-thread-only and enter() runs on the
        // render thread, so the direct read is legal.
        if (watch_ != nullptr) episode_watch_ = watch_->series_watch(tmdb_id_);
        // A chooser left open when the page was last shown (the global exit
        // modal can take the user away mid-choice) must not greet them with
        // a half-finished choice: re-entry starts idle.
        season_chooser_.cancel();
        rebuild_rows();
        rebuild_buttons();
    }
    // Season-end card intent ("Start Season N" pressed during playback).
    // NOT started here: on a FullPause box Sonarr's container is still
    // restarting at this instant (leave() only queued the resume), and the
    // page's rows are a pre-playback snapshot. Starting now failed with
    // "couldn't monitor season" or refused with "didn't apply" for a season
    // that was downloadable. The deferral holds it until Sonarr answers and
    // a fresh record lands — see step_deferred_season_start.
    if (intent.has_value()) begin_deferred_season_start(*intent);
}

void SeriesDetailScreen::leave() {
    if (deferred_start_.has_value()) {
        const auto& d = *deferred_start_;
        drop_deferred_season_start(
            d.title + ": Season " + std::to_string(d.season) +
            " not started \xE2\x80\x94 open the show again to start it");
    }
}

void SeriesDetailScreen::begin_deferred_season_start(int season) {
    // A previous deferral (only reachable by two card presses in a row) is
    // superseded silently: this one is the user's latest word.
    drop_deferred_season_start({});
    reap_finished_workers();
    DeferredSeasonStart d;
    d.season = season;
    d.tmdb_id = tmdb_id_;
    d.title = detail_.has_value() ? detail_->title : std::string("This series");
    d.gate = std::make_shared<std::atomic<int>>(0);
    d.cancel = std::make_shared<std::atomic<bool>>(false);
    d.began_at = std::chrono::steady_clock::now();
    try {
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::thread t([this, gate = d.gate, cancel = d.cancel, done]() {
            DoneFlag df{done};
            run_guarded("season-end service gate", [&] {
                // No MovieQuietMode is reachable from this screen, so the
                // gate runs its ping phase only. That is still correct
                // here: the card is pressed at the END of an episode, so
                // the session's pause completed long ago, and a stopped
                // container cannot answer a status request.
                ServiceGateHooks hooks;
                hooks.ping = [this] { return sonarr_.get_status().has_value(); };
                hooks.cancelled = [cancel] {
                    return cancel->load(std::memory_order_acquire);
                };
                const GateResult res = wait_for_service(hooks);
                if (res == GateResult::Ready) {
                    gate->store(1, std::memory_order_release);
                } else if (res == GateResult::TimedOut) {
                    gate->store(2, std::memory_order_release);
                }
            });
        });
        workers_.push_back(FetchWorker{std::move(t), std::move(done)});
    } catch (const std::system_error& e) {
        spdlog::warn("[SeriesDetail] season-end gate spawn failed: {}",
                     e.what());
        ::ui::Toast::show("Season update didn't apply \xE2\x80\x94 try from "
                          "this screen");
        return;
    }
    spdlog::info("[SeriesDetail] season-end intent: Season {} for '{}' held "
                 "until Sonarr answers", season, d.title);
    deferred_start_ = std::move(d);
}

void SeriesDetailScreen::drop_deferred_season_start(const std::string& toast) {
    if (!deferred_start_.has_value()) return;
    if (deferred_start_->cancel)
        deferred_start_->cancel->store(true, std::memory_order_release);
    deferred_start_.reset();
    if (!toast.empty()) ::ui::Toast::show(toast);
}

void SeriesDetailScreen::step_deferred_season_start() {
    if (!deferred_start_.has_value()) return;
    auto& d = *deferred_start_;
    const auto now = std::chrono::steady_clock::now();
    const int g = d.gate->load(std::memory_order_acquire);
    const DeferredGate gate = g == 1   ? DeferredGate::Ready
                              : g == 2 ? DeferredGate::TimedOut
                                       : DeferredGate::Pending;
    if (gate == DeferredGate::Ready && !d.gate_seen_ready) {
        d.gate_seen_ready = true;
        d.answers_at_ready = sonarr_answers_;
        d.settle_deadline =
            now + std::chrono::milliseconds(kDeferredSettleMs);
        // Ask for the fresh answer NOW rather than at the next 9 s poll.
        // A page whose Sonarr half never loaded has no poll to bring
        // forward, so it reloads — fetch() keeps this deferral (same id).
        if (in_library_ && sonarr_ok_ && series_.has_value() &&
            series_->sonarr_id > 0) {
            last_poll_at_ = {};
        } else {
            fetch();
        }
    }
    DeferredStartInputs in;
    in.gate = gate;
    in.fresh_answer =
        d.gate_seen_ready && sonarr_answers_ > d.answers_at_ready;
    in.has_series =
        in_library_ && series_.has_value() && series_->sonarr_id > 0;
    in.settled = series_settled_;
    in.mutation_in_flight = mut_in_flight_.load();
    const auto elig = eligible_seasons(rows_);
    in.want_eligible =
        std::find(elig.begin(), elig.end(), d.season) != elig.end();
    in.past_deadline = d.gate_seen_ready && now >= d.settle_deadline;
    switch (decide_deferred_season_start(in)) {
        case DeferredStartStep::Wait:
            if (!d.announced &&
                now - d.began_at >=
                    std::chrono::milliseconds(kDeferredAnnounceMs)) {
                d.announced = true;
                ::ui::Toast::show(d.title + ": starting Season " +
                                  std::to_string(d.season) +
                                  " once services are back\xE2\x80\xA6");
            }
            return;
        case DeferredStartStep::Start: {
            // Honour exactly the season the card offered (not the lowest
            // unmonitored one: that refused it on any show with a deleted
            // earlier season — emptied GoT: finish S5, offered S6, refused).
            const int want = d.season;
            drop_deferred_season_start({});
            start_season_download(want);
            return;
        }
        case DeferredStartStep::Drifted:
            drop_deferred_season_start(
                "Season update didn't apply \xE2\x80\x94 try from this screen");
            return;
        case DeferredStartStep::ServicesDown: {
            const std::string msg =
                d.title + ": Sonarr didn't come back \xE2\x80\x94 Season " +
                std::to_string(d.season) +
                " not started; try from this screen";
            drop_deferred_season_start(msg);
            return;
        }
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
    season_chooser_.cancel();
    navigate_back_ = false;
    // Task 8's poll gate must not inherit series A's timestamp — it would
    // delay series B's first poll by a full interval.
    last_poll_at_ = {};
    // Episode picker state (Task 6): a new load starts on the season page
    // with no episodes, no watch join, and no armed play/intent leftovers.
    // The in-flight episode worker (if any) was invalidated by the gen bump
    // above; its publish gen-mismatches and drops.
    region_ = DetailRegion::Seasons;
    episodes_.clear();
    episode_watch_.clear();
    episodes_done_ = episodes_ok_ = false;
    last_episode_file_total_ = -1;
    episodes_season_ = 0;
    episode_focus_ = 0;
    episode_page_ = 0;
    episode_page_count_ = 1;
    season_focus_ = -1;
    pending_play_index_ = -1;
    navigate_playback_ = false;
    // The delete row's confirm belonged to the OLD series' season list. A
    // latched arm would make the first press on the new page a confirm.
    season_del_armed_ = false;
    season_del_inflight_ = false;
    pending_intent_next_season_.reset();
    // A held season-end start survives a reload of the SAME series (the
    // deferral itself reloads a page whose Sonarr half never loaded); a
    // different series is a new world. leave() already said so out loud.
    if (deferred_start_.has_value() && deferred_start_->tmdb_id != tmdb_id_)
        drop_deferred_season_start({});
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
    pending_.sonarr_fresh = lib.has_value();
    pending_.in_library = match.has_value();
    if (match.has_value()) {
        pending_.settled = record_refreshed(*match);
        pending_.has_settled = true;
    }
    pending_.series = std::move(match);
    pending_.quality_defs = std::move(defs);
    pending_ready_.store(true, std::memory_order_release);
}

void SeriesDetailScreen::maybe_fetch_episodes() {
    // RENDER THREAD ONLY, called from apply_pending() after a publish landed
    // series_. Spawning from run_series_poll instead would race the
    // render-thread-only workers_ vector — the brief's rule (b).
    if (!series_.has_value() || series_->sonarr_id <= 0) return;
    // Cheap refetch trigger: the per-season file-count total. Compared HERE,
    // on the render thread, against the total at the last accepted spawn —
    // a moved total means an import/delete landed and the picker's has_file
    // facts are stale. -1 (never fetched this load / retry after a failed
    // publish) never equals a real total, so the first sight of a sonarr_id
    // spawns the initial fetch through the same gate.
    int total = 0;
    for (const auto& s : series_->seasons) {
        if (s.season_number == 0) continue;
        total += s.episode_file_count;
    }
    if (total == last_episode_file_total_) return;
    // Double-spawn guard — the poll_inflight_ idiom. On CAS failure the
    // total is deliberately NOT recorded, so the next drain retries.
    bool expected = false;
    if (!episodes_inflight_.compare_exchange_strong(expected, true)) return;
    // Capture the generation WITHOUT bumping: fetch() is the ONLY bumper of
    // fetch_gen_. A bump here would invalidate any in-flight tmdb/sonarr
    // worker for the SAME series — their publishes would gen-mismatch and
    // the page would wedge in Loading (the brief's rule (a)).
    const uint64_t gen = fetch_gen_.load();
    reap_finished_workers();
    try {
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::thread t(&SeriesDetailScreen::run_episodes_fetch, this, gen,
                      series_->sonarr_id, done);
        workers_.push_back(FetchWorker{std::move(t), std::move(done)});
        last_episode_file_total_ = total;
    } catch (const std::system_error& e) {
        spdlog::warn("[SeriesDetail] episode worker spawn failed: {}",
                     e.what());
        episodes_inflight_.store(false, std::memory_order_release);
        // last_episode_file_total_ untouched — the next drain retries.
    }
}

void SeriesDetailScreen::run_episodes_fetch(
        uint64_t gen, int sonarr_id, std::shared_ptr<std::atomic<bool>> done) {
    DoneFlag df{done};
    // Clears the in-flight flag on EVERY exit path, gen-mismatch included —
    // run_series_poll's InflightGuard, verbatim rationale.
    struct InflightGuard {
        std::atomic<bool>& flag;
        ~InflightGuard() { flag.store(false, std::memory_order_release); }
    } inflight_guard{episodes_inflight_};
    auto eps = sonarr_.get_episodes_checked(sonarr_id);
    std::lock_guard<std::mutex> lk(pending_mtx_);
    // Recheck under the lock — a worker that passed a pre-lock check could
    // be descheduled across fetch()'s bump-and-clear and publish stale data
    // into the new series' pending_.
    if (gen != fetch_gen_.load()) return;  // preempted — discard
    pending_.episodes_done = true;
    pending_.episodes_ok = eps.has_value();
    if (eps.has_value()) pending_.episodes = std::move(*eps);
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
        // The season-end deferral's "has a fresh answer landed?" signal.
        if (p.sonarr_fresh) ++sonarr_answers_;
        in_library_ = p.in_library;
        if (p.series.has_value()) series_ = std::move(p.series);
        if (p.has_settled) series_settled_ = p.settled;
        if (!p.quality_defs.empty())
            mb_per_min_ = pick_preferred_mb_per_min(p.quality_defs);
    }
    if (p.has_downloading) downloading_seasons_ = std::move(p.downloading);
    if (p.episodes_done) {
        episodes_done_ = true;
        episodes_ok_ = p.episodes_ok;
        if (p.episodes_ok) {
            episodes_ = std::move(p.episodes);
        } else {
            // nullopt = Sonarr never answered. The region renders the outage
            // line; arming a retry (the -1 sentinel) lets the NEXT poll
            // drain respawn the fetch instead of wedging on the outage
            // until a file count happens to move.
            last_episode_file_total_ = -1;
        }
    }
    // Watch-state join on EVERY drain (the brief's rule (c)): checkpoints
    // written during playback and EOS watched-marks land in the store from
    // main.cpp — this page's ✓/▶ glyphs and the PlayNextUp label must track
    // them without waiting for anything else to change. Render thread;
    // WatchStore is main-thread-only, so the direct read is legal.
    if (watch_ != nullptr && tmdb_id_ > 0)
        episode_watch_ = watch_->series_watch(tmdb_id_);
    rebuild_rows();
    rebuild_buttons();
    // AFTER series_ landed: initial episode fetch / file-count refetch.
    maybe_fetch_episodes();
}

void SeriesDetailScreen::rebuild_rows() {
    if (!detail_.has_value()) {
        rows_.clear();
        return;
    }
    rows_ = merge_season_rows(detail_->seasons,
                              series_.has_value() ? &*series_ : nullptr,
                              downloading_seasons_);
    // A shrink must not leave the season ring pointing past the end (SELECT
    // would index garbage). -1 = the ring is on the action row.
    if (season_focus_ >= static_cast<int>(rows_.size()))
        season_focus_ = static_cast<int>(rows_.size()) - 1;
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
    in.primary_season = suggested_season(rows_, episode_watch_);
    // Chooser open: fresh rows may have moved the candidates under it (a
    // poll flipped a season to Downloading) — snap or cancel, then label the
    // primary button with the season being chosen and its size estimate.
    season_chooser_.revalidate(eligible_seasons(rows_));
    if (const auto cur = season_chooser_.current()) {
        std::vector<SeasonRow> one;
        for (const auto& r : rows_)
            if (r.season_number == *cur) one.push_back(r);
        const int runtime = (in_library_ && series_.has_value())
                                ? series_->runtime_minutes : 0;
        in.primary_label_override = chooser_label(
            *cur, estimate_remaining_bytes(one, runtime, mb_per_min_),
            /*estimated=*/runtime <= 0);
    }
    // PlayNextUp inputs (Task 6): evidence-based — next_up's current==nullptr
    // form skips watched episodes and only ever returns one WITH a file, so
    // the button can never promise an episode that cannot start. "First"
    // (label "Start watching") = no watched flag and no resumable position
    // anywhere in the series' watch map.
    const EpisodeInfo* nu = episodes_.empty()
        ? nullptr
        : next_up<EpisodeInfo>(episodes_, episode_watch_, nullptr);
    in.has_next_up = nu != nullptr;
    if (nu != nullptr) {
        in.next_up_season = nu->season_number;
        in.next_up_episode = nu->episode_number;
        bool any_progress = false;
        for (const auto& kv : episode_watch_) {
            if (kv.second.watched ||
                is_resumable_position(kv.second.position_s,
                                      kv.second.duration_s)) {
                any_progress = true;
                break;
            }
        }
        in.next_up_is_first = !any_progress;
    }
    in.remove_pending = remove_pending_;
    in.whole_armed = whole_armed_;
    in.whole_estimate_bytes = whole_estimate_bytes_;
    if (focus_ >= 0 && focus_ < static_cast<int>(buttons_.size()))
        in.prev_focus_action = buttons_[static_cast<size_t>(focus_)].action;
    // Was the ring's current position FORCED by the layout rather than chosen?
    // It was iff the row we are replacing held nothing but Remove/ConfirmRemove
    // (canonicalized — they are one button in two states). decide_action_row
    // drops the identity preservation in that case; see its comment.
    in.prev_row_remove_only =
        !buttons_.empty() &&
        std::all_of(buttons_.begin(), buttons_.end(), [](const ActionButton& b) {
            return canonical_action(b.action) == Action::Remove;
        });

    ActionRow row = decide_action_row(in);
    // The chooser lives ON the primary button. If this rebuild left focus
    // anywhere else (the button vanished — record went unsettled — or
    // identity moved focus), the choice is no longer on screen: cancel it
    // and relabel, so a later SELECT elsewhere can never act on it.
    if (season_chooser_.choosing) {
        const bool on_primary =
            row.focus >= 0 && row.focus < static_cast<int>(row.buttons.size()) &&
            (row.buttons[static_cast<size_t>(row.focus)].action ==
                 Action::AddSeason ||
             row.buttons[static_cast<size_t>(row.focus)].action ==
                 Action::NextSeason);
        if (!on_primary) {
            season_chooser_.cancel();
            in.primary_label_override.reset();
            row = decide_action_row(in);
        }
    }
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
    // The picker's delete row leaves "Removing season…" on EVERY verdict —
    // success, abort or throw — and whichever page is on screen now, for the
    // same reason mut_in_flight_ is cleared unconditionally above. (A page
    // change already cleared it in fetch(); this covers the stay-put case,
    // which is the common one.)
    season_del_inflight_ = false;

    std::string toast;
    std::optional<Series> fresh;
    bool fresh_settled = true;
    bool removed = false;
    bool season_removed = false;
    int season_number = 0;
    bool have_verdict = false;
    DiskVerdict verdict = DiskVerdict::Block;
    int64_t estimate = 0;
    std::optional<int> start_season;
    std::string start_title;
    {
        std::lock_guard<std::mutex> lk(mut_mtx_);
        toast = std::move(mut_toast_);
        mut_toast_.clear();
        fresh = std::move(mut_series_);
        mut_series_.reset();
        start_season = mut_start_season_;
        mut_start_season_.reset();
        start_title = std::move(mut_start_title_);
        mut_start_title_.clear();
        fresh_settled = mut_settled_;
        mut_settled_ = true;
        removed = mut_removed_;
        mut_removed_ = false;
        season_removed = mut_season_removed_;
        mut_season_removed_ = false;
        season_number = mut_season_number_;
        mut_season_number_ = 0;
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
        // A dropped `removed` is an outcome that outlives the page it was
        // started from: the Sonarr record is gone for good. Without this
        // flag, re-entering that same tmdb_id hits set_tmdb_id's same-id
        // no-op and enter()'s short-circuit, and the page repaints its cached
        // pre-remove snapshot — a "Remove" button aimed at a record Sonarr no
        // longer knows about. DetailScreen's drain_remove_result sets exactly
        // this for exactly this reason.
        //
        // A dropped `season_removed` is the SAME class of fact — that season's
        // files are gone on disk — reachable through the A→B→A gen mismatch
        // (id equal, load different). Cached pre-delete rows would otherwise
        // paint file counts and ✓ glyphs for episodes that no longer exist,
        // and offer to play them.
        if (removed || season_removed) needs_refresh_ = true;
        // A chosen-season add whose start was never run: the series IS in
        // Sonarr now, with nothing monitored. Starting it against a page
        // that is not on screen would be the unasked-for mutation, so say
        // what is left to do instead of dropping it silently. The page's
        // pre-add snapshot is also stale now — refresh on the way back.
        if (start_season.has_value()) {
            needs_refresh_ = true;
            ::ui::Toast::show((start_title.empty() ? std::string("This series")
                                                   : start_title) +
                              ": added \xE2\x80\x94 open the show again to "
                              "start Season " +
                              std::to_string(*start_season));
        }
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
    if (season_removed) {
        // ONE season is gone; the series record is not. The user stays on this
        // page — but the episode picker they are standing in was listing a
        // season whose files no longer exist, so hand navigation back to the
        // season list and reload the page from Sonarr.
        //
        // A full fetch() rather than a patched row: the season's file counts,
        // the episode list, and every clamp/page in the picker are all stale
        // at once, and fetch() is exactly the reset for those (it is what
        // enter() calls for needs_refresh_). needs_refresh_ is cleared, not
        // set, because the reload happens HERE — leaving it armed would make
        // the next enter() (a return from Playback) reload a second time and
        // blink the page back to Loading for no reason.
        //
        // RETURNS rather than falling through: fetch() has already cleared
        // buttons_ (a SELECT during Loading must be a structural no-op) and
        // reset last_poll_at_, and the rebuild_buttons() below would undo the
        // first of those.
        spdlog::info("[SeriesDetail] season {} removed for tmdb:{} — reloading "
                     "the page", season_number, tmdb_id_);
        region_ = DetailRegion::Seasons;
        needs_refresh_ = false;
        fetch();
        return;
    }
    last_poll_at_ = {};  // Task 8: refresh badges next frame, not in 9 s
    rebuild_buttons();
    // Chosen-season add (Season > 1), second half: the record the worker
    // published was applied above (fresh -> series_, rows rebuilt), so the
    // shared single-season path can run against it. Re-checked against the
    // FRESH rows — the record may show that season on disk or in flight
    // already (the find-existing branch), and starting it then would be a
    // redundant search under a misleading toast.
    if (start_season.has_value()) {
        const auto elig = eligible_seasons(rows_);
        if (std::find(elig.begin(), elig.end(), *start_season) != elig.end()) {
            start_season_download(*start_season);
        } else {
            ::ui::Toast::show("Season " + std::to_string(*start_season) +
                              " isn't available to download");
        }
    }
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
    // The episode picker's delete row: its 4 s window, PLUS every structural
    // disarm in one sweep. Focus moved off the row, BTN4 took the region back
    // to Seasons, or a poll landed with the season's files gone — all three
    // read as "not focused" here, and this runs before render() every frame
    // (main.cpp: input, then update, then render), so the armed label can
    // never be painted on a row that no longer owns the ring. Deliberately
    // NOT part of `changed`: the row is not an action-row button, so there is
    // nothing for rebuild_buttons() to do.
    if (season_del_armed_ &&
        (std::chrono::duration_cast<std::chrono::milliseconds>(
             now - season_del_armed_at_).count() > kSeasonDelConfirmMs ||
         !season_delete_focused())) {
        season_del_armed_ = false;
    }
    if (changed) rebuild_buttons();
}

void SeriesDetailScreen::publish_mutation(SeriesMutationOutcome out) {
    // WORKER thread, the last act of a run_* body (series_mutations.h). Only
    // one mutation runs at a time and drain_mutation resets every one of
    // these fields before the next can spawn, so each holds its reset value
    // here — and SeriesMutationOutcome's defaults ARE those reset values. A
    // field the outcome leaves alone therefore publishes exactly what the
    // inline worker published by not writing it.
    std::lock_guard<std::mutex> lk(mut_mtx_);
    mut_toast_ = std::move(out.toast);
    mut_series_ = std::move(out.series);
    mut_settled_ = out.settled;
    mut_removed_ = out.removed;
    mut_start_season_ = out.start_season;
    mut_start_title_ = std::move(out.start_title);
    // mut_verdict_ is the one field the drain does not reset (it is read
    // only under mut_have_verdict_), so it is written only with a verdict.
    if (out.have_verdict) {
        mut_have_verdict_ = true;
        mut_verdict_ = out.verdict;
        mut_estimate_ = out.estimate;
    }
}

void SeriesDetailScreen::start_season_download(int season) {
    // RENDER thread. The one entry point for "download this one season":
    // the action row's "Download Season N" and the season list's SELECT on
    // a season with nothing on disk both land here.
    // spawn_mutation drops a request made while one is running; say so up
    // front instead of toasting "starting..." for a start that never runs.
    if (mut_in_flight_.load()) {
        ::ui::Toast::show("Still finishing the last action\xE2\x80\xA6");
        return;
    }
    const std::string title =
        detail_.has_value() ? detail_->title : std::string("This series");
    if (!in_library_ || !series_.has_value() || series_->sonarr_id <= 0) {
        // Reachable from the season list on a series that is not in the
        // library (rows_ are TMDB's there). Never a silent no-op — that
        // reads as a dead row.
        ::ui::Toast::show(title + ": not in your TV library yet \xE2\x80\x94 "
                          "use Add Season or Whole series\xE2\x80\xA6");
        return;
    }
    if (!series_settled_) {
        // The same race decide_action_row hides the add controls for, and
        // that the whole-series worker refuses to PUT seasons across: until
        // Sonarr has applied firstSeason, a season PUT can be overwritten
        // behind us and we would have promised a search for nothing.
        ::ui::Toast::show(title + ": still syncing \xE2\x80\x94 try again in "
                          "a moment");
        return;
    }
    const int sid = series_->sonarr_id;
    // The user got there first (pressed the same season by hand while the
    // season-end deferral was still waiting): the deferral would only find
    // the season in flight and toast a misleading "didn't apply" later.
    if (deferred_start_.has_value() && deferred_start_->season == season)
        drop_deferred_season_start({});
    // Same immediate-feedback rule as AddSeason.
    ::ui::Toast::show(title + ": starting Season " + std::to_string(season) +
                      "\xE2\x80\xA6");
    // The worker body (monitor, episode re-monitor, search, Quick Start,
    // re-read) and its toast live in series_mutations.cpp, unit-tested.
    spawn_mutation([this, sid, season, title]() {
        publish_mutation(run_start_season_download(sonarr_, sid, season, title));
    });
}

void SeriesDetailScreen::start_add_at_season(int season) {
    // RENDER thread. See the header: Season 1 keeps Sonarr's own
    // firstSeason + search; Season > 1 adds with nothing monitored and
    // starts the chosen season once the record is applied.
    const int id = tmdb_id_;
    const std::string title =
        detail_.has_value() ? detail_->title : std::string("This series");
    // Immediate press feedback (operator-reported): the row dims,
    // but the outcome toast can be ~13.5 s away (add_series' settle
    // ceiling) and a dim alone reads as "nothing happened". Same
    // precedent as whole-series press-1's "checking free space".
    ::ui::Toast::show(title + ": adding Season " + std::to_string(season) +
                      "\xE2\x80\xA6");
    // Season 1 vs Season > 1, the did-the-add-do-anything check and every
    // toast live in series_mutations.cpp (run_add_at_season), unit-tested.
    spawn_mutation([this, id, title, season]() {
        publish_mutation(run_add_at_season(sonarr_, id, season, title));
    });
}

void SeriesDetailScreen::dispatch_action(Action a) {
    // Pressing anything OTHER than the armed control disarms it first.
    if (a != Action::WholeSeries) whole_armed_ = false;
    if (a != Action::Remove && a != Action::ConfirmRemove) remove_pending_ = false;
    // Same rule for the episode picker's delete row. It lives in the OTHER
    // region, so it can never BE the control being pressed here — an action
    // row press is unconditionally "something else", and two armed
    // destructive confirms at once is the state to avoid.
    season_del_armed_ = false;
    // And the season chooser: every press routed here is a DIFFERENT
    // control (the primary button's own presses go through press_primary
    // in handle_input), so an open choice is abandoned, not carried along.
    season_chooser_.cancel();
    switch (a) {
        case Action::PlayNextUp: {
            // Re-derive at press time — the button's label was decided on an
            // earlier rebuild, and an EOS drain may have advanced next_up
            // since. nullptr here means the world moved (all watched, or the
            // file facts went stale): repaint the row honestly, no toast —
            // the refreshed row IS the answer.
            const EpisodeInfo* nu = episodes_.empty()
                ? nullptr
                : next_up<EpisodeInfo>(episodes_, episode_watch_, nullptr);
            if (nu == nullptr) {
                rebuild_buttons();
                break;
            }
            start_playback_for(static_cast<int>(nu - episodes_.data()));
            break;
        }
        case Action::AddSeason:
            // Reached only through the chooser's Fallthrough (no eligible
            // season to choose between) — the chooser's own confirm calls
            // start_add_at_season directly.
            start_add_at_season(
                suggested_season(rows_, episode_watch_).value_or(1));
            break;
        case Action::NextSeason: {
            const auto next = suggested_season(rows_, episode_watch_);
            if (!next.has_value()) break;
            // Every guard, the toast and the worker live in the shared helper:
            // the season list's SELECT starts the SAME flow for a season with
            // nothing on disk, and two copies of a monitor+re-monitor+search
            // sequence is how one of them silently loses the re-monitor.
            start_season_download(*next);
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
                // The add (pre-add only), season PUTs, episode re-monitor,
                // series search and the two-signal toast live in
                // series_mutations.cpp (run_whole_series), unit-tested.
                spawn_mutation([this, pre_add, id, title, sid, to_monitor]() {
                    publish_mutation(run_whole_series(sonarr_, pre_add, id, sid,
                                                      to_monitor, title));
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
            // Free space (Sonarr's /tv root folder, then the resolved host
            // path — two sources with DIFFERENT zero semantics), the verdict
            // and its toast live in series_mutations.cpp
            // (run_whole_series_preflight), unit-tested. A Block verdict is
            // published so drain_mutation does NOT arm.
            spawn_mutation([this, estimate, title]() {
                publish_mutation(
                    run_whole_series_preflight(sonarr_, estimate, title));
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
            // The orphan-proof sequence — checked queue read, one cancel per
            // download, checked history walk + qBit purge, remove_series —
            // and its abort toasts live in series_mutations.cpp
            // (run_remove_series), unit-tested.
            spawn_mutation([this, sid, title]() {
                publish_mutation(run_remove_series(sonarr_, qbit_, sid, title));
            });
            break;
        }
    }
}

void SeriesDetailScreen::start_playback_for(int index) {
    if (index < 0 || index >= static_cast<int>(episodes_.size())) return;
    const auto& ep = episodes_[static_cast<size_t>(index)];
    if (!ep.has_file) {
        ::ui::Toast::show("Not downloaded yet");
        return;
    }
    // The do_play precedent (detail_screen.cpp): resolve, then stat the HOST
    // path before promising playback — Sonarr's has_file is its container
    // view, and the drive can have moved/unmounted under it.
    const std::string host = ep.file_container_path.empty()
        ? std::string()
        : sonarr_.resolve_host_path(ep.file_container_path);
    std::error_code ec;
    if (host.empty() || !std::filesystem::exists(host, ec)) {
        ::ui::Toast::show("File missing on disk");
        return;
    }
    pending_play_index_ = index;
    navigate_playback_ = true;
}

std::vector<int> SeriesDetailScreen::season_episode_indices(int season) const {
    std::vector<int> idxs;
    for (size_t i = 0; i < episodes_.size(); ++i) {
        if (episodes_[i].season_number == season)
            idxs.push_back(static_cast<int>(i));
    }
    return idxs;
}

bool SeriesDetailScreen::season_delete_row_present() const {
    // Only inside the drill-down, and only on the frames the picker actually
    // paints episode rows: render_episode_region returns early on loading /
    // outage / "No episodes", and an armable row that was never drawn is the
    // invisible-affordance bug class both SELECT paths already guard. These
    // three predicates are exactly that function's bail conditions, so the
    // two can never disagree about whether the row is on screen.
    if (region_ != DetailRegion::Episodes) return false;
    if (!episodes_done_ || !episodes_ok_) return false;
    if (season_episode_indices(episodes_season_).empty()) return false;
    // Eligibility itself is Task 4's pure helper, fed from THIS season's
    // merged row — the same row the no-file suffix below reads, so the
    // "downloading" the row offers to cancel is the one the list shows.
    int files = 0;
    bool downloading = false;
    for (const auto& row : rows_) {
        if (row.season_number == episodes_season_) {
            files = row.episode_file_count;
            downloading = row.state == SeasonState::Downloading;
            break;
        }
    }
    return season_delete_row_exists(files, downloading);
}

bool SeriesDetailScreen::season_delete_focused() const {
    return season_delete_row_present() &&
           episode_focus_ ==
               static_cast<int>(season_episode_indices(episodes_season_).size());
}

int SeriesDetailScreen::episode_nav_count() const {
    return static_cast<int>(season_episode_indices(episodes_season_).size()) +
           (season_delete_row_present() ? 1 : 0);
}

SeriesDetailScreen::SeriesPlayTarget SeriesDetailScreen::get_play_target() {
    SeriesPlayTarget pt;
    const std::string series_title =
        detail_.has_value() ? detail_->title : std::string("Series");
    pt.series_title = series_title;
    pt.rows = rows_;
    pt.watch = episode_watch_;
    pt.episodes = episodes_;
    // Index-aligned host paths for the WHOLE vector — the end-of-episode
    // overlay's in-place advance loads episodes this screen never focused.
    // resolve_host_path is a pure, non-virtual string mapping (no HTTP), so
    // the render thread can afford one pass here.
    pt.host_paths.reserve(episodes_.size());
    for (const auto& e : episodes_) {
        pt.host_paths.push_back(
            (e.has_file && !e.file_container_path.empty())
                ? sonarr_.resolve_host_path(e.file_container_path)
                : std::string());
    }
    if (detail_.has_value()) {
        pt.year = detail_->year;
        pt.synopsis = detail_->overview;
        pt.poster_url = detail_->poster_path;
        // Up to 3 genre names joined with " · " (DetailScreen's precedent).
        for (size_t i = 0; i < detail_->genres.size() && i < 3; ++i) {
            if (i > 0) pt.genres += " \xC2\xB7 ";
            pt.genres += detail_->genres[i];
        }
    }
    // Identity is ALWAYS the TV ref — even on the (guarded-out) fallback
    // path below, a session must never be attributable to Movie/tmdb_id:
    // the two id spaces overlap completely.
    pt.identity = WatchIdentity{MediaRef{MediaKind::Tv, tmdb_id_}, 0, 0};
    const int idx = pending_play_index_;
    if (idx < 0 || idx >= static_cast<int>(episodes_.size()))
        return pt;  // no armed episode: host_path stays empty, caller-safe
    const auto& ep = episodes_[static_cast<size_t>(idx)];
    pt.host_path = pt.host_paths[static_cast<size_t>(idx)];
    pt.display_title = series_title + " \xE2\x80\x94 S" +
                       std::to_string(ep.season_number) + "E" +
                       std::to_string(ep.episode_number) + " \xC2\xB7 " +
                       ep.title;
    // Episode runtime, falling back to the series' per-episode figure
    // (sonarr_types.h documents runtime 0 as real for specials/unknown).
    pt.runtime_min = ep.runtime_minutes > 0
        ? ep.runtime_minutes
        : (series_.has_value() ? series_->runtime_minutes : 0);
    pt.identity.season = ep.season_number;
    pt.identity.episode = ep.episode_number;
    // Resume via the joined watch map — same rule as the movie path: only a
    // resumable position (>= 60 s, short of the watched threshold) carries.
    const auto it = episode_watch_.find(
        WatchKey{ep.season_number, ep.episode_number});
    if (it != episode_watch_.end() &&
        is_resumable_position(it->second.position_s, it->second.duration_s)) {
        pt.resume_position = it->second.position_s;
    }
    return pt;
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
        // ---- Episodes region: one season's episode list owns the input ----
        if (region_ == DetailRegion::Episodes) {
            // BTN4 returns to the SEASON page, never to origin_ — the
            // episode list is a drill-down inside this screen.
            if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
                region_ = DetailRegion::Seasons;
                // Leaving the region disarms the delete row: its confirm
                // belongs to a list that is no longer on screen.
                season_del_armed_ = false;
                continue;
            }
            // The trailing delete row is part of the SAME chain as the
            // episodes, so every clamp and page below counts with
            // episode_nav_count(), never with the raw episode count.
            const int n = episode_nav_count();
            if ((e.action == platform::InputAction::ROTATE ||
                 e.action == platform::InputAction::ROTATE_VERTICAL) &&
                e.delta != 0) {
                if (n == 0) continue;
                episode_focus_ = std::clamp(episode_focus_ + e.delta, 0, n - 1);
                // Page follows focus (render re-derives it too; this keeps
                // the indicator honest within the same frame's input burst).
                if (episode_per_page_ > 0)
                    episode_page_ = episode_focus_ / episode_per_page_;
                // Navigating OFF the delete row disarms it — the action
                // row's rule, so nobody can press-move-press their way into
                // a delete they were not looking at.
                if (!season_delete_focused()) season_del_armed_ = false;
                continue;
            }
            // BTN1 / BTN3 page by moving FOCUS a page at a time — page and
            // focus never diverge, so SELECT always fires a visible row.
            if (e.action == platform::InputAction::PREV && e.pressed) {
                if (n > 0 && episode_per_page_ > 0)
                    episode_focus_ =
                        std::max(0, episode_focus_ - episode_per_page_);
                if (!season_delete_focused()) season_del_armed_ = false;
                continue;
            }
            if (e.action == platform::InputAction::NEXT && e.pressed) {
                if (n > 0 && episode_per_page_ > 0)
                    episode_focus_ = std::min(
                        n - 1, episode_focus_ + episode_per_page_);
                if (!season_delete_focused()) season_del_armed_ = false;
                continue;
            }
            if (e.action == platform::InputAction::SELECT && e.pressed) {
                if (n == 0) continue;
                // per_page 0 = the canvas draws NO episode rows (CRT_NATIVE's
                // 640x480). Firing a row nobody can see is the invisible-
                // affordance bug class; the drill-down is a 720p+ affordance
                // like the season detail, and PlayNextUp still covers play.
                if (episode_per_page_ <= 0) continue;
                // Same global gate as the action row: one mutation worker,
                // and a silent no-op reads as a dead button.
                if (mut_in_flight_.load()) {
                    ::ui::Toast::show(
                        "Still finishing the last action\xE2\x80\xA6");
                    continue;
                }
                // ---- the trailing "Delete Season N…" row ----
                // Focus one past the last episode index IS the delete row;
                // the episode branch below never sees that index.
                if (season_delete_focused()) {
                    // Inert while the whole-series Remove confirm is armed or
                    // a season remove is already running. This is about not
                    // ARMING two destructive confirms at once (the mutation
                    // lane already serializes the workers themselves), and
                    // about the second press of the OTHER confirm never
                    // landing here.
                    //
                    // Both used to be one SILENT `continue`, against this
                    // block's own rule ("a silent no-op reads as a dead
                    // button", the mut_in_flight_ gate three lines up).
                    // Defence in depth, and no longer mute: with the spawn
                    // guard below, season_del_inflight_ implies
                    // mut_in_flight_, so that gate answers first — but the
                    // day the two ever come apart, a confirmed destructive
                    // press must still say something.
                    if (season_del_inflight_) {
                        ::ui::Toast::show(
                            "Still finishing the last action\xE2\x80\xA6");
                        continue;
                    }
                    if (remove_pending_) {
                        ::ui::Toast::show(
                            "Finish or cancel Remove first\xE2\x80\xA6");
                        continue;
                    }
                    if (!season_del_armed_) {
                        // Press 1: arm. Stamped HERE, on the render thread,
                        // so the 4 s window starts when the label changes.
                        season_del_armed_ = true;
                        season_del_armed_at_ = std::chrono::steady_clock::now();
                        continue;
                    }
                    // ---- Press 2 inside the window: confirmed ----
                    season_del_armed_ = false;
                    const std::string title = detail_.has_value()
                        ? detail_->title : std::string("This series");
                    if (!series_.has_value() || series_->sonarr_id <= 0) {
                        // Structurally unreachable (the row only exists when a
                        // merged Sonarr row reports files or a live download),
                        // but ConfirmRemove's precedent applies: never fall
                        // out of a confirmed destructive press in silence.
                        ::ui::Toast::show(title + ": series id unknown "
                                          "\xE2\x80\x94 try again once syncing "
                                          "finishes");
                        continue;
                    }
                    const int sid = series_->sonarr_id;
                    const int season = episodes_season_;
                    // How many files this season is KNOWN to have, read here
                    // on the render thread that owns rows_ — the same merged
                    // row season_delete_row_present() gates the row's very
                    // existence on. Stage (f) needs it to tell "Sonarr listed
                    // no files" (a contradiction: the row would not exist)
                    // apart from "this season genuinely has none" (legitimate
                    // — the row is also offered for a download-only season,
                    // where episode_file_count is 0).
                    int expected_files = 0;
                    for (const auto& row : rows_) {
                        if (row.season_number == season) {
                            expected_files = row.episode_file_count;
                            break;
                        }
                    }
                    // Set BEFORE the spawn so the row reads "Removing season…"
                    // on this very frame; cleared by drain_mutation on every
                    // verdict, and below if the spawn never took.
                    season_del_inflight_ = true;
                    spawn_mutation([this, sid, season, title,
                                    expected_files]() {
                        // The whole 7-stage sequence — unmonitor, history,
                        // AutoRedownloadGuard, cancel, mark-failed, purge,
                        // delete — lives in season_delete.cpp, where its
                        // abort rule and toast wording are unit-tested
                        // (tests/media_browser/test_season_delete.cpp).
                        SeasonDeleteInputs in;
                        in.title = title;
                        in.expected_files = expected_files;
                        const SeasonDeleteOutcome out = run_delete_season(
                            sonarr_, qbit_, sid, season, in);
                        // run_delete_season has already put Sonarr's
                        // auto-redownload back (it can spend ~15 s retrying
                        // the PUT) — never under mut_mtx_, which the render
                        // thread waits on in drain_mutation.
                        const std::string toast =
                            compose_season_delete_toast(out);
                        std::lock_guard<std::mutex> lk(mut_mtx_);
                        if (out.removed) {
                            mut_season_removed_ = true;
                            mut_season_number_ = season;
                        }
                        mut_toast_ = toast;
                    });
                    // spawn_mutation can decline (one at a time) or fail to
                    // start the thread, and NEITHER path ever sets mut_done_ —
                    // so the drain would never run and the row would sit on
                    // "Removing season…", inert, for the rest of the session.
                    // mut_in_flight_ is the one signal that says the worker
                    // actually took.
                    if (!mut_in_flight_.load()) season_del_inflight_ = false;
                    continue;
                }
                const auto idxs = season_episode_indices(episodes_season_);
                if (episode_focus_ < 0 ||
                    episode_focus_ >= static_cast<int>(idxs.size()))
                    continue;
                start_playback_for(idxs[static_cast<size_t>(episode_focus_)]);
                if (navigate_playback_) {
                    navigate_playback_ = false;
                    return Screen::Playback;
                }
                continue;
            }
            continue;  // everything else is inert inside the drill-down
        }
        // BTN4 (SETTINGS_MENU, black) — back to whoever opened us. With
        // the season chooser open, back closes the chooser instead: the
        // user is one level "inside" the primary button.
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            if (season_chooser_.choosing) {
                season_chooser_.cancel();
                rebuild_buttons();
                continue;
            }
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
            if (busy) continue;
            // The chooser owns rotation while open: it steps the season
            // (clamped, no wrap) and focus never moves — so it can never
            // leave the primary button by rotating.
            if (season_chooser_.choosing) {
                season_chooser_.step(e.delta);
                rebuild_buttons();
                continue;
            }
            // ONE navigation chain: season rows top-to-bottom, then the
            // action buttons left-to-right. season_focus_ == -1 means the
            // ring is on the button row (focus_), preserving every pre-Task-6
            // behavior when no season rows exist.
            // per_page 0 = the canvas draws NO season rows (CRT_NATIVE's
            // 640x480 clamps list_avail to zero in render()). Walking the
            // ring into an undrawn row is the invisible-affordance bug class
            // the episode-side SELECT already guards, so the chain holds
            // zero season rows there — action buttons only; PlayNextUp
            // stays the CRT play path.
            const int n_rows = season_per_page_ > 0
                ? static_cast<int>(rows_.size()) : 0;
            const int n_btns = static_cast<int>(buttons_.size());
            const int total = n_rows + n_btns;
            if (total == 0) continue;
            // A season ring at/past n_rows (stale after the CRT clamp or a
            // shrink) restarts from the visible button row instead.
            int pos = (season_focus_ >= 0 && season_focus_ < n_rows)
                ? season_focus_ : n_rows + focus_;
            // Clamp, do not wrap — DetailScreen's exact idiom, so the ends
            // of the chain feel like ends rather than teleporting focus.
            pos = std::clamp(pos + e.delta, 0, total - 1);
            if (pos < n_rows) {
                season_focus_ = pos;
                // The list page follows the ring so SELECT always targets a
                // visible row (render clamps again, belt-and-braces).
                if (season_per_page_ > 0)
                    season_page_ = season_focus_ / season_per_page_;
            } else {
                season_focus_ = -1;
                focus_ = pos - n_rows;
            }
            // Any navigation cancels BOTH pending confirms, so the user can
            // never press-move-press their way into a mutation they were not
            // looking at. (The season chooser is closed here too for
            // uniformity, though an open chooser consumed this rotate above.)
            if (whole_armed_ || remove_pending_ || season_chooser_.choosing) {
                whole_armed_ = false;
                remove_pending_ = false;
                season_chooser_.cancel();
                rebuild_buttons();
            }
            continue;
        }
        // BTN1 / BTN3 page the season list when it overflows. When the ring
        // is IN the list, it moves with the page (page-start row) so it can
        // never sit on an off-screen row; on the action row the pages browse
        // freely underneath, exactly as before.
        if (e.action == platform::InputAction::PREV && e.pressed) {
            // A page flip is navigation: it closes the season chooser.
            if (season_chooser_.choosing) {
                season_chooser_.cancel();
                rebuild_buttons();
            }
            if (season_page_ > 0) {
                --season_page_;
                if (season_focus_ >= 0 && season_per_page_ > 0)
                    season_focus_ = season_page_ * season_per_page_;
            }
            continue;
        }
        if (e.action == platform::InputAction::NEXT && e.pressed) {
            if (season_chooser_.choosing) {
                season_chooser_.cancel();
                rebuild_buttons();
            }
            if (season_page_ + 1 < season_page_count_) {
                ++season_page_;
                if (season_focus_ >= 0 && season_per_page_ > 0)
                    season_focus_ = std::min(
                        season_page_ * season_per_page_,
                        static_cast<int>(rows_.size()) - 1);
            }
            continue;
        }
        if (e.action == platform::InputAction::SELECT && e.pressed) {
            // The gate is GLOBAL (one mutation worker) but the dim is
            // series-scoped, so series B's row looks perfectly live while
            // series A's mutation finishes — up to ~14 s of presses landing
            // on nothing. A silent no-op reads as a dead button; say it.
            if (season_focus_ >= 0 &&
                season_focus_ < static_cast<int>(rows_.size())) {
                // Mirror of the episode-side per_page guard: on the
                // CRT_NATIVE canvas no season rows are drawn, so a season
                // ring here is invisible and SELECT would open a drill-down
                // the user never saw targeted (the invisible-affordance bug
                // class). Return the ring to the visible action row.
                if (season_per_page_ <= 0) {
                    season_focus_ = -1;
                    continue;
                }
                if (mut_in_flight_.load()) {
                    ::ui::Toast::show(
                        "Still finishing the last action\xE2\x80\xA6");
                    continue;
                }
                const auto& row = rows_[static_cast<size_t>(season_focus_)];
                // Openable when there is something to SEE or something to
                // STOP. The spec's eligibility rule has always been
                // "episode_file_count > 0 OR live queue rows"
                // (season_delete_row_exists encodes exactly that), but this
                // gate implemented only the first half — so the case the
                // delete row was BUILT for ("I can see the wrong-language pack
                // downloading; stop it") could not be reached at all: a
                // season with a download in flight and no files yet answered
                // "Not downloaded yet". The picker already renders fileless
                // episodes (dim, with a "· downloading" suffix), and the
                // delete worker already handles expected_files == 0.
                if (season_row_opens_picker(row)) {
                    // Open the drill-down. The episode fetch covers ALL
                    // seasons and ran at load; loading/outage/empty states
                    // render inside the region.
                    region_ = DetailRegion::Episodes;
                    episodes_season_ = row.season_number;
                    episode_focus_ = 0;
                    episode_page_ = 0;
                } else {
                    // Nothing on disk and nothing in flight. Offering the
                    // download is the useful answer: the direct, one-press
                    // path to THIS season (the action row's "Download Season
                    // N" proposes suggested_season and reaches any other
                    // eligible season only through its chooser). Every guard
                    // and the re-monitor live in the shared helper.
                    start_season_download(row.season_number);
                }
                continue;
            }
            if (buttons_.empty()) continue;
            if (mut_in_flight_.load()) {
                ::ui::Toast::show("Still finishing the last action\xE2\x80\xA6");
                continue;
            }
            if (focus_ >= 0 && focus_ < static_cast<int>(buttons_.size())) {
                const Action a = buttons_[static_cast<size_t>(focus_)].action;
                if (a == Action::AddSeason || a == Action::NextSeason) {
                    // The primary button is two presses (season_choice.h):
                    // press 1 opens the chooser on the suggested season,
                    // press 2 starts the chosen one. Pressing it is
                    // "something else" to any armed confirm — the same
                    // disarm dispatch_action does for every other button.
                    whole_armed_ = false;
                    remove_pending_ = false;
                    season_del_armed_ = false;
                    const PrimaryPress p =
                        press_primary(season_chooser_, rows_, episode_watch_);
                    if (p.kind == PrimaryPress::Kind::Opened) {
                        rebuild_buttons();
                        continue;
                    }
                    if (p.kind == PrimaryPress::Kind::Start) {
                        rebuild_buttons();
                        if (a == Action::NextSeason)
                            start_season_download(p.season);
                        else
                            start_add_at_season(p.season);
                        continue;
                    }
                    // Fallthrough: no eligible season to choose between —
                    // keep the pre-chooser behaviour (never a dead press).
                }
                dispatch_action(a);
            }
            // PlayNextUp arms the transition synchronously inside
            // dispatch_action (the navigate_back_ idiom, same-frame form).
            if (navigate_playback_) {
                navigate_playback_ = false;
                return Screen::Playback;
            }
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
        poll_worker_ = std::thread(
            [this, gen, id = series_->sonarr_id, ok = sonarr_ok_,
             lib = in_library_] {
                run_guarded("series poll",
                            [&] { run_series_poll(gen, id, ok, lib); });
            });
    } catch (const std::system_error& e) {
        spdlog::warn("[SeriesDetail] poll spawn failed: {}", e.what());
        poll_inflight_.store(false);
    }
}

void SeriesDetailScreen::run_series_poll(uint64_t gen, int sonarr_id,
                                         bool prev_sonarr_ok,
                                         bool prev_in_library) {
    // Clears the in-flight flag on EVERY exit path, the gen-mismatch discard
    // included. Without it, one future early return leaves poll_inflight_
    // stuck true and maybe_repoll_series() never polls again for the rest of
    // the session — silently, since the page still renders its last snapshot.
    struct InflightGuard {
        std::atomic<bool>& flag;
        ~InflightGuard() { flag.store(false, std::memory_order_release); }
    } inflight_guard{poll_inflight_};
    auto fresh = sonarr_.get_series(sonarr_id);
    // CHECKED queue read: nullopt = Sonarr did not answer THIS request,
    // which is independent of whether get_series just did. See
    // downloading_seasons_from_queue for the duplicate-search bug the
    // unchecked read caused.
    std::optional<std::unordered_set<int>> downloading;
    if (fresh.has_value()) {
        downloading = downloading_seasons_from_queue(sonarr_.get_queue_checked(),
                                                     sonarr_id);
    }
    std::lock_guard<std::mutex> lk(pending_mtx_);
    // Recheck under the lock — a worker that passed a pre-lock check could
    // be descheduled across fetch()'s bump-and-clear and publish stale data
    // into the new series' pending_.
    if (gen != poll_gen_.load()) return;  // preempted — discard
    pending_.sonarr_done = true;
    // A poll is ADVISORY: a transient blip must not flip the page to
    // SonarrUnreachable under the user. Success proves reachability;
    // failure leaves the original fetch's verdict standing. Both priors
    // arrive by value so nothing here reads render-thread state.
    pending_.sonarr_ok = fresh.has_value() ? true : prev_sonarr_ok;
    pending_.sonarr_fresh = fresh.has_value();
    if (fresh.has_value()) {
        pending_.in_library = true;
        // The settle signal for an existing record: has Sonarr ever
        // actually refreshed it? This is what un-hides the add controls
        // ~9 s after an unsettled add, with no user action.
        pending_.settled = record_refreshed(*fresh);
        pending_.has_settled = true;
        pending_.series = std::move(fresh);
        // The queue snapshot is published only when the queue read ITSELF
        // answered. It used to ride on get_series' verdict alone, but the
        // two are separate requests: a queue read that failed after a good
        // series read published an empty set. A missing snapshot leaves
        // has_downloading false, so apply_pending() keeps the existing
        // badges standing rather than clearing them on no evidence.
        if (downloading.has_value()) {
            pending_.downloading = std::move(*downloading);
            pending_.has_downloading = true;
        }
    } else {
        pending_.in_library = prev_in_library;
        // has_downloading stays false — apply_pending() leaves the existing
        // badges standing rather than clearing them on no evidence.
    }
    pending_ready_.store(true, std::memory_order_release);
}

uint64_t SeriesDetailScreen::redraw_signature() const {
    app::ContentSignature sig;
    sig.add(static_cast<uint64_t>(tmdb_done_));
    sig.add(static_cast<uint64_t>(tmdb_ok_));
    sig.add(static_cast<uint64_t>(sonarr_done_));
    sig.add(static_cast<uint64_t>(sonarr_ok_));
    sig.add(static_cast<uint64_t>(in_library_));
    sig.add(static_cast<uint64_t>(series_settled_));
    sig.add(static_cast<uint64_t>(detail_.has_value()));
    sig.add(static_cast<uint64_t>(series_.has_value()));
    sig.add(static_cast<uint64_t>(episodes_done_));
    sig.add(static_cast<uint64_t>(episodes_ok_));
    sig.add(static_cast<uint64_t>(episodes_.size()));
    sig.add(static_cast<uint64_t>(episode_watch_.size()));
    sig.add(static_cast<uint64_t>(mut_in_flight_.load(std::memory_order_acquire)));
    sig.add(static_cast<uint64_t>(season_del_inflight_));
    sig.add(static_cast<uint64_t>(season_del_armed_));
    sig.add(static_cast<uint64_t>(whole_armed_));
    sig.add(static_cast<uint64_t>(remove_pending_));
    sig.add(static_cast<uint64_t>(season_chooser_.choosing));
    sig.add(static_cast<uint64_t>(season_chooser_.index));
    sig.add(static_cast<uint64_t>(region_));
    sig.add(static_cast<uint64_t>(focus_));
    sig.add(static_cast<uint64_t>(season_focus_));
    sig.add(static_cast<uint64_t>(season_page_));
    sig.add(static_cast<uint64_t>(episodes_season_));
    sig.add(static_cast<uint64_t>(episode_focus_));
    sig.add(static_cast<uint64_t>(episode_page_));
    sig.add(static_cast<uint64_t>(downloading_seasons_.size()));
    sig.add(static_cast<uint64_t>(buttons_.size()));
    for (const auto& b : buttons_) {
        sig.add(static_cast<uint64_t>(b.action));
        sig.add(b.label);
    }
    sig.add(static_cast<uint64_t>(rows_.size()));
    for (const auto& row : rows_) {
        sig.add(static_cast<uint64_t>(row.season_number));
        sig.add(static_cast<uint64_t>(row.episode_count));
        sig.add(static_cast<uint64_t>(row.episode_file_count));
        sig.add(static_cast<uint64_t>(row.monitored));
        sig.add(static_cast<uint64_t>(row.state));
    }
    return sig.value();
}

void SeriesDetailScreen::update() {
    drain_mutation();
    expire_confirms();
    apply_pending();
    // After the drain (it may carry the fresh answer the deferral waits
    // for) and before the re-poll (a Ready gate brings the poll forward to
    // this very frame).
    step_deferred_season_start();
    maybe_repoll_series();
}

void SeriesDetailScreen::render_episode_region(::ui::Renderer& r, int screen_w,
                                               int body_x, int list_top,
                                               int list_bottom,
                                               bool& ep_overflow) {
    const auto& th = r.mb_theme();
    // Region title: which season is open.
    r.mb_draw_text("Season " + std::to_string(episodes_season_) +
                       " \xC2\xB7 Episodes",
                   static_cast<float>(body_x),
                   static_cast<float>(list_top + 22), kRowFontPx, th.accent);
    const int rows_top = list_top + kRowH;
    // Loading / outage / empty. The outage copy comes through the SAME
    // resolver as the page-level banner so the two lines can never drift.
    const char* placeholder = nullptr;
    ::ui::Color pcol = th.dim;
    if (!episodes_done_) {
        placeholder = "Loading...";
    } else if (!episodes_ok_) {
        placeholder = series_detail_state_message(
            SeriesDetailState::SonarrUnreachable);
        pcol = th.highlight2;
    }
    const auto idxs = season_episode_indices(episodes_season_);
    const int total = static_cast<int>(idxs.size());
    if (placeholder == nullptr && total == 0) placeholder = "No episodes";
    if (placeholder != nullptr) {
        r.mb_draw_text(placeholder, static_cast<float>(body_x),
                       static_cast<float>(rows_top + 22), kRowFontPx, pcol);
        return;
    }
    // The trailing "Delete Season N…" row rides the SAME list: it is one more
    // navigable row after the last episode, so every clamp, page count and
    // range below counts with nav_total. season_delete_row_present() is false
    // on exactly the frames this function returned above, so the row can only
    // appear on a frame that draws rows.
    const int nav_total = episode_nav_count();
    const bool del_row = nav_total > total;
    // The season-paging idiom: geometry decides per_page each frame, zero is
    // legal (CRT_NATIVE's 640x480 canvas), and EVERY division is guarded.
    const int list_avail = list_bottom - kIndicatorRowH - rows_top;
    const int per_page = std::max(0, list_avail / kRowH);
    episode_per_page_ = per_page;
    episode_page_count_ =
        per_page > 0 ? std::max(1, (nav_total + per_page - 1) / per_page) : 1;
    if (episode_focus_ >= nav_total) episode_focus_ = nav_total - 1;
    if (episode_focus_ < 0) episode_focus_ = 0;
    // Page follows focus, so SELECT can only ever fire a visible row.
    episode_page_ = per_page > 0 ? episode_focus_ / per_page : 0;
    if (episode_page_ >= episode_page_count_)
        episode_page_ = episode_page_count_ - 1;
    ep_overflow = per_page > 0 && nav_total > per_page;
    const int first = episode_page_ * per_page;
    const int last = per_page > 0 ? std::min(nav_total, first + per_page) : 0;
    // The no-file rows' suffix rides on THIS season's live state.
    bool season_downloading = false;
    for (const auto& row : rows_) {
        if (row.season_number == episodes_season_) {
            season_downloading = row.state == SeasonState::Downloading;
            break;
        }
    }
    // Columns: focus marker | state glyph (▶ h:mm:ss is the widest) | text.
    const int glyph_x = body_x + 18;
    const int text_x = body_x + 118;
    const float text_w =
        static_cast<float>(screen_w - text_x - chrome::kSafeInset_px);
    int list_y = rows_top;
    for (int i = first; i < last; ++i) {
        const float row_baseline = static_cast<float>(list_y + 22);
        // The trailing delete row. Its armed fill goes down FIRST so it sits
        // behind everything; the focus marker is then the same glyph, at the
        // same body_x column, as an episode row's ring.
        if (del_row && i == total) {
            const SeasonDeleteState ds =
                season_del_inflight_  ? SeasonDeleteState::Removing
                : season_del_armed_   ? SeasonDeleteState::Armed
                                      : SeasonDeleteState::Idle;
            // The Remove button's own paint: chrome::ButtonKind::Warn is
            // th.highlight2, and the whole-series confirm keeps that same red
            // when armed — the LABEL carries the state change there, and
            // season_delete_label carries it here. Armed additionally lifts
            // the row onto th.bg_lift (the theme's one sanctioned off-bg
            // fill, the focused-row precedent from pairing_screen_renderer)
            // so the confirm reads brighter without leaving the danger color.
            // Removing dims: the press already landed, nothing to press.
            if (ds == SeasonDeleteState::Armed) {
                r.mb_fill_rect(static_cast<float>(body_x),
                               static_cast<float>(list_y),
                               static_cast<float>(screen_w - body_x -
                                                  chrome::kSafeInset_px),
                               static_cast<float>(kRowH), th.bg_lift);
            }
            if (i == episode_focus_) {
                r.mb_draw_text("\xE2\x96\xB8", static_cast<float>(body_x),
                               row_baseline, kBodyFontPx, th.accent);
            }
            // Label starts at the glyph column, not the episode-title column:
            // the row carries no state glyph, and body_x + 18 is the season
            // list's own row-label inset (render()'s "Season N").
            const std::string dlabel =
                season_delete_label(ds, episodes_season_);
            r.mb_draw_text(
                truncate_to_width(r, dlabel, kBodyFontPx,
                                  static_cast<float>(screen_w - glyph_x -
                                                     chrome::kSafeInset_px)),
                static_cast<float>(glyph_x), row_baseline, kBodyFontPx,
                ds == SeasonDeleteState::Removing ? th.dim : th.highlight2);
            list_y += kRowH;
            continue;
        }
        const auto& ep =
            episodes_[static_cast<size_t>(idxs[static_cast<size_t>(i)])];
        if (i == episode_focus_) {
            r.mb_draw_text("\xE2\x96\xB8", static_cast<float>(body_x),
                           row_baseline, kBodyFontPx, th.accent);
        }
        // Glyph: ✓ watched, "▶ <hms>" resumable, · unwatched-with-file,
        // nothing for a fileless row (the dim text IS its state).
        const auto it = episode_watch_.find(
            WatchKey{ep.season_number, ep.episode_number});
        const bool watched =
            it != episode_watch_.end() &&
            (it->second.watched ||
             is_watched_position(it->second.position_s, it->second.duration_s));
        const bool resumable =
            it != episode_watch_.end() && !watched &&
            is_resumable_position(it->second.position_s, it->second.duration_s);
        if (ep.has_file) {
            if (watched) {
                r.mb_draw_text("\xE2\x9C\x93", static_cast<float>(glyph_x),
                               row_baseline, kBodyFontPx, th.highlight1);
            } else if (resumable) {
                r.mb_draw_text(
                    "\xE2\x96\xB6 " + format_position_hms(it->second.position_s),
                    static_cast<float>(glyph_x), row_baseline, kBodyFontPx,
                    th.accent);
            } else {
                r.mb_draw_text("\xC2\xB7", static_cast<float>(glyph_x),
                               row_baseline, kBodyFontPx, th.dim);
            }
        }
        // "E<n> · <title> · <runtime>m" — runtime falls back to the series'
        // per-episode figure and is omitted when genuinely unknown.
        std::string text = "E" + std::to_string(ep.episode_number) +
                           " \xC2\xB7 " + ep.title;
        const int rt = ep.runtime_minutes > 0
            ? ep.runtime_minutes
            : (series_.has_value() ? series_->runtime_minutes : 0);
        if (rt > 0) text += " \xC2\xB7 " + std::to_string(rt) + "m";
        if (!ep.has_file && season_downloading) text += " \xC2\xB7 downloading";
        r.mb_draw_text(truncate_to_width(r, text, kBodyFontPx, text_w),
                       static_cast<float>(text_x), row_baseline, kBodyFontPx,
                       ep.has_file ? th.fg : th.dim);
        list_y += kRowH;
    }
    if (ep_overflow) {
        // Paging counts the delete row (nav_total above); the INDICATOR does
        // not — it names episodes, and the delete row is not one. Clamping
        // both ends into the episode range is what keeps a last page that
        // holds only the delete row from reading "Episodes 11-10 of 10".
        const int ind_first = std::min(first + 1, total);
        const int ind_last = std::min(last, total);
        const std::string ind =
            "Episodes " + std::to_string(ind_first) + "\xE2\x80\x93" +
            std::to_string(ind_last) + " of " + std::to_string(total) +
            " \xC2\xB7 [BTN1/BTN3]";
        r.mb_draw_text(ind, static_cast<float>(body_x),
                       static_cast<float>(list_bottom - 8), kBodyFontPx,
                       th.dim);
    }
}

void SeriesDetailScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    const auto& th = r.mb_theme();
    r.mb_fill_rect(0, 0, static_cast<float>(screen_w),
                   static_cast<float>(screen_h), th.bg);
    // Recomputed by the body path below; body-less states have one page.
    season_page_count_ = 1;
    bool overflow = false;
    bool ep_overflow = false;

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
        // list_bottom derives from where the footer hints ACTUALLY draw,
        // not from kFooterHeight_px. draw_footer_hints anchors its text
        // baseline at screen_h - kFrameInset_px - kPad3 (the wood frame's
        // inner edge — 664 at 720p), 26 px HIGHER than the notional
        // 30 px bottom band, and its icon row extends ~20 px above that
        // baseline. Budgeting against the band model put the action
        // buttons THROUGH the hint row on real hardware (caught by
        // kmsgrab, invisible to every arithmetic review that assumed the
        // band). 28 = hint-row height above baseline (~20) + an 8 px gap.
        const int list_bottom = screen_h - chrome::kFrameInset_px -
                                chrome::kPad3 - 28;
        if (region_ == DetailRegion::Episodes) {
            // The drill-down replaces the season list AND the action row in
            // the same band; BTN4 brings both back.
            render_episode_region(r, screen_w, body_x, list_top, list_bottom,
                                  ep_overflow);
        } else {
            const int list_avail =
                list_bottom - kButtonRowH - kIndicatorRowH - list_top;
            // CRT_NATIVE (640x480 logical) leaves no room below the poster —
            // list_avail goes NEGATIVE there, and a forced one-row minimum
            // drew into the reserved button band. Clamp to zero rows: the
            // totals line, action row, and footer still render; per-season
            // detail is a 720p+ affordance.
            const int per_page = std::max(0, list_avail / kRowH);
            season_per_page_ = per_page;  // handle_input's page/focus sync
            const int total_rows = static_cast<int>(rows_.size());
            // per_page can be 0 on the 640x480 canvas (the clamp above) —
            // dividing by it is UB, and the indicator would land back in the
            // poster region. Zero rows means one page, no paging affordance.
            season_page_count_ = per_page > 0
                ? std::max(1, (total_rows + per_page - 1) / per_page) : 1;
            if (season_page_ >= season_page_count_)
                season_page_ = season_page_count_ - 1;
            if (season_page_ < 0) season_page_ = 0;
            overflow = per_page > 0 && total_rows > per_page;
            const int first = season_page_ * per_page;
            const int last = std::min(total_rows, first + per_page);
            // The ring must never sit on a row this page does not show —
            // rebuild/geometry churn can strand it. Snap it into the page.
            if (season_focus_ >= 0 && per_page > 0 &&
                (season_focus_ < first || season_focus_ >= last)) {
                season_focus_ = std::min(last - 1, std::max(first,
                                                            season_focus_));
            }

            int list_y = list_top;
            for (int i = first; i < last; ++i) {
                const auto& row = rows_[static_cast<size_t>(i)];
                std::string label =
                    "Season " + std::to_string(row.season_number);
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
                // Focus marker (Task 6): the season rows joined the rotary
                // chain, so the ring needs a visible home in the list.
                if (i == season_focus_) {
                    r.mb_draw_text("\xE2\x96\xB8", static_cast<float>(body_x),
                                   static_cast<float>(list_y + 22),
                                   kBodyFontPx, th.accent);
                }
                r.mb_draw_text(label, static_cast<float>(body_x + 18),
                               static_cast<float>(list_y + 22), kRowFontPx,
                               th.fg);
                r.mb_draw_text(counts, static_cast<float>(body_x + 220),
                               static_cast<float>(list_y + 22), kBodyFontPx,
                               th.dim);
                r.mb_draw_text(state_txt, static_cast<float>(body_x + 360),
                               static_cast<float>(list_y + 22), kBodyFontPx,
                               state_col);
                list_y += kRowH;
            }
            // Paging indicator — inside the reserved band, only when it
            // earns its line. A 21-season show is the headline case for this
            // screen; without paging its later seasons were unreachable.
            if (overflow) {
                const std::string ind =
                    "Seasons " + std::to_string(first + 1) + "\xE2\x80\x93" +
                    std::to_string(last) + " of " +
                    std::to_string(total_rows) + " \xC2\xB7 [BTN1/BTN3]";
                r.mb_draw_text(ind, static_cast<float>(body_x),
                               static_cast<float>(list_bottom - kButtonRowH -
                                                  8),
                               kBodyFontPx, th.dim);
            }
            // Action row. Its height was already reserved out of the season
            // list's budget above (kButtonRowH), so it cannot overlap a row.
            if (!buttons_.empty()) {
                // SERIES-SCOPED: another series' still-finishing mutation
                // must not gray THIS page's row. SELECT stays globally gated
                // (one worker), so a press in that window is ignored briefly
                // (≤14 s worst case) — never misapplied.
                const bool busy =
                    mut_in_flight_.load() && mut_tmdb_id_ == tmdb_id_;
                int bx = body_x;
                const int brow_y = list_bottom - kButtonRowH;
                const int row_right = screen_w - chrome::kSafeInset_px;
                int drawn = 0;
                for (size_t i = 0; i < buttons_.size(); ++i) {
                    // Defensive clamp: stop before a button would cross the
                    // safe inset rather than running it under the cabinet
                    // art. The motivating canvas is 640x480 (CRT_NATIVE),
                    // where the buttons plus a long "Confirm ~N GB (est)"
                    // label are not guaranteed to fit; Task 9's acceptance
                    // eyeballs the row on the real box. Width is predicted
                    // with draw_button's own geometry (kBtnFontPx 18 +
                    // kBtnPadX 18 a side, mb_chrome.cpp) — worst case a
                    // drift there clamps one button early, which is still
                    // strictly better than drawing it where nobody can see.
                    const int bw = r.mb_text_width(buttons_[i].label,
                                                   kButtonFontPx) +
                                   2 * kButtonPadX_px;
                    if (drawn > 0 && bx + bw > row_right) break;
                    chrome::ButtonKind kind = chrome::ButtonKind::Ok;
                    if (buttons_[i].action == Action::Remove ||
                        buttons_[i].action == Action::ConfirmRemove ||
                        (buttons_[i].action == Action::WholeSeries &&
                         whole_armed_)) {
                        kind = chrome::ButtonKind::Warn;
                    } else if (buttons_[i].action == Action::WholeSeries ||
                               (season_chooser_.choosing &&
                                (buttons_[i].action == Action::AddSeason ||
                                 buttons_[i].action == Action::NextSeason))) {
                        // The open season chooser reads as "in a mode", the
                        // way the whole-series button does.
                        kind = chrome::ButtonKind::Action;
                    }
                    // While a mutation runs the row stays put with its
                    // labels unchanged and simply loses its focus ring — it
                    // reads as "busy" without destroying focus identity.
                    // SELECT is already gated on !mut_in_flight_ in
                    // handle_input. The ring also hides while it lives in
                    // the season list (season_focus_ >= 0) — one chain, one
                    // ring.
                    const auto rect = chrome::draw_button(
                        r, bx, brow_y, buttons_[i].label, kind,
                        /*focused=*/!busy && season_focus_ < 0 &&
                            static_cast<int>(i) == focus_);
                    bx = rect.x + rect.w + chrome::kPad3;
                    ++drawn;
                }
                // A focused button that was never drawn is an invisible
                // affordance: SELECT would fire something the user cannot
                // see. Clamp onto the last button that actually made it onto
                // the screen (render() already clamps season_page_ the same
                // way).
                if (focus_ >= drawn) focus_ = drawn > 0 ? drawn - 1 : 0;
            }
        }
    }

    // ALWAYS LAST on every path. The Toast is NOT drawn here: main.cpp owns
    // the single app-wide Toast::render in the correct projection.
    const bool in_episodes = region_ == DetailRegion::Episodes;
    // The trailing delete row does not play anything — read the same
    // "Select" the action row uses while the ring sits on it. Evaluated
    // AFTER render_episode_region so it sees this frame's clamped focus.
    const bool on_delete_row = in_episodes && season_delete_focused();
    const bool nothing_focusable = buttons_.empty() && rows_.empty();
    chrome::draw_footer_hints(r, screen_w, screen_h, {
        {chrome::HintIcon::Btn1Yellow,
         in_episodes
             ? (ep_overflow ? "Episodes \xE2\x86\x90" : "\xE2\x80\x94")
             : (overflow ? "Seasons \xE2\x86\x90" : "\xE2\x80\x94")},
        {chrome::HintIcon::Btn2Red, "Exit"},
        {chrome::HintIcon::Btn3Green,
         in_episodes
             ? (ep_overflow ? "Episodes \xE2\x86\x92" : "\xE2\x80\x94")
             : (overflow ? "Seasons \xE2\x86\x92" : "\xE2\x80\x94")},
        {chrome::HintIcon::Btn4Black, in_episodes ? "Seasons" : "Back"},
        {chrome::HintIcon::RotaryNav,
         in_episodes ? "Choose"
                     : (nothing_focusable ? "\xE2\x80\x94" : "Choose")},
        {chrome::HintIcon::RotaryPress,
         in_episodes ? (on_delete_row ? "Select" : "Play")
                     : (nothing_focusable ? "\xE2\x80\x94" : "Select")},
    });
}

}  // namespace media_browser::ui
