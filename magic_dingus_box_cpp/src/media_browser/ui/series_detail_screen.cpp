#include "media_browser/ui/series_detail_screen.h"

#include <algorithm>
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
}  // namespace

SeriesDetailScreen::SeriesDetailScreen(SonarrClient& sonarr, TmdbClient& tmdb,
                                       QbittorrentClient* qbit,
                                       bool sonarr_configured)
    : sonarr_(sonarr), tmdb_(tmdb), qbit_(qbit),
      sonarr_configured_(sonarr_configured) {}

SeriesDetailScreen::~SeriesDetailScreen() {
    // Invalidate every in-flight publish BEFORE joining. Task 8 states this
    // destructor's final form; until then there is no poll worker to join.
    fetch_gen_.fetch_add(1);
    poll_gen_.fetch_add(1);
    if (mut_worker_.joinable()) mut_worker_.join();
    for (auto& w : workers_) {
        if (w.thread.joinable()) w.thread.join();
    }
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
    // Focus is preserved by ACTION IDENTITY, never by index. A rebuild can
    // insert, drop or relabel rows (an add turns "Add Season 1" into
    // "Download Season 2" + "Remove"), and forcing focus_ = 0 meant the
    // whole-series confirm was never the focused button — pressing SELECT
    // inside the 4 s window fired "Add Season 1" instead. Deterministically.
    std::optional<Action> keep;
    if (focus_ >= 0 && focus_ < static_cast<int>(buttons_.size()))
        keep = buttons_[static_cast<size_t>(focus_)].action;
    // Remove and ConfirmRemove are ONE button in two states: arming the
    // confirm must not move focus off it.
    const auto canonical = [](Action a) {
        return a == Action::ConfirmRemove ? Action::Remove : a;
    };

    buttons_.clear();
    const SeriesDetailInputs in{tmdb_done_, tmdb_ok_, sonarr_configured_,
                                sonarr_done_, sonarr_ok_, in_library_};
    const SeriesDetailState st = decide_series_detail_state(in);
    if (st == SeriesDetailState::NotInLibrary) {
        buttons_.push_back({Action::AddSeason1, "Add Season 1"});
        buttons_.push_back({Action::WholeSeries, whole_series_label()});
    } else if (st == SeriesDetailState::InLibrary) {
        // While the record is unsettled EVERY season reads unmonitored, so
        // next_unmonitored_season would answer "1" one second after we added
        // season 1 and the primary button would read "Download Season 1".
        // Offer Remove only until the poll settles it; the meta line says
        // "syncing…" so the missing controls read as pending, not broken.
        if (series_settled_) {
            const auto next = next_unmonitored_season(rows_);
            if (next.has_value()) {
                buttons_.push_back({Action::NextSeason,
                                    "Download Season " + std::to_string(*next)});
                buttons_.push_back({Action::WholeSeries, whole_series_label()});
            }
        }
        buttons_.push_back(remove_pending_
                               ? ActionButton{Action::ConfirmRemove, "Confirm Remove"}
                               : ActionButton{Action::Remove, "Remove"});
    }
    // NOTE: there is deliberately NO "Working…" swap while a mutation is in
    // flight. Replacing the row wholesale destroyed focus identity and was
    // the root of the wrong-mutation bug; render() dims the real row instead,
    // and SELECT is already gated on !mut_in_flight_.

    // No-match fallback biases AWAY from destructive actions: after an
    // unsettled add the row is [Remove] alone for ~9 s, and a satisfied
    // user tapping again should not find the delete button pre-focused
    // unless it is genuinely the only thing on offer.
    focus_ = 0;
    for (size_t i = 0; i < buttons_.size(); ++i) {
        if (canonical(buttons_[i].action) != Action::Remove) {
            focus_ = static_cast<int>(i);
            break;
        }
    }
    if (keep.has_value()) {
        for (size_t i = 0; i < buttons_.size(); ++i) {
            if (canonical(buttons_[i].action) == canonical(*keep)) {
                focus_ = static_cast<int>(i);
                break;
            }
        }
    }
    if (focus_ >= static_cast<int>(buttons_.size())) focus_ = 0;
}

std::string SeriesDetailScreen::whole_series_label() const {
    if (!whole_armed_) return "Whole series\xE2\x80\xA6";
    // Binary GB (GiB) on purpose — it is the same unit the free-space
    // readings arrive in, so the two numbers in the Block toast compare
    // like with like. "(est)" is the honesty: pre-add the per-episode
    // runtime is estimate_remaining_bytes' 45-minute ASSUMPTION, because
    // Sonarr has no record to read runtime_minutes from yet.
    return "Confirm ~" +
           std::to_string(whole_estimate_bytes_ / (1024LL * 1024 * 1024)) +
           " GB (est)";
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

    // Everything that MUTATES THIS PAGE is gated on identity.
    if (mut_tmdb_id_ != tmdb_id_) {
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
                const std::string err = res.ok ? std::string() : sonarr_.last_error();
                std::lock_guard<std::mutex> lk(mut_mtx_);
                if (!res.ok) {
                    mut_toast_ = title + ": add failed \xE2\x80\x94 " + err;
                    return;
                }
                mut_series_ = res.series;
                mut_settled_ = res.settled;
                // settled==false: seasons[] is EMPTY by contract. The page
                // keeps rendering TMDB rows and hides the add controls until
                // the Task-8 poll settles the record.
                mut_toast_ = res.settled
                    ? title + ": Season 1 search started"
                    : title + ": added \xE2\x80\x94 syncing seasons\xE2\x80\xA6";
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
        case Action::WholeSeries:
        case Action::Remove:
        case Action::ConfirmRemove:
            break;  // Tasks 6 and 7
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
            if (buttons_.empty()) continue;
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
            if (!buttons_.empty() && !mut_in_flight_.load() &&
                focus_ >= 0 && focus_ < static_cast<int>(buttons_.size())) {
                dispatch_action(buttons_[static_cast<size_t>(focus_)].action);
            }
            continue;
        }
        // BTN2 (PLAY_PAUSE, red) — intercepted globally by the exit modal in
        // main.cpp. It never reaches here; no per-screen handler needed.
    }
    return Screen::SeriesDetail;
}

void SeriesDetailScreen::update() {
    drain_mutation();
    expire_confirms();
    apply_pending();
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
            for (size_t i = 0; i < buttons_.size(); ++i) {
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
            }
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
