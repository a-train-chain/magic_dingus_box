#include "media_browser/ui/detail_screen.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "media_browser/radarr/radarr_client.h"
#include "media_browser/tmdb_client.h"
#include "ui/renderer.h"
#include "ui/theme.h"
#include "ui/toast.h"

namespace media_browser::ui {

namespace {

// Layout fractions. The spec calls for 35 / 40 / 25. Action row sits in the
// bottom 25%. Margins below are chosen so the three regions visually
// separate without overlapping.
constexpr float kBannerFrac    = 0.35f;
constexpr float kOverviewFrac  = 0.40f;  // Unused directly, implied by kBannerFrac + kActionFrac.
constexpr float kActionFrac    = 0.25f;
constexpr float kPaddingX      = 48.0f;
constexpr float kBannerPadding = 28.0f;

// Action button metrics.
constexpr float kButtonW        = 240.0f;
constexpr float kButtonH        = 60.0f;
constexpr float kButtonGap      = 24.0f;
constexpr float kButtonOutlineW = 3.0f;

// Overview layout.
constexpr int   kOverviewMaxLines = 6;
constexpr float kOverviewMaxWFrac = 0.60f;

// Backdrop poster tint — same deterministic hash used by Browse, so the
// color theme on the detail screen matches the grid placeholder.
::ui::Color poster_tint_for_tmdb(int tmdb_id) {
    uint32_t h = static_cast<uint32_t>(tmdb_id) * 2654435761u;  // Knuth hash
    uint8_t r = 64 + static_cast<uint8_t>((h >>  0) & 0x7F);
    uint8_t g = 40 + static_cast<uint8_t>((h >>  8) & 0x5F);
    uint8_t b = 80 + static_cast<uint8_t>((h >> 16) & 0x7F);
    return {r, g, b, 255};
}

// Greedy word-wrap by pixel width. Breaks words that individually exceed
// the width at whatever partial point fits.
std::vector<std::string> wrap_text(::ui::Renderer& r, const std::string& text,
                                   int font_size, float max_w) {
    std::vector<std::string> lines;
    if (text.empty()) return lines;

    std::istringstream iss(text);
    std::string word;
    std::string current;

    auto width_of = [&](const std::string& s) {
        return static_cast<float>(r.mb_text_width(s, font_size));
    };

    while (iss >> word) {
        std::string candidate = current.empty() ? word : current + " " + word;
        if (width_of(candidate) <= max_w) {
            current = candidate;
            continue;
        }
        // Candidate is too wide. Flush current (if any) and start new line.
        if (!current.empty()) {
            lines.push_back(current);
            current.clear();
        }
        // If the word by itself fits on a line, start the line with it.
        if (width_of(word) <= max_w) {
            current = word;
            continue;
        }
        // Word is wider than max_w — hard-split by characters.
        std::string fragment;
        for (char c : word) {
            std::string next = fragment + c;
            if (width_of(next) > max_w) {
                lines.push_back(fragment);
                fragment = std::string(1, c);
            } else {
                fragment = next;
            }
        }
        current = fragment;
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

// Truncate `text` with a trailing ellipsis if it exceeds max_w at font_size.
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

// Format runtime as e.g. "1h 42m".
std::string format_runtime(int minutes) {
    if (minutes <= 0) return "";
    int h = minutes / 60;
    int m = minutes % 60;
    std::ostringstream os;
    if (h > 0) os << h << "h ";
    os << m << "m";
    return os.str();
}

// Format rating as e.g. "7.4" (one decimal). Empty string if unrated.
std::string format_rating(double rating) {
    if (rating <= 0.0) return "";
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", rating);
    return buf;
}

}  // namespace

DetailScreen::DetailScreen(RadarrClient& radarr, TmdbClient& tmdb)
    : radarr_(radarr), tmdb_(tmdb) {
    mode_ = Mode::Loading;
    rebuild_buttons();
}

void DetailScreen::enter() {
    // Clear any transient state from a previous visit.
    remove_pending_ = false;
    banner_.clear();
    focus_ = 0;
    needs_refresh_ = true;
    mode_ = Mode::Loading;
    fetch();
}

void DetailScreen::fetch() {
    needs_refresh_ = false;
    movie_.reset();
    tmdb_detail_.reset();
    banner_.clear();

    if (tmdb_id_ == 0) {
        mode_ = Mode::NoTmdb;
        rebuild_buttons();
        return;
    }

    // 1) TMDB metadata — the primary source. Always try this first. We used
    // to route through radarr_.lookup() (Radarr's SkyHook proxy to TMDB)
    // but SkyHook on api.radarr.video has flaky transient 503s, which
    // bricked the entire Detail screen even when Radarr itself was healthy.
    auto detail = tmdb_.get_movie(tmdb_id_);
    if (!detail) {
        // TMDB itself failed — rare. Show an error with Retry.
        mode_ = Mode::Error;
        rebuild_buttons();
        return;
    }
    tmdb_detail_ = *detail;

    // 2) Radarr library state — optional. If Radarr is unreachable we still
    // render the Detail screen with TMDB metadata; only the action buttons
    // become best-effort (Add will fail with a toast, etc.).
    auto library = radarr_.get_library();
    bool library_ok = library.empty() ? radarr_.last_error().empty() : true;

    const Movie* found = nullptr;
    if (library_ok) {
        for (const auto& m : library) {
            if (m.tmdb_id == tmdb_id_) { found = &m; break; }
        }
    }

    if (found) {
        movie_ = *found;
        mode_ = found->has_file ? Mode::InLibraryWithFile : Mode::InLibraryNoFile;
    } else {
        mode_ = Mode::NotInLibrary;
    }

    // Best-effort fetch of quality profiles — needed for Add. Cheap; safe
    // to call even when not strictly required.
    if (library_ok) {
        profiles_ = radarr_.get_quality_profiles();
    }

    // If Radarr was unreachable, surface a non-blocking banner so the user
    // knows mutating actions may fail.
    if (!library_ok) {
        show_banner("Radarr service offline — adding to library may fail");
    }

    rebuild_buttons();
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
            break;
        case Mode::InLibraryNoFile:
            buttons_.push_back({Action::SearchAgain, "Search Again"});
            buttons_.push_back(remove_pending_
                               ? Button{Action::ConfirmRemove, "Confirm Remove"}
                               : Button{Action::Remove, "Remove"});
            break;
        case Mode::InLibraryWithFile:
            buttons_.push_back({Action::Play, "Play"});
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
    for (const auto& e : events) {
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            // Return to the screen that opened us (Browse / Search /
            // Library / ...). main.cpp sets origin_ on every transition
            // into Detail; default is Browse.
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

        // BTN2 (PLAY_PAUSE): activate the default (leftmost / primary)
        // action for the current Mode. That's Add in NotInLibrary,
        // Search Again in InLibraryNoFile, Play in InLibraryWithFile,
        // Retry in Error. Same flow as selecting button index 0 — we
        // snap focus_ there before delegating.
        if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed) {
            if (buttons_.empty()) continue;
            focus_ = 0;
            // Cancel any pending remove confirmation to avoid a surprise
            // destructive fire from a different button.
            if (remove_pending_) {
                remove_pending_ = false;
                rebuild_buttons();
            }
            Screen next = on_activate();
            if (next != Screen::Detail) return next;
            continue;
        }
    }
    return Screen::Detail;
}

Screen DetailScreen::on_activate() {
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
    }
    return Screen::Detail;
}

int DetailScreen::pick_quality_profile_id() const {
    // Look for "HD-1080p" (Radarr's default), then "1080p" anywhere in the
    // name, else fall back to the first profile.
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
    bool ok = radarr_.add_movie(tmdb_id_, qp, /*monitor=*/true);
    if (!ok) {
        // Keep the in-screen banner and also surface a top-level toast so
        // the failure is visible outside the action button row context.
        show_banner("Add failed — see Radarr logs");
        ::ui::Toast::show("Add failed — see Radarr logs");
        return Screen::Detail;
    }
    // Success: pop a toast confirming the action, refresh the library
    // cache (so Detail shows the new record if the user comes back), and
    // jump to the Queue screen so they see the download start populating
    // in real time.
    ::ui::Toast::show("Added to library — downloading");
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
    if (!movie_.has_value()) {
        show_banner("No movie record");
        rebuild_buttons();
        return Screen::Detail;
    }
    bool ok = radarr_.remove_movie(movie_->radarr_id, /*delete_files=*/false);
    if (!ok) {
        show_banner("Remove failed");
        rebuild_buttons();
        return Screen::Detail;
    }
    // After a successful remove, the library view is the natural home.
    return Screen::Library;
}

Screen DetailScreen::do_play() {
    // Task 24 wires up real playback. For now just hand off to Library so
    // the user ends up somewhere sensible instead of staring at a dead
    // button.
    return Screen::Library;
}

Screen DetailScreen::do_retry() {
    mode_ = Mode::Loading;
    rebuild_buttons();
    fetch();
    return Screen::Detail;
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

    const float banner_h  = h * kBannerFrac;
    const float action_h  = h * kActionFrac;
    const float overview_top    = banner_h;
    const float overview_bottom = h - action_h;

    // Special states override the standard layout with a centered message.
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
    // Error state: render a centered message above the Retry button row
    // (button row layout is handled by the common code below). Only TMDB
    // failures land here — Radarr being unreachable just sets a banner.
    if (mode_ == Mode::Error) {
        int sz = th.font_large_size;
        std::string msg = "Couldn't fetch movie info from TMDB. Check network?";
        int mw = r.mb_text_width(msg, sz);
        float x = (w - static_cast<float>(mw)) / 2.0f;
        float y = (overview_bottom / 2.0f)
                + static_cast<float>(r.mb_text_baseline(sz));
        r.mb_draw_text(msg, x, y, sz, th.highlight2, 0.95f);
        // fallthrough into button row render below
    }

    // --- Top banner / backdrop ---------------------------------------
    if (mode_ != Mode::Error) {
        // Backdrop: prefer real fanart if the async cache has it;
        // else fall back to a dimmed poster-tint rectangle. Falling
        // back to the poster_url if fanart is missing keeps the banner
        // useful for records that only have one artwork type.
        ::ui::Color tint = poster_tint_for_tmdb(tmdb_id_);
        // TMDB metadata is the primary source; fall back to library record
        // (mostly relevant for movies that have local fanart not yet in
        // TMDB's payload, e.g. user-supplied artwork).
        std::string backdrop_url;
        if (tmdb_detail_.has_value()) {
            backdrop_url = tmdb_detail_->backdrop_path.empty()
                               ? tmdb_detail_->poster_path
                               : tmdb_detail_->backdrop_path;
        }
        if (backdrop_url.empty() && movie_.has_value()) {
            backdrop_url = movie_->fanart_url.empty() ? movie_->poster_url
                                                      : movie_->fanart_url;
        }
        r.mb_draw_poster_or_tint(backdrop_url,
                                 0.0f, 0.0f, w, banner_h,
                                 tint, 0.7f);
        // Subtle bottom fade — a second dark rect with gradient-ish alpha to
        // make the text legible. We don't have a real gradient primitive so
        // approximate with two stacked bands.
        r.mb_fill_rect(0.0f, banner_h * 0.55f, w, banner_h * 0.45f,
                       th.bg, 0.55f);
        r.mb_fill_rect(0.0f, banner_h - 1.0f, w, 1.0f, th.dim, 0.8f);

        // Title / year / rating (bottom-left). Runtime (bottom-right).
        // Prefer TMDB fields (always populated when mode != Error/NoTmdb).
        // Fall back to the library Movie record only if TMDB somehow has
        // an empty title (defensive — shouldn't happen in practice).
        std::string title;
        int year = 0;
        double rating = 0.0;
        int runtime = 0;
        if (tmdb_detail_.has_value()) {
            title = tmdb_detail_->title;
            year = tmdb_detail_->year;
            rating = tmdb_detail_->rating;
            runtime = tmdb_detail_->runtime_minutes;
        }
        if (title.empty() && movie_.has_value()) {
            title = movie_->title;
            year = movie_->year;
            rating = movie_->rating;
            runtime = movie_->runtime_minutes;
        }
        if (title.empty()) title = "Untitled";

        int title_size = th.font_title_size;
        // Truncate title if it would run off the screen (leave room on
        // the right for runtime).
        float title_max_w = w - 2.0f * kBannerPadding - 180.0f;
        std::string title_drawn = truncate_to_width(r, title, title_size, title_max_w);
        int title_baseline = r.mb_text_baseline(title_size);

        // Meta line (year + rating), rendered below title.
        int meta_size = th.font_medium_size;
        std::ostringstream meta_os;
        if (year > 0) meta_os << year;
        std::string rating_str = format_rating(rating);
        if (!rating_str.empty()) {
            if (year > 0) meta_os << "  \xE2\x80\xA2  ";  // U+2022 bullet
            meta_os << rating_str << " / 10";
        }
        std::string meta = meta_os.str();
        int meta_baseline = r.mb_text_baseline(meta_size);

        // Position title such that the meta line fits beneath it with
        // kBannerPadding at the bottom.
        float meta_y  = banner_h - kBannerPadding
                      - static_cast<float>(meta_size)
                      + static_cast<float>(meta_baseline);
        float title_y = meta_y - static_cast<float>(meta_size)
                      - static_cast<float>(title_size) * 0.3f;
        r.mb_draw_text(title_drawn, kBannerPadding, title_y,
                       title_size, th.accent, 1.0f);
        if (!meta.empty()) {
            r.mb_draw_text(meta, kBannerPadding, meta_y,
                           meta_size, th.fg, 0.95f);
        }

        // Runtime, right-aligned at the same y as meta.
        std::string runtime_str = format_runtime(runtime);
        if (!runtime_str.empty()) {
            int rw = r.mb_text_width(runtime_str, meta_size);
            float rx = w - kBannerPadding - static_cast<float>(rw);
            r.mb_draw_text(runtime_str, rx, meta_y,
                           meta_size, th.fg, 0.95f);
        }

        // Back-hint top-right.
        const std::string back_hint =
            "Rotate: nav   RCLICK/BTN2: action   BTN4: back (hold: exit)";
        int hint_size = th.font_small_size;
        int hint_baseline = r.mb_text_baseline(hint_size);
        int hw = r.mb_text_width(back_hint, hint_size);
        float hx = w - kBannerPadding - static_cast<float>(hw);
        float hy = kBannerPadding + static_cast<float>(hint_baseline);
        r.mb_draw_text(back_hint, hx, hy, hint_size, th.fg, 0.8f);
    }

    // --- Middle: overview text ---------------------------------------
    if (mode_ != Mode::Error) {
        std::string overview;
        if (tmdb_detail_.has_value()) overview = tmdb_detail_->overview;
        if (overview.empty() && movie_.has_value()) overview = movie_->overview;

        if (!overview.empty()) {
            int ov_size = th.font_medium_size;
            int ov_baseline = r.mb_text_baseline(ov_size);
            float ov_max_w = w * kOverviewMaxWFrac;
            auto lines = wrap_text(r, overview, ov_size, ov_max_w);
            bool truncated = (static_cast<int>(lines.size()) > kOverviewMaxLines);
            if (truncated) {
                lines.resize(kOverviewMaxLines);
                // Append ellipsis to the last line, respecting width.
                std::string& last = lines.back();
                while (!last.empty() &&
                       r.mb_text_width(last + "...", ov_size) > ov_max_w) {
                    last.pop_back();
                }
                last += "...";
            }
            // Center the block vertically in the overview region, and each
            // line horizontally by its own width (so variable-length lines
            // all read well).
            float line_h = static_cast<float>(ov_size) * 1.35f;
            float total_h = line_h * static_cast<float>(lines.size());
            float region_h = overview_bottom - overview_top;
            float start_y = overview_top + (region_h - total_h) / 2.0f
                          + static_cast<float>(ov_baseline);
            for (size_t i = 0; i < lines.size(); ++i) {
                const std::string& line = lines[i];
                int lw = r.mb_text_width(line, ov_size);
                float lx = (w - static_cast<float>(lw)) / 2.0f;
                float ly = start_y + static_cast<float>(i) * line_h;
                r.mb_draw_text(line, lx, ly, ov_size, th.fg, 0.95f);
            }
        } else {
            // No overview provided — show a dim placeholder.
            int sz = th.font_medium_size;
            std::string msg = "No synopsis available.";
            int mw = r.mb_text_width(msg, sz);
            float x = (w - static_cast<float>(mw)) / 2.0f;
            float y = (overview_top + overview_bottom) / 2.0f
                    + static_cast<float>(r.mb_text_baseline(sz));
            r.mb_draw_text(msg, x, y, sz, th.dim, 0.7f);
        }
    }

    // --- Bottom: action button row -----------------------------------
    if (!buttons_.empty()) {
        int nb = static_cast<int>(buttons_.size());
        float row_w = static_cast<float>(nb) * kButtonW
                    + static_cast<float>(nb - 1) * kButtonGap;
        float row_x = (w - row_w) / 2.0f;
        float row_y = overview_bottom + (action_h - kButtonH) / 2.0f;

        int lbl_size = th.font_medium_size;
        int lbl_baseline = r.mb_text_baseline(lbl_size);

        for (int i = 0; i < nb; ++i) {
            const auto& btn = buttons_[i];
            float bx = row_x + i * (kButtonW + kButtonGap);
            float by = row_y;
            bool focused = (i == focus_);

            // Destructive-looking fill for the Confirm Remove button so the
            // user notices what they're about to do.
            ::ui::Color bg_color = (btn.action == Action::ConfirmRemove)
                                       ? th.highlight2
                                       : th.action;
            float fill_alpha = focused ? 0.95f : 0.6f;
            r.mb_fill_rect(bx, by, kButtonW, kButtonH, bg_color, fill_alpha);

            if (focused) {
                r.mb_stroke_rect(bx - kButtonOutlineW / 2.0f,
                                 by - kButtonOutlineW / 2.0f,
                                 kButtonW + kButtonOutlineW,
                                 kButtonH + kButtonOutlineW,
                                 kButtonOutlineW,
                                 th.accent, 1.0f);
            } else {
                r.mb_stroke_rect(bx, by, kButtonW, kButtonH,
                                 1.0f, th.dim, 0.6f);
            }

            int tw = r.mb_text_width(btn.label, lbl_size);
            float tx = bx + (kButtonW - static_cast<float>(tw)) / 2.0f;
            float ty = by + (kButtonH / 2.0f)
                     - static_cast<float>(lbl_size) / 2.0f
                     + static_cast<float>(lbl_baseline);
            const ::ui::Color& fg =
                (btn.action == Action::ConfirmRemove) ? th.fg : th.fg;
            r.mb_draw_text(btn.label, tx, ty, lbl_size, fg,
                           focused ? 1.0f : 0.9f);
        }
    }

    // --- Transient banner --------------------------------------------
    if (!banner_.empty()) {
        int sz = th.font_medium_size;
        int baseline = r.mb_text_baseline(sz);
        int bw = r.mb_text_width(banner_, sz);
        float pad = 16.0f;
        float box_w = static_cast<float>(bw) + 2.0f * pad;
        float box_h = static_cast<float>(sz) + 2.0f * pad * 0.5f;
        float box_x = (w - box_w) / 2.0f;
        float box_y = overview_bottom - box_h - 12.0f;
        if (box_y < banner_h + 8.0f) box_y = banner_h + 8.0f;
        r.mb_fill_rect(box_x, box_y, box_w, box_h, th.bg, 0.9f);
        r.mb_stroke_rect(box_x, box_y, box_w, box_h, 2.0f, th.accent, 1.0f);
        float tx = box_x + pad;
        float ty = box_y + (box_h / 2.0f) - static_cast<float>(sz) / 2.0f
                 + static_cast<float>(baseline);
        r.mb_draw_text(banner_, tx, ty, sz, th.fg, 1.0f);
    }
}

}  // namespace media_browser::ui
