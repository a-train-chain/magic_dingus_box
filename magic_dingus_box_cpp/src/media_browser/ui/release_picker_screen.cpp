#include "media_browser/ui/release_picker_screen.h"
#include "media_browser/qbittorrent/download_watchdog.h"
#include "media_browser/radarr/radarr_client.h"
#include "media_browser/ui/mb_chrome.h"
#include "ui/renderer.h"
#include "ui/theme.h"
#include "ui/toast.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <json/json.h>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace media_browser::ui {

namespace {

// Body inset matches QueueScreen's row-padding so the picker visually
// reads as a peer of the Queue list — same left/right gutter, same
// breathing room inside the wood-frame bezel.
constexpr float kPaddingX        = 60.0f;

// Row geometry. ~80px is tall enough for a wrapped title + a sub-line
// of metadata + the indexer chip on the right edge, while still
// leaving room for 6 visible rows on a 720p display below the header
// + count sub-line.
constexpr float kRowHeight       = 76.0f;
constexpr float kRowGap          = 8.0f;
constexpr float kRowInnerPadX    = 16.0f;
constexpr float kRowInnerPadY    = 10.0f;
constexpr float kRowOutlineW     = 3.0f;   // focused / auto-pick stroke
constexpr float kRowDimOutlineW  = 1.0f;   // unfocused stroke

// Truncate `text` with a trailing ellipsis if it exceeds max_w at
// font_size. Same helper Detail / Queue use.
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

// Human-readable byte count — same format_bytes idiom QueueScreen uses
// so the size column reads consistently across the two screens.
std::string format_bytes(int64_t b) {
    if (b <= 0) return "?";
    double v = static_cast<double>(b);
    const char* unit = "B";
    if (v >= 1024.0 * 1024.0 * 1024.0) {
        v /= (1024.0 * 1024.0 * 1024.0); unit = "GB";
    } else if (v >= 1024.0 * 1024.0) {
        v /= (1024.0 * 1024.0); unit = "MB";
    } else if (v >= 1024.0) {
        v /= 1024.0; unit = "KB";
    }
    char buf[32];
    if (v >= 100.0) snprintf(buf, sizeof(buf), "%.0f %s", v, unit);
    else            snprintf(buf, sizeof(buf), "%.1f %s", v, unit);
    return buf;
}

// Build the JSON release payload Radarr's POST /api/v3/release expects.
// At minimum Radarr needs guid + indexerId — without indexerId the grab
// endpoint returns 404. Candidates sourced from Radarr's own
// /api/v3/release?movieId=X (the path Detail uses, see Task 13) carry
// indexerId natively. Candidates that omit it (e.g. constructed from
// Prowlarr-only data) fall back to guid + downloadUrl, which Radarr
// can resolve via its release cache when the entry is still warm.
Json::Value to_release_json(const ReleasePickerScreen::ReleaseCandidate& c) {
    Json::Value out(Json::objectValue);
    out["guid"]        = c.guid;
    out["title"]       = c.title;
    out["downloadUrl"] = c.download_url;
    if (c.indexer_id > 0) out["indexerId"] = c.indexer_id;
    if (c.size_bytes > 0) out["size"] = static_cast<Json::Int64>(c.size_bytes);
    return out;
}

}  // namespace

ReleasePickerScreen::ReleasePickerScreen(::media_browser::RadarrClient& radarr)
    : radarr_(radarr) {}

ReleasePickerScreen::~ReleasePickerScreen() {
    // Bump generation so any in-flight worker sees its result is stale
    // and bails before publishing, then join every spawned worker so a
    // worker mid-CURL doesn't outlive the screen and segfault on
    // pending_rows_ assignment. Worst-case wait is bounded by Radarr's
    // own /api/v3/release timeout (typical 14-25s, hard cap from
    // RadarrClient::Config::timeout_secs); in practice load workers
    // are joined synchronously when the user backs out of the picker
    // before results land — load_state_ is already Idle by then so
    // there's nothing to race against.
    load_generation_.fetch_add(1);
    for (auto& t : load_workers_) {
        if (t.joinable()) t.join();
    }
}

void ReleasePickerScreen::load_async(int radarr_movie_id,
                                     std::string movie_title) {
    // Bump unconditionally — any older in-flight worker compares its
    // captured gen to load_generation_ at publish time, sees they
    // differ, and silently drops its result. This decouples "start a
    // new load" from "wait for the old one to finish," so the UI
    // never blocks on a worker join.
    const int gen = load_generation_.fetch_add(1) + 1;

    {
        std::lock_guard<std::mutex> lk(load_mu_);
        loading_title_ = movie_title;
        // Clear any leftover pending_rows_ from a previous load — defensive,
        // since update() drains pending_rows_ on Ready, but a Failed/canceled
        // load could leave them in place.
        pending_rows_.clear();
    }
    loading_movie_id_   = radarr_movie_id;
    loading_started_at_ = std::chrono::steady_clock::now();
    movie_title_        = std::move(movie_title);  // for the loading-state header
    rows_.clear();
    focus_      = 0;
    scroll_top_ = 0;
    load_state_.store(LoadState::Loading);

    spdlog::info("[release_picker] load_async movie_id={} title='{}' gen={}",
                 radarr_movie_id, movie_title_, gen);

    // Opportunistically reap older workers so a spam-clicking user doesn't
    // accumulate dozens of threads — Radarr's interactive search timeout
    // is 45s, so the front workers in this vector are usually long-done
    // and join() returns immediately. Same heuristic ProwlarrClient uses.
    if (load_workers_.size() > 4) {
        if (load_workers_.front().joinable()) load_workers_.front().join();
        load_workers_.erase(load_workers_.begin());
    }

    // Tracked rather than detached: ~ReleasePickerScreen joins every
    // worker so a thread mid-CURL doesn't outlive the screen and segfault
    // on result publication. Same pattern DetailScreen's tmdb_workers_
    // uses.
    load_workers_.emplace_back(&ReleasePickerScreen::run_load, this,
                               gen, radarr_movie_id);
}

void ReleasePickerScreen::run_load(int gen, int radarr_movie_id) {
    // Helper: returns true if a newer load has been requested since we
    // started. When stale, the worker silently abandons its result —
    // load_async has already updated load_state_ for the new load and
    // any write here would clobber the new load's Loading state.
    auto stale = [this, gen]() {
        return gen != load_generation_.load();
    };

    std::vector<Json::Value> json_releases;
    try {
        json_releases = radarr_.get_releases_for_movie(radarr_movie_id);
    } catch (...) {
        // Defensive: RadarrClient's HTTP layer returns an empty vector
        // on transport failure rather than throwing, but networking
        // surprises happen. Treat any throw as Failed.
        if (!stale()) {
            load_state_.store(LoadState::Failed);
            spdlog::warn("[release_picker] load gen={} threw; marking Failed",
                         gen);
        }
        return;
    }

    if (stale()) {
        spdlog::info("[release_picker] load gen={} stale after Radarr; "
                     "discarding", gen);
        return;
    }

    // Parse JSON releases into picker rows. Same field-by-field shape
    // DetailScreen::do_pick_source used to do synchronously — moved here
    // so the parsing happens on the worker thread along with the HTTP.
    std::vector<ReleaseCandidate> candidates;
    candidates.reserve(json_releases.size());
    for (const auto& r : json_releases) {
        ReleaseCandidate c;
        c.title        = r.get("title", "").asString();
        c.indexer      = r.get("indexer", "").asString();
        c.indexer_id   = r.get("indexerId", 0).asInt();
        c.guid         = r.get("guid", "").asString();
        c.download_url = r.get("downloadUrl", "").asString();
        c.seeders      = r.get("seeders", 0).asInt();
        c.leechers     = r.get("leechers", 0).asInt();
        c.size_bytes   = r.get("size", 0).asInt64();
        c.score        = r.get("customFormatScore", 0).asInt();
        // Cheap title-based parsing for the picker's column badges. Radarr's
        // /api/v3/release response also carries a structured `quality` block
        // we could plumb through, but the picker columns only need short
        // string tags and the title regex covers ~95% of releases without
        // wiring up a new parser.
        auto match_first = [](const std::string& h,
                              std::initializer_list<const char*> ns) {
            for (auto* n : ns) {
                if (h.find(n) != std::string::npos) return std::string(n);
            }
            return std::string{};
        };
        c.codec      = match_first(c.title,
            {"x264", "x265", "h264", "h265", "AV1", "HEVC"});
        c.resolution = match_first(c.title,
            {"720p", "1080p", "2160p", "4K"});
        c.source     = match_first(c.title,
            {"BluRay", "WEB-DL", "WEBRip", "HDTV", "BDRip"});
        candidates.push_back(std::move(c));
    }

    // Final stale-check before publishing. If a newer load has fired,
    // drop our result — the newer load already set Loading and will
    // publish its own result.
    if (stale()) {
        spdlog::info("[release_picker] load gen={} stale after parse; "
                     "discarding {} rows", gen, candidates.size());
        return;
    }
    {
        std::lock_guard<std::mutex> lk(load_mu_);
        if (gen != load_generation_.load()) return;
        pending_rows_ = std::move(candidates);
    }
    load_state_.store(LoadState::Ready);
    spdlog::info("[release_picker] load gen={} ready", gen);
}

void ReleasePickerScreen::update() {
    if (load_state_.load() != LoadState::Ready) return;
    std::vector<ReleaseCandidate> candidates;
    std::string title;
    {
        std::lock_guard<std::mutex> lk(load_mu_);
        candidates = std::move(pending_rows_);
        title      = loading_title_;
    }
    set_candidates(std::move(title), std::move(candidates));
    // Mark the load done so a subsequent load_async can transition us
    // back to Loading without having to clear Ready first. Note that
    // rows_ may still be empty here — that's the legitimate "Radarr
    // returned 0 releases" case, which render() distinguishes from the
    // Loading state via load_state_ == Idle + rows_.empty().
    load_state_.store(LoadState::Idle);
}

void ReleasePickerScreen::set_candidates(std::string movie_title,
                                         std::vector<ReleaseCandidate> rows) {
    movie_title_ = std::move(movie_title);
    rows_        = std::move(rows);
    sort_candidates(rows_);
    // Radarr's minFormatScore = -200 in the kiosk's quality profile.
    // See CLAUDE.md "Quality configuration" — that's the floor below
    // which a release would be rejected by Radarr's auto-pick.
    flag_auto_pick_and_threshold(rows_, /*min_format_score=*/-200);
    focus_      = 0;
    scroll_top_ = 0;
}

Screen ReleasePickerScreen::handle_input(
    const std::vector<platform::InputEvent>& events) {
    // While loading or after a failed load, only BTN4 (back) is
    // interactive — there are no rows to navigate or grab. We also
    // bump the generation counter on back-out from Loading so an
    // in-flight worker drops its result on the floor when it finally
    // returns rather than transiently flipping us back to Ready after
    // we've left the screen.
    {
        const LoadState s = load_state_.load();
        if (s == LoadState::Loading || s == LoadState::Failed) {
            for (const auto& e : events) {
                if (e.action == platform::InputAction::SETTINGS_MENU
                    && e.pressed) {
                    if (s == LoadState::Loading) {
                        load_generation_.fetch_add(1);
                    }
                    load_state_.store(LoadState::Idle);
                    return Screen::Detail;
                }
            }
            return Screen::ReleasePicker;
        }
    }

    for (const auto& e : events) {
        // BTN4 (SETTINGS_MENU, black) — short-press is "back" on every
        // Marquee drill-down screen. Detail is the only sensible parent
        // for the picker (it's the only screen that opens the picker).
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            return Screen::Detail;
        }

        // Vertical navigation — both rotary (ROTATE) and DPad-vertical
        // (ROTATE_VERTICAL) are accepted, matching the navigation idiom
        // QueueScreen uses (rotary scrolls a vertical list).
        if ((e.action == platform::InputAction::ROTATE
             || e.action == platform::InputAction::ROTATE_VERTICAL)
            && e.delta != 0) {
            if (rows_.empty()) continue;
            int n = static_cast<int>(rows_.size());
            focus_ = std::clamp(focus_ + e.delta, 0, n - 1);
            // Keep focus in the visible window. Scroll happens here (not
            // in render) so render stays a pure function of state.
            if (focus_ < scroll_top_) scroll_top_ = focus_;
            if (focus_ >= scroll_top_ + kVisibleRows) {
                scroll_top_ = focus_ - kVisibleRows + 1;
            }
            continue;
        }

        if (e.action == platform::InputAction::SELECT && e.pressed) {
            if (rows_.empty()) continue;
            if (focus_ < 0 || focus_ >= static_cast<int>(rows_.size())) continue;
            const auto& cand = rows_[focus_];
            Json::Value payload = to_release_json(cand);
            bool ok = radarr_.grab_release(payload);
            if (ok) {
                ::ui::Toast::show("Grabbing release \xE2\x80\x94 see Queue");
                // Register a stall watch so DownloadWatchdog can later
                // surface a "Pick another" prompt if this manually-chosen
                // release also stalls. No-op if main.cpp didn't wire the
                // watchdog or if tmdb_id wasn't forwarded by the picker
                // callback (defensive — both should be set in production).
                if (watchdog_ && tmdb_id_ > 0) {
                    // loading_movie_id_ is populated by load_async() with
                    // the radarr_movie_id passed in from the picker
                    // callback (DetailScreen::do_pick_source or the
                    // stall-modal "Pick another" deep-link). Forwarding
                    // it lets the watchdog later raise a stall event
                    // that carries radarr_movie_id, which the stall
                    // modal uses to deep-link straight back into the
                    // picker (skipping the Detail intermediate).
                    watchdog_->watch(tmdb_id_, loading_movie_id_,
                                     movie_title_);
                }
                return Screen::Detail;
            } else {
                ::ui::Toast::show("Grab failed \xE2\x80\x94 see Radarr logs");
                // Stay on picker so the user can try a different row.
                continue;
            }
        }
    }
    return Screen::ReleasePicker;
}

void ReleasePickerScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    const ::ui::Theme& th = r.mb_theme();
    r.mb_fill_background();

    const float w = static_cast<float>(screen_w);
    const float h = static_cast<float>(screen_h);

    // --- Loading / Failed states ----------------------------------------
    // Branch early: when a load_async() worker is in flight (or failed),
    // we don't render the row table at all — there are no rows to show.
    // The header still draws (so the user has the same Marquee chrome)
    // plus a centered status message + the BTN4 footer hint that tells
    // them how to back out. This block matches BrowseScreen's loading
    // idiom (single centered message in the body region).
    {
        const LoadState lstate = load_state_.load();
        if (lstate == LoadState::Loading || lstate == LoadState::Failed) {
            namespace chrome = ::media_browser::ui::chrome;
            std::ostringstream sub;
            if (!movie_title_.empty()) {
                sub << movie_title_ << "  \xC2\xB7  ";
            }
            sub << (lstate == LoadState::Loading
                       ? "asking Radarr to scan indexers"
                       : "load failed")
                << "  \xC2\xB7  BTN4 back";
            int header_bottom = chrome::draw_screen_header(
                r, screen_w, "Pick a Source",
                /*tabs=*/{}, /*focused_tab=*/-1,
                sub.str());

            // Steel-blue underline matching the populated state below.
            const float rule_y = static_cast<float>(header_bottom);
            r.mb_draw_line(kPaddingX, rule_y,
                           static_cast<float>(screen_w) - kPaddingX, rule_y,
                           2.0f, th.dim, 0.95f);

            // Centered status message in the body region. For Loading
            // we cycle 0/1/2/3 dots at ~500ms so it reads as alive
            // even though the kiosk has no separate spinner widget.
            int sz = th.font_large_size;
            std::string msg;
            ::ui::Color msg_color = th.fg;
            if (lstate == LoadState::Loading) {
                auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - loading_started_at_).count();
                int dots = static_cast<int>((elapsed_ms / 500) % 4);
                std::string suffix(static_cast<size_t>(dots), '.');
                msg = "Loading releases" + suffix;
            } else {
                // Failed state — surface a hint that points the user
                // back to Detail's Search Again button, where they can
                // retry without the picker's larger interactive scan.
                msg = "Couldn't load releases \xE2\x80\x94 "
                      "Radarr unreachable";
                msg_color = th.highlight2;
            }
            int mw = r.mb_text_width(msg, sz);
            float mx = (static_cast<float>(screen_w)
                        - static_cast<float>(mw)) / 2.0f;
            const float footer_band = 60.0f;
            const float body_top    = static_cast<float>(header_bottom) + 30.0f;
            const float body_bottom = static_cast<float>(screen_h) - footer_band;
            float my = body_top + (body_bottom - body_top) / 2.0f
                     - static_cast<float>(sz) * 0.6f
                     + static_cast<float>(r.mb_text_baseline(sz));
            r.mb_draw_text(msg, mx, my, sz, msg_color, 0.95f);

            // Sub-message — explanation / hint.
            int sz2 = th.font_medium_size;
            std::string hint =
                lstate == LoadState::Loading
                    ? "Radarr scans every indexer in the pool, this can "
                      "take 15-25 seconds."
                    : "Try Search Again on the Detail screen, or check "
                      "Radarr's connection.";
            std::string drawn = truncate_to_width(r, hint, sz2,
                static_cast<float>(screen_w) - 2.0f * kPaddingX);
            int hw = r.mb_text_width(drawn, sz2);
            float hx = (static_cast<float>(screen_w)
                        - static_cast<float>(hw)) / 2.0f;
            float hy = my + static_cast<float>(sz) * 0.9f
                     + static_cast<float>(r.mb_text_baseline(sz2));
            r.mb_draw_text(drawn, hx, hy, sz2, th.dim, 0.8f);

            // Footer hints — only Back is meaningful while loading/failed.
            namespace mc = ::media_browser::ui::chrome;
            mc::draw_footer_hints(r, screen_w, screen_h, {
                {mc::HintIcon::Btn1Yellow,  "\xE2\x80\x94"},
                {mc::HintIcon::Btn2Red,     "Exit"},
                {mc::HintIcon::Btn3Green,   "\xE2\x80\x94"},
                {mc::HintIcon::Btn4Black,   "Back"},
                {mc::HintIcon::RotaryNav,   "\xE2\x80\x94"},
                {mc::HintIcon::RotaryPress, "\xE2\x80\x94"},
            });
            return;
        }
    }

    // 500ms blink for the focused-row marker. Same heartbeat as the
    // home menu cursor + Detail / Queue focus indicators so the visual
    // language stays consistent walking between screens.
    auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    const bool blink_on = (epoch_ms / 500) % 2 == 0;

    // --- Header -----------------------------------------------------------
    // No tabs — like Detail, the picker is a drill-down off Detail, not
    // a peer in the 6-tab Marquee strip. Title left, sub-info on the
    // right (movie name + back hint), with the same steel-blue rule
    // underneath that Detail uses.
    int header_bottom = 0;
    {
        namespace chrome = ::media_browser::ui::chrome;
        std::ostringstream sub;
        if (!movie_title_.empty()) sub << movie_title_ << "  \xC2\xB7  ";
        sub << rows_.size() << " release"
            << (rows_.size() == 1 ? "" : "s")
            << "  \xC2\xB7  BTN4 back";
        header_bottom = chrome::draw_screen_header(
            r, screen_w, "Pick a Source",
            /*tabs=*/{}, /*focused_tab=*/-1,
            sub.str());
        // Steel-blue underline matching Detail's idiom.
        const float rule_y = static_cast<float>(header_bottom);
        r.mb_draw_line(kPaddingX, rule_y, w - kPaddingX, rule_y,
                       2.0f, th.dim, 0.95f);
    }

    // --- Column header strip ---------------------------------------------
    // Five labels — Title takes the flexible middle, the right-hand
    // four (Size, Codec, Res, Source) sit in a fixed-width column rail
    // so the data below lines up cleanly across rows.
    const float col_size_w   = 90.0f;
    const float col_codec_w  = 70.0f;
    const float col_res_w    = 60.0f;
    const float col_source_w = 80.0f;
    const float col_seed_w   = 90.0f;
    const float col_gap      = 12.0f;
    // Right-edge anchors (rightmost column flush with row's inner padding).
    const float row_x        = kPaddingX;
    const float row_w        = w - 2.0f * kPaddingX;
    const float row_right_in = row_x + row_w - kRowInnerPadX;

    const float col_source_x = row_right_in - col_source_w;
    const float col_res_x    = col_source_x - col_gap - col_res_w;
    const float col_codec_x  = col_res_x    - col_gap - col_codec_w;
    const float col_size_x   = col_codec_x  - col_gap - col_size_w;
    const float col_seed_x   = col_size_x   - col_gap - col_seed_w;

    const float title_col_x  = row_x + kRowInnerPadX;
    const float title_col_w  = col_seed_x - col_gap - title_col_x;

    // Header label band sits between the rule and the first row.
    const int  hdr_size       = th.font_small_size;
    const int  hdr_baseline   = r.mb_text_baseline(hdr_size);
    const float header_label_y = static_cast<float>(header_bottom)
                               + 14.0f
                               + static_cast<float>(hdr_baseline);

    auto draw_col_label = [&](const char* label, float x) {
        r.mb_draw_text(label, x, header_label_y, hdr_size, th.dim, 0.85f);
    };
    draw_col_label("Title",   title_col_x);
    draw_col_label("Seeders", col_seed_x);
    draw_col_label("Size",    col_size_x);
    draw_col_label("Codec",   col_codec_x);
    draw_col_label("Res",     col_res_x);
    draw_col_label("Source",  col_source_x);

    // --- Body region ------------------------------------------------------
    const float body_top    = header_label_y + 14.0f;
    const float footer_band = 60.0f;  // reserve for chrome footer hints
    const float body_bottom = h - footer_band;
    const float body_h      = body_bottom - body_top;

    // --- Empty state ------------------------------------------------------
    if (rows_.empty()) {
        int sz = th.font_large_size;
        std::string msg = "No releases found";
        int mw = r.mb_text_width(msg, sz);
        float mx = (w - static_cast<float>(mw)) / 2.0f;
        float my = body_top + body_h / 2.0f
                 - static_cast<float>(sz) * 0.6f
                 + static_cast<float>(r.mb_text_baseline(sz));
        r.mb_draw_text(msg, mx, my, sz, th.dim, 0.9f);

        int sz2 = th.font_medium_size;
        std::string hint = "Try Search Again on the Detail screen, "
                           "or wait for Radarr's next RSS sweep.";
        std::string drawn = truncate_to_width(r, hint, sz2, w - 2.0f * kPaddingX);
        int hw = r.mb_text_width(drawn, sz2);
        float hx = (w - static_cast<float>(hw)) / 2.0f;
        float hy = my + static_cast<float>(sz) * 0.9f
                 + static_cast<float>(r.mb_text_baseline(sz2));
        r.mb_draw_text(drawn, hx, hy, sz2, th.dim, 0.8f);
    } else {
        // --- Rows ---------------------------------------------------------
        // Clamp scroll just in case set_candidates didn't (defensive).
        if (scroll_top_ < 0) scroll_top_ = 0;
        if (scroll_top_ > std::max(0,
                static_cast<int>(rows_.size()) - kVisibleRows)) {
            scroll_top_ = std::max(0,
                static_cast<int>(rows_.size()) - kVisibleRows);
        }
        int end_row = std::min(static_cast<int>(rows_.size()),
                               scroll_top_ + kVisibleRows);

        const int title_size  = th.font_medium_size;
        const int title_base  = r.mb_text_baseline(title_size);
        const int sub_size    = th.font_small_size;
        const int sub_base    = r.mb_text_baseline(sub_size);
        const int col_size    = th.font_small_size;
        const int col_base    = r.mb_text_baseline(col_size);

        for (int i = scroll_top_; i < end_row; ++i) {
            const auto& c = rows_[i];
            const bool focused = (i == focus_);
            float ry = body_top + (i - scroll_top_) * (kRowHeight + kRowGap);

            // Below-threshold rows render in red with reduced alpha so
            // they read as "Radarr would reject this" without being
            // un-selectable. Operator override is the whole point of
            // the picker — they may want to grab one anyway.
            ::ui::Color text_color = th.fg;
            float text_alpha       = focused ? 1.0f : 0.92f;
            if (c.below_threshold) {
                text_color = th.highlight2;
                text_alpha = focused ? 0.95f : 0.7f;
            }

            // Row outline — gold for "Radarr's would-be auto pick" so
            // the operator can see the system's own preference at a
            // glance, focus override (steel-blue accent2) so the
            // currently-selected row is unambiguous when both apply.
            // Below-threshold but focused rows get a red outline so the
            // visual signal matches the dim-red text inside.
            if (focused) {
                ::ui::Color outline = c.below_threshold
                                    ? th.highlight2
                                    : th.accent2;  // steel blue
                r.mb_stroke_rect(row_x, ry, row_w, kRowHeight,
                                 kRowOutlineW, outline, 1.0f);
            } else if (c.would_auto_pick) {
                r.mb_stroke_rect(row_x, ry, row_w, kRowHeight,
                                 kRowOutlineW, th.accent, 0.95f);  // gold
            } else {
                r.mb_stroke_rect(row_x, ry, row_w, kRowHeight,
                                 kRowDimOutlineW, th.dim, 0.4f);
            }

            // --- Title (top) + indexer sub-line ---------------------------
            std::string title_str =
                c.title.empty() ? std::string("Untitled") : c.title;
            title_str = truncate_to_width(r, title_str, title_size, title_col_w);
            float title_y = ry + kRowInnerPadY
                          + static_cast<float>(title_base);
            r.mb_draw_text(title_str, title_col_x, title_y,
                           title_size, text_color, text_alpha);

            // Sub-line: indexer + score + (auto-pick / below-threshold)
            // tag. Dim cream small font, same idiom as Queue's per-row
            // sub-line.
            std::ostringstream sub;
            sub << (c.indexer.empty() ? "?" : c.indexer);
            sub << "  \xC2\xB7  score " << c.score;
            if (c.would_auto_pick) sub << "  \xC2\xB7  Radarr's pick";
            if (c.below_threshold) sub << "  \xC2\xB7  below score floor";
            std::string sub_drawn = truncate_to_width(r, sub.str(),
                                                      sub_size, title_col_w);
            float sub_y = title_y
                        + static_cast<float>(title_size) * 0.4f
                        + static_cast<float>(sub_base);
            // Sub-line stays dim regardless of below-threshold (it's
            // already a metadata color, more red would be noise).
            r.mb_draw_text(sub_drawn, title_col_x, sub_y,
                           sub_size, th.dim, focused ? 0.95f : 0.85f);

            // --- Right-rail metadata columns -----------------------------
            // Vertically centered against the title baseline so the
            // numbers line up visually with the row's primary text.
            float col_y = title_y;
            auto draw_col_value = [&](const std::string& v, float x,
                                      float col_w) {
                std::string val = v.empty() ? std::string("?") : v;
                std::string drawn = truncate_to_width(r, val, col_size, col_w);
                r.mb_draw_text(drawn, x, col_y, col_size,
                               text_color, text_alpha);
            };

            std::ostringstream seed;
            seed << c.seeders << " \xE2\x86\x91 / " << c.leechers
                 << " \xE2\x86\x93";
            draw_col_value(seed.str(),         col_seed_x,   col_seed_w);
            draw_col_value(format_bytes(c.size_bytes), col_size_x, col_size_w);
            draw_col_value(c.codec,            col_codec_x,  col_codec_w);
            draw_col_value(c.resolution,       col_res_x,    col_res_w);
            draw_col_value(c.source,           col_source_x, col_source_w);

            // Blinking ◂ marker on the focused row's right edge — same
            // triangle Queue uses, in steel-blue accent2.
            if (focused && blink_on) {
                int ref = th.font_medium_size;
                float marker_size = static_cast<float>(ref) * 0.45f;
                float marker_cx = row_x + row_w - 14.0f;
                float marker_cy = ry + kRowHeight / 2.0f;
                r.mb_fill_triangle(
                    marker_cx,                       marker_cy - marker_size,
                    marker_cx,                       marker_cy + marker_size,
                    marker_cx - marker_size * 1.2f,  marker_cy,
                    th.accent2, 1.0f);
            }
            (void)col_base;
        }

        // --- Scroll affordance: "+N more" hint when not all visible -----
        int remaining = static_cast<int>(rows_.size()) - end_row;
        if (remaining > 0 || scroll_top_ > 0) {
            std::ostringstream hint;
            if (scroll_top_ > 0 && remaining > 0) {
                hint << "\xE2\x96\xB2 " << scroll_top_ << " above   "
                     << remaining << " below \xE2\x96\xBC";
            } else if (remaining > 0) {
                hint << remaining << " more below \xE2\x96\xBC";
            } else {
                hint << "\xE2\x96\xB2 " << scroll_top_ << " above";
            }
            std::string s = hint.str();
            int sw = r.mb_text_width(s, sub_size);
            float sx = (w - static_cast<float>(sw)) / 2.0f;
            float sy = body_bottom - 6.0f;
            r.mb_draw_text(s, sx, sy, sub_size, th.dim, 0.7f);
        }
    }

    // --- Footer hints (Marquee shared chrome) ----------------------------
    namespace mc = ::media_browser::ui::chrome;
    mc::draw_footer_hints(r, screen_w, screen_h, {
        {mc::HintIcon::Btn1Yellow,  "\xE2\x80\x94"},
        {mc::HintIcon::Btn2Red,     "Exit"},
        {mc::HintIcon::Btn3Green,   "\xE2\x80\x94"},
        {mc::HintIcon::Btn4Black,   "Back"},
        {mc::HintIcon::RotaryNav,   "Browse"},
        {mc::HintIcon::RotaryPress, "Grab"},
    });
}

}  // namespace media_browser::ui
