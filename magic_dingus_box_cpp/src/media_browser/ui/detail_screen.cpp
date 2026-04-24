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

// New rich-detail layout (target 1280x720). Pixel constants — all positions
// are absolute and computed from screen_w/screen_h at render time so the
// design degrades gracefully at non-720p resolutions.
constexpr float kBackdropH      = 280.0f;  // 16:9 banner at the top.
constexpr float kPaddingX       = 32.0f;   // Outer horizontal margin.
constexpr float kPaddingY       = 18.0f;   // Outer vertical margin.

constexpr float kPosterW        = 180.0f;  // 2:3 poster left of metadata.
constexpr float kPosterH        = 270.0f;
constexpr float kPosterOverlap  = 60.0f;   // How much the poster sits over the backdrop.
constexpr float kColumnGap      = 24.0f;   // Between poster and metadata column.

// Genre chips.
constexpr float kChipH          = 26.0f;
constexpr float kChipPadX       = 12.0f;
constexpr float kChipGap        = 8.0f;

// Action buttons.
constexpr float kButtonW        = 220.0f;
constexpr float kButtonH        = 52.0f;
constexpr float kButtonGap      = 20.0f;
constexpr float kButtonOutlineW = 3.0f;

// Overview wrap settings.
constexpr int   kOverviewMaxLines = 4;

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

    // --- Action row geometry (computed first so other regions clear it) ---
    const float action_row_h = kButtonH + 2.0f * kPaddingY;
    const float action_row_y = h - action_row_h;
    // Bottom-of-screen hint sits below the action row.
    const float hint_h = static_cast<float>(th.font_small_size) + 12.0f;
    const float content_bottom = action_row_y - hint_h - 8.0f;

    // --- Backdrop banner ---------------------------------------------
    {
        ::ui::Color tint = poster_tint_for_tmdb(tmdb_id_);
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
        // Aspect-fit so 16:9 backdrops fill nicely; 2:3 fallback posters
        // letterbox with the tint color rather than warping.
        r.mb_draw_poster_fit(backdrop_url,
                             0.0f, 0.0f, w, kBackdropH,
                             tint, 0.85f);
        // Approximate bottom fade for legibility — two stacked bands.
        r.mb_fill_rect(0.0f, kBackdropH * 0.55f, w, kBackdropH * 0.45f,
                       th.bg, 0.55f);
        r.mb_fill_rect(0.0f, kBackdropH - 1.0f, w, 1.0f, th.dim, 0.8f);

        // Top-right back-hint inside the backdrop area.
        const std::string back_hint = "BTN4: back (hold: home)";
        int hint_size = th.font_small_size;
        int hint_baseline = r.mb_text_baseline(hint_size);
        int hw = r.mb_text_width(back_hint, hint_size);
        float hx = w - kPaddingX - static_cast<float>(hw);
        float hy = kPaddingY + static_cast<float>(hint_baseline);
        r.mb_draw_text(back_hint, hx, hy, hint_size, th.fg, 0.85f);
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

    // --- Poster (left column, overlapping the backdrop bottom edge) --
    const float poster_x = kPaddingX;
    const float poster_y = kBackdropH - kPosterOverlap;
    {
        ::ui::Color tint = poster_tint_for_tmdb(tmdb_id_);
        r.mb_draw_poster_fit(poster_url,
                             poster_x, poster_y,
                             kPosterW, kPosterH,
                             tint, 1.0f);
        // Subtle outline so the poster reads as a card against the dark bg.
        r.mb_stroke_rect(poster_x, poster_y, kPosterW, kPosterH,
                         1.0f, th.dim, 0.7f);
    }

    // --- Right column: title / meta / chips / tagline ----------------
    const float col_x = poster_x + kPosterW + kColumnGap;
    const float col_w = w - col_x - kPaddingX;
    float cursor_y = kBackdropH - kPosterOverlap;  // Aligns top with poster.

    // Title (truncated to column width).
    {
        int title_size = th.font_title_size;
        int title_baseline = r.mb_text_baseline(title_size);
        std::string title_drawn = truncate_to_width(r, title, title_size, col_w);
        cursor_y += static_cast<float>(title_baseline);
        r.mb_draw_text(title_drawn, col_x, cursor_y,
                       title_size, th.accent, 1.0f);
        cursor_y += static_cast<float>(title_size) * 0.55f;  // baseline → next-line gap
    }

    // Meta line: "1999 · 2h 16m · en"  (left)  /  "★ 8.2/10" (right)
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
            meta_os << language;
        }
        std::string meta = meta_os.str();

        cursor_y += static_cast<float>(meta_baseline);
        if (!meta.empty()) {
            r.mb_draw_text(meta, col_x, cursor_y,
                           meta_size, th.fg, 0.92f);
        }

        // Rating + vote count, right-aligned within the column.
        std::string rating_str = format_rating(rating);
        if (!rating_str.empty()) {
            // U+2605 BLACK STAR (UTF-8 0xE2 0x98 0x85).
            std::string rating_text = std::string("\xE2\x98\x85 ") + rating_str + "/10";
            int rw = r.mb_text_width(rating_text, meta_size);
            float rx = col_x + col_w - static_cast<float>(rw);
            r.mb_draw_text(rating_text, rx, cursor_y,
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
        cursor_y += static_cast<float>(meta_size) * 0.4f;
        // Reserve a slot for the votes line below meta (only if drawn).
        if (vote_count > 0 && !rating_str.empty()) {
            cursor_y += static_cast<float>(th.font_small_size) + 4.0f;
        }
    }

    // Genre chips row.
    if (!genres.empty()) {
        int chip_size = th.font_small_size;
        int chip_baseline = r.mb_text_baseline(chip_size);
        cursor_y += 8.0f;
        float chip_y = cursor_y;
        float chip_x = col_x;
        for (const auto& g : genres) {
            int tw = r.mb_text_width(g, chip_size);
            float chip_w = static_cast<float>(tw) + 2.0f * kChipPadX;
            // Wrap to next row if we'd overflow the column.
            if (chip_x + chip_w > col_x + col_w) {
                chip_x = col_x;
                chip_y += kChipH + kChipGap;
            }
            r.mb_fill_rect(chip_x, chip_y, chip_w, kChipH,
                           th.action, 0.55f);
            r.mb_stroke_rect(chip_x, chip_y, chip_w, kChipH,
                             1.0f, th.dim, 0.6f);
            float tx = chip_x + kChipPadX;
            float ty = chip_y + (kChipH - static_cast<float>(chip_size)) / 2.0f
                     + static_cast<float>(chip_baseline);
            r.mb_draw_text(g, tx, ty, chip_size, th.fg, 0.95f);
            chip_x += chip_w + kChipGap;
        }
        cursor_y = chip_y + kChipH;
    }

    // Tagline (italic-feeling: smaller + dim).
    if (!tagline.empty()) {
        int tg_size = th.font_medium_size;
        int tg_baseline = r.mb_text_baseline(tg_size);
        cursor_y += 10.0f + static_cast<float>(tg_baseline);
        std::string tagline_q = std::string("\xE2\x80\x9C") + tagline + "\xE2\x80\x9D";  // smart quotes
        std::string drawn = truncate_to_width(r, tagline_q, tg_size, col_w);
        r.mb_draw_text(drawn, col_x, cursor_y, tg_size, th.dim, 0.95f);
        cursor_y += static_cast<float>(tg_size) * 0.4f;
    }

    // --- Below the poster row: overview, cast, director ---------------
    // The "below" boundary is the lower edge of the poster.
    const float poster_bottom = poster_y + kPosterH;
    float info_y = poster_bottom + 18.0f;

    // Overview block (full content width, capped at kOverviewMaxLines).
    if (mode_ != Mode::Error && !overview.empty()) {
        int ov_size = th.font_medium_size;
        int ov_baseline = r.mb_text_baseline(ov_size);
        float ov_max_w = w - 2.0f * kPaddingX;
        auto lines = wrap_text(r, overview, ov_size, ov_max_w);
        truncate_wrapped(r, lines, ov_size, ov_max_w, kOverviewMaxLines);

        float line_h = static_cast<float>(ov_size) * 1.35f;
        info_y += static_cast<float>(ov_baseline);
        for (size_t i = 0; i < lines.size(); ++i) {
            r.mb_draw_text(lines[i],
                           kPaddingX,
                           info_y + static_cast<float>(i) * line_h,
                           ov_size, th.fg, 0.95f);
        }
        info_y += line_h * static_cast<float>(lines.size()) - static_cast<float>(ov_baseline);
        info_y += 12.0f;
    } else if (mode_ != Mode::Error) {
        int sz = th.font_medium_size;
        int sz_baseline = r.mb_text_baseline(sz);
        info_y += static_cast<float>(sz_baseline);
        r.mb_draw_text("No synopsis available.", kPaddingX, info_y,
                       sz, th.dim, 0.7f);
        info_y += static_cast<float>(sz) * 0.4f + 12.0f;
    }

    // Error message (replaces overview if TMDB fetch failed).
    if (mode_ == Mode::Error) {
        int sz = th.font_large_size;
        int sz_baseline = r.mb_text_baseline(sz);
        std::string msg = "Couldn't fetch movie info from TMDB. Check network?";
        info_y += static_cast<float>(sz_baseline) + 8.0f;
        r.mb_draw_text(msg, kPaddingX, info_y,
                       sz, th.highlight2, 0.95f);
        info_y += static_cast<float>(sz) * 1.5f;
    }

    // Cast line.
    if (!cast_top.empty() && info_y < content_bottom) {
        int sz = th.font_medium_size;
        int sz_baseline = r.mb_text_baseline(sz);
        std::string cast_line = "Cast: " + join_with_bullet(cast_top);
        std::string drawn = truncate_to_width(r, cast_line, sz, w - 2.0f * kPaddingX);
        info_y += static_cast<float>(sz_baseline);
        r.mb_draw_text(drawn, kPaddingX, info_y, sz, th.dim, 0.95f);
        info_y += static_cast<float>(sz) * 1.0f + 6.0f;
    }

    // Director line.
    if (!directors.empty() && info_y < content_bottom) {
        int sz = th.font_medium_size;
        int sz_baseline = r.mb_text_baseline(sz);
        std::string label = directors.size() == 1 ? "Directed by: " : "Directors: ";
        std::string director_line = label + join_with_bullet(directors);
        std::string drawn = truncate_to_width(r, director_line, sz, w - 2.0f * kPaddingX);
        info_y += static_cast<float>(sz_baseline);
        r.mb_draw_text(drawn, kPaddingX, info_y, sz, th.dim, 0.95f);
    }

    // --- Action button row -------------------------------------------
    if (!buttons_.empty()) {
        int nb = static_cast<int>(buttons_.size());
        float row_w = static_cast<float>(nb) * kButtonW
                    + static_cast<float>(nb - 1) * kButtonGap;
        float row_x = (w - row_w) / 2.0f;
        float row_y = action_row_y + (action_row_h - kButtonH) / 2.0f;

        int lbl_size = th.font_medium_size;
        int lbl_baseline = r.mb_text_baseline(lbl_size);

        for (int i = 0; i < nb; ++i) {
            const auto& btn = buttons_[i];
            float bx = row_x + i * (kButtonW + kButtonGap);
            float by = row_y;
            bool focused = (i == focus_);

            ::ui::Color bg_color = (btn.action == Action::ConfirmRemove)
                                       ? th.highlight2
                                       : th.action;
            float fill_alpha = focused ? 0.95f : 0.55f;
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
            r.mb_draw_text(btn.label, tx, ty, lbl_size, th.fg,
                           focused ? 1.0f : 0.9f);
        }
    }

    // --- Bottom hint -------------------------------------------------
    {
        const std::string hint =
            "Rotate: nav   RCLICK/BTN2: action   BTN4: back (hold for home)";
        int sz = th.font_small_size;
        int baseline = r.mb_text_baseline(sz);
        int hw = r.mb_text_width(hint, sz);
        float hx = (w - static_cast<float>(hw)) / 2.0f;
        float hy = h - 10.0f - static_cast<float>(sz) + static_cast<float>(baseline);
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
        float box_y = action_row_y - box_h - 12.0f;
        if (box_y < kBackdropH + 8.0f) box_y = kBackdropH + 8.0f;
        r.mb_fill_rect(box_x, box_y, box_w, box_h, th.bg, 0.92f);
        r.mb_stroke_rect(box_x, box_y, box_w, box_h, 2.0f, th.accent, 1.0f);
        float tx = box_x + pad;
        float ty = box_y + (box_h / 2.0f) - static_cast<float>(sz) / 2.0f
                 + static_cast<float>(baseline);
        r.mb_draw_text(banner_, tx, ty, sz, th.fg, 1.0f);
    }
}

}  // namespace media_browser::ui
