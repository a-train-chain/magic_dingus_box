#include "media_browser/ui/series_detail_screen.h"

#include <algorithm>

#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/sonarr/sonarr_client.h"
#include "media_browser/ui/mb_chrome.h"
#include "media_browser/ui/mb_ui_utils.h"
#include "platform/input_manager.h"
#include "spdlog/spdlog.h"
#include "ui/renderer.h"
#include "ui/theme.h"

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
    if (gen != fetch_gen_.load()) return;  // preempted — discard
    std::lock_guard<std::mutex> lk(pending_mtx_);
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
    if (gen != fetch_gen_.load()) return;  // preempted — discard
    std::lock_guard<std::mutex> lk(pending_mtx_);
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
        p = pending_;
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

Screen SeriesDetailScreen::handle_input(
        const std::vector<platform::InputEvent>& events) {
    for (const auto& e : events) {
        // BTN4 (SETTINGS_MENU, black) — back to whoever opened us. Gated on
        // e.pressed, exactly like DetailScreen's.
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            return origin_;
        }
        // BTN1 / BTN3 page the season list. Task 5 replaces this whole
        // function with its final form (rotary + SELECT + the back relay).
        if (e.action == platform::InputAction::PREV && e.pressed) {
            if (season_page_ > 0) --season_page_;
            continue;
        }
        if (e.action == platform::InputAction::NEXT && e.pressed) {
            if (season_page_ + 1 < season_page_count_) ++season_page_;
            continue;
        }
    }
    return Screen::SeriesDetail;
}

void SeriesDetailScreen::update() { apply_pending(); }

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
        // Task 5 draws the action row here, inside this scope (body_x and
        // list_bottom are in scope only here).
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
        {chrome::HintIcon::RotaryNav, "\xE2\x80\x94"},
        {chrome::HintIcon::RotaryPress, "\xE2\x80\x94"},
    });
}

}  // namespace media_browser::ui
