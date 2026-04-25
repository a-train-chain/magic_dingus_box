#include "media_browser/ui/detail_screen.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
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
constexpr float kPaddingX        = 32.0f;
constexpr float kPaddingY        = 18.0f;

// Top "FEATURE PRESENTATION" header strip.
constexpr float kHeaderBaselineY = 38.0f;     // baseline of header text
constexpr float kHeaderRuleY     = 58.0f;     // Y of the 2px steel-blue rule

// Big poster — 320x480 (2:3) is roughly 2x the previous poster, large
// enough to anchor the screen the way the playlist list anchors the home
// menu without overwhelming the metadata column.
constexpr float kPosterX         = 32.0f;
constexpr float kPosterY         = 84.0f;
constexpr float kPosterW         = 320.0f;
constexpr float kPosterH         = 480.0f;
constexpr float kPosterBorderW   = 2.0f;

// Gap between poster and metadata column.
constexpr float kColumnGap       = 32.0f;

// Genre chips.
constexpr float kChipH           = 28.0f;
constexpr float kChipPadX        = 14.0f;
constexpr float kChipGap         = 10.0f;
constexpr float kChipBorderW     = 2.0f;

// Action row.
constexpr float kSectionRuleY    = 588.0f;    // 2px steel-blue divider
constexpr float kActionRowTop    = 608.0f;    // top of buttons
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
        last.pop_back();
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
            buttons_.push_back({Action::MoreInfo, "More Info"});
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
        case Action::MoreInfo:       return do_more_info();
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
    // delete_files=true: remove the imported file from /library on disk too,
    // not just the Radarr DB record. The two-stage Remove → Confirm Remove
    // flow already protects against accidental presses, and a kiosk with
    // a 29 GB USB drive can't afford orphaned files. Note: the original
    // torrent download in /downloads/complete/ and the qBit torrent record
    // are NOT cleaned up by Radarr's API — that's a known gap, eventual
    // follow-up to wire qBit's torrents/delete endpoint into this flow.
    bool ok = radarr_.remove_movie(movie_->radarr_id, /*delete_files=*/true);
    if (!ok) {
        show_banner("Remove failed");
        rebuild_buttons();
        return Screen::Detail;
    }
    // After a successful remove, the library view is the natural home.
    return Screen::Library;
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

    // --- Top header bar: "FEATURE PRESENTATION" + back hint ---------
    // This mirrors render_title()/render_playlist_list()'s pattern: a Zen
    // Dots heading in steel-blue (accent2), underlined with a 2px rule.
    {
        const std::string heading = "FEATURE PRESENTATION";
        int hd_size = th.font_heading_size;
        r.mb_draw_title_text(heading, kPaddingX, kHeaderBaselineY,
                             hd_size, th.accent2, 1.0f);

        // U+25C2 BLACK LEFT-POINTING SMALL TRIANGLE, encoded as a text glyph
        // here is fine for the hint because the body font ships triangle
        // arrows. (The action-button cursor still uses a primitive — that
        // one needs guaranteed pixel-perfect rendering at small sizes.)
        const std::string back_hint = "BTN4: back  (hold for home)";
        int hint_size = th.font_small_size;
        int hw = r.mb_text_width(back_hint, hint_size);
        float hx = w - kPaddingX - static_cast<float>(hw);
        // Align the hint baseline to the heading baseline so both sit on
        // the same visual line, then nudge a couple of px lower so the
        // smaller text reads as a subtitle rather than competing.
        float hy = kHeaderBaselineY + 2.0f;
        r.mb_draw_text(back_hint, hx, hy, hint_size, th.dim, 0.9f);

        // Full-width 2px steel-blue rule beneath the header — same pattern
        // as the home menu's title underline, but stretched edge-to-edge so
        // it reads as a screen frame rather than a heading underline.
        r.mb_draw_line(kPaddingX, kHeaderRuleY,
                       w - kPaddingX, kHeaderRuleY,
                       2.0f, th.accent2, 0.95f);
    }

    // --- Big poster card (left column) -------------------------------
    {
        ::ui::Color tint = poster_tint_for_tmdb(tmdb_id_);
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

        auto lines = wrap_text(r, body, body_font_size, col_w);
        truncate_wrapped(r, lines, body_font_size, col_w, max_lines);
        if (lines.empty()) return 0;

        cursor_y += top_pad;
        if (!label.empty()) {
            int label_size = th.font_small_size;
            int label_baseline = r.mb_text_baseline(label_size);
            cursor_y += static_cast<float>(label_baseline);
            r.mb_draw_text(label, col_x, cursor_y,
                           label_size, th.accent2, 0.95f);
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
        std::string msg = "Couldn't fetch movie info from TMDB.";
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
    r.mb_draw_line(kPaddingX, kSectionRuleY,
                   w - kPaddingX, kSectionRuleY,
                   2.0f, th.accent2, 0.85f);

    // --- Action button row -------------------------------------------
    // Outlined-only buttons (no fill). Focused button: thicker gold outline,
    // text in accent (gold), and a blinking ◂ marker inside the right edge —
    // same blink+marker pattern as the home menu's playlist cursor.
    if (!buttons_.empty()) {
        int nb = static_cast<int>(buttons_.size());
        float row_w = static_cast<float>(nb) * kButtonW
                    + static_cast<float>(nb - 1) * kButtonGap;
        float row_x = (w - row_w) / 2.0f;
        float row_y = kActionRowTop;

        int lbl_size = th.font_medium_size;
        int lbl_baseline = r.mb_text_baseline(lbl_size);

        for (int i = 0; i < nb; ++i) {
            const auto& btn = buttons_[i];
            float bx = row_x + i * (kButtonW + kButtonGap);
            float by = row_y;
            bool focused = (i == focus_);

            // Confirm-remove uses warning color; everything else stays gold.
            ::ui::Color border_color = (btn.action == Action::ConfirmRemove)
                                           ? th.highlight2
                                           : th.accent;
            float thickness = focused ? (kButtonOutlineW + 1.0f)
                                       : kButtonOutlineW;
            r.mb_stroke_rect(bx, by, kButtonW, kButtonH,
                             thickness, border_color,
                             focused ? 1.0f : 0.75f);

            // Label centered in the FULL button width. The button is now
            // wide enough (260px) that even long labels like "Confirm
            // Remove" leave clear space on the right for the blinking ◂
            // cursor sitting in the reserved marker zone — the cursor
            // never visually crowds the text in practice.
            int tw = r.mb_text_width(btn.label, lbl_size);
            float tx = bx + (kButtonW - static_cast<float>(tw)) / 2.0f;
            float ty = by + (kButtonH / 2.0f)
                     - static_cast<float>(lbl_size) / 2.0f
                     + static_cast<float>(lbl_baseline);
            ::ui::Color text_color = focused ? border_color : th.fg;
            r.mb_draw_text(btn.label, tx, ty, lbl_size, text_color,
                           focused ? 1.0f : 0.85f);

            // Blinking ◂ marker centered in the reserved right-edge zone.
            if (focused && blink_on) {
                float marker_size = static_cast<float>(lbl_size) * 0.45f;
                float marker_cx = bx + kButtonW - kButtonMarkerW * 0.5f;
                float marker_cy = by + kButtonH / 2.0f;
                // Triangle pointing LEFT — toward the label, matching the
                // home-menu cursor orientation.
                r.mb_fill_triangle(
                    marker_cx,                marker_cy - marker_size,
                    marker_cx,                marker_cy + marker_size,
                    marker_cx - marker_size * 1.2f, marker_cy,
                    th.accent2, 1.0f);
            }
        }
    }

    // --- Bottom hint -------------------------------------------------
    {
        const std::string hint =
            "Rotate: nav   RCLICK / BTN2: action   BTN4: back (hold for home)";
        int sz = th.font_small_size;
        int baseline = r.mb_text_baseline(sz);
        int hw = r.mb_text_width(hint, sz);
        float hx = (w - static_cast<float>(hw)) / 2.0f;
        float hy = h - 12.0f - static_cast<float>(sz)
                 + static_cast<float>(baseline);
        r.mb_draw_text(hint, hx, hy, sz, th.dim, 0.85f);
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
        float box_y = kSectionRuleY - box_h - 12.0f;
        if (box_y < kHeaderRuleY + 8.0f) box_y = kHeaderRuleY + 8.0f;
        r.mb_fill_rect(box_x, box_y, box_w, box_h, th.bg, 0.92f);
        r.mb_stroke_rect(box_x, box_y, box_w, box_h, 2.0f, th.accent, 1.0f);
        float tx = box_x + pad;
        float ty = box_y + (box_h / 2.0f) - static_cast<float>(sz) / 2.0f
                 + static_cast<float>(baseline);
        r.mb_draw_text(banner_, tx, ty, sz, th.fg, 1.0f);
    }
}

}  // namespace media_browser::ui
