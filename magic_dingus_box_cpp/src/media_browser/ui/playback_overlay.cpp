#include "media_browser/ui/playback_overlay.h"

#include <spdlog/spdlog.h>

#include "media_browser/tmdb_client.h"
#include "media_browser/ui/mb_chrome.h"
#include "media_browser/ui/mb_ui_utils.h"
#include "ui/renderer.h"
#include "ui/text_utf8.h"
#include "ui/theme.h"

#include <algorithm>

namespace media_browser::ui {

namespace {

// Full-screen overlay. The overlay covers the entire playback area at
// ~85% alpha so the movie reads through.
//
// Layout (top→bottom):
//
//   [60px safe inset top]
//   Header: poster on the left (200×300), info column on the right
//     · "NOW PLAYING" accent label
//     · Title (ZenDots, gold)
//     · Year · runtime · genres line
//     · Synopsis (word-wrapped, truncated to fit)
//     · "Cast: ..." line
//     · "Director: ..." line
//   [divider rule]
//   "SIMILAR FILMS" label
//   Single row of up to 9 poster cards
//   [gap before footer]
//   Footer hint row
//   [40px bezel guard]

constexpr float kPanelAlpha     = 0.85f;

// Safe-area insets — match chrome::kSafeInset_px and kFrameInset_px.
constexpr int kSafeInset    = chrome::kSafeInset_px;  // 60 px
constexpr int kBezelInset   = chrome::kFrameInset_px; // 40 px

// Poster dimensions — smaller than Detail (280×420) so the similar-films
// row has enough vertical room below the info section. 200×300 keeps the
// proper 2:3 movie-poster aspect; at 720p the row + title pad still
// clear the footer hint band by ~15px (verified via the row_fits check
// in the similar-films render block).
constexpr int kPosterW      = 200;
constexpr int kPosterH      = 300;

// Info column starts after the poster + gap.
constexpr int kColumnGap    = 24;

// Header starts below bezel guard.
constexpr int kHeaderTopY   = kSafeInset;  // 60 px

// Grid layout — mirrors BrowseScreen exactly.
constexpr int kGridCols     = 9;
constexpr int kCellGap      = 8;
constexpr int kMetaFontPx   = 14;
constexpr int kMetaLineGap  = 2;
constexpr int kMetaTotalH   = kMetaFontPx + kMetaLineGap + kMetaFontPx; // 30
constexpr int kMetaGap      = 4;

constexpr int kMaxSimilar   = 18;  // 2 viewport-widths; cursor scrolls horizontally past col 8

// Simple greedy word-wrap. Returns lines, each fitting within max_w pixels.
std::vector<std::string> wrap_text_overlay(::ui::Renderer& r,
                                           const std::string& text,
                                           int font_size, float max_w) {
    std::vector<std::string> lines;
    if (text.empty()) return lines;

    std::string current;
    size_t pos = 0;
    // Tokenize by spaces.
    while (pos <= text.size()) {
        size_t next = text.find(' ', pos);
        if (next == std::string::npos) next = text.size();
        std::string word = text.substr(pos, next - pos);
        pos = next + 1;
        if (word.empty()) continue;

        std::string candidate = current.empty() ? word : current + " " + word;
        if (r.mb_text_width(candidate, font_size) <= static_cast<int>(max_w)) {
            current = candidate;
        } else {
            if (!current.empty()) lines.push_back(current);
            current = word;
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

// Cap lines at max_lines, appending "..." to the last line if truncated.
void cap_lines(::ui::Renderer& r, std::vector<std::string>& lines,
               int font_size, float max_w, int max_lines) {
    if (static_cast<int>(lines.size()) <= max_lines) return;
    lines.resize(max_lines);
    if (lines.empty()) return;
    auto& last = lines.back();
    while (!last.empty() && r.mb_text_width(last + "...", font_size) > max_w) {
        ::ui::utf8_pop_back(last);
    }
    last += "...";
}

}  // namespace

// ---------------------------------------------------------------------------

PlaybackOverlay::PlaybackOverlay() = default;

PlaybackOverlay::~PlaybackOverlay() {
    cancel_prefetch();
    if (fetch_thread_.joinable()) fetch_thread_.join();
}

void PlaybackOverlay::start_prefetch(::media_browser::TmdbClient& tmdb,
                                      const PlaybackOverlayMovieMeta& meta) {
    meta_ = meta;

    if (meta.tmdb_id == 0) {
        // No TMDB binding — mark done with empty list, no thread needed.
        {
            std::lock_guard<std::mutex> lk(similar_mu_);
            similar_.clear();
        }
        fetch_state_.store(FetchState::Loaded);
        spdlog::debug("[playback_overlay] tmdb_id=0, skipping similar-films fetch");
        return;
    }

    // Idempotent: don't re-fetch the same movie.
    auto fs = fetch_state_.load();
    if (prefetched_tmdb_id_ == meta.tmdb_id &&
        (fs == FetchState::InFlight || fs == FetchState::Loaded)) {
        spdlog::debug("[playback_overlay] already fetching/fetched tmdb_id={}", meta.tmdb_id);
        return;
    }

    // Cancel any previous in-flight fetch and wait for it to exit.
    cancel_prefetch();
    if (fetch_thread_.joinable()) fetch_thread_.join();

    prefetched_tmdb_id_ = meta.tmdb_id;
    cancel_requested_.store(false);
    fetch_state_.store(FetchState::InFlight);
    {
        std::lock_guard<std::mutex> lk(similar_mu_);
        similar_.clear();
    }

    spdlog::info("[playback_overlay] start_prefetch tmdb_id={} title='{}'",
                 meta.tmdb_id, meta.title);

    fetch_thread_ = std::thread([this, &tmdb, id = meta.tmdb_id]() {
        // Try recommendations first — better algorithmic suggestions than
        // /similar. Fall back to get_similar when recommendations is empty
        // (some films have no data on TMDB's recommendations endpoint).
        auto results = tmdb.get_recommendations(id, /*page=*/1);
        if (cancel_requested_.load()) {
            spdlog::debug("[playback_overlay] prefetch cancelled for tmdb_id={}", id);
            return;
        }
        spdlog::info("[playback_overlay] get_recommendations returned {} films for tmdb_id={}",
                     results.size(), id);
        if (results.empty()) {
            results = tmdb.get_similar(id, /*page=*/1);
            if (cancel_requested_.load()) {
                spdlog::debug("[playback_overlay] prefetch cancelled (fallback) for tmdb_id={}", id);
                return;
            }
            spdlog::info("[playback_overlay] get_similar (fallback) returned {} films for tmdb_id={}",
                         results.size(), id);
        }
        if (results.size() > static_cast<size_t>(kMaxSimilar)) {
            results.resize(static_cast<size_t>(kMaxSimilar));
        }
        auto count = results.size();
        {
            std::lock_guard<std::mutex> lk(similar_mu_);
            similar_ = std::move(results);
        }
        fetch_state_.store(FetchState::Loaded);
        spdlog::info("[playback_overlay] prefetch complete: {} films for tmdb_id={}",
                     count, id);
    });
}

void PlaybackOverlay::cancel_prefetch() {
    cancel_requested_.store(true);
}

void PlaybackOverlay::open() {
    open_.store(true);
    cursor_ = 0;
    render_log_once_ = true;  // arm one-shot render diagnostic log
}

void PlaybackOverlay::close() {
    open_.store(false);
}

bool PlaybackOverlay::on_rotate(int delta) {
    if (!open_.load() || delta == 0) return false;
    std::lock_guard<std::mutex> lk(similar_mu_);
    if (similar_.empty()) return true;  // consumed but nothing to scroll
    int n = static_cast<int>(similar_.size());
    cursor_ = std::clamp(cursor_ + (delta > 0 ? 1 : -1), 0, n - 1);
    return true;
}

std::optional<::media_browser::TmdbSearchHit> PlaybackOverlay::focused_film() const {
    std::lock_guard<std::mutex> lk(similar_mu_);
    if (similar_.empty() || cursor_ < 0 || cursor_ >= static_cast<int>(similar_.size())) {
        return std::nullopt;
    }
    return similar_[cursor_];
}

void PlaybackOverlay::show_toast(const std::string& msg) {
    toast_msg_ = msg;
    toast_started_at_ = std::chrono::steady_clock::now();
    toast_active_ = true;
}

void PlaybackOverlay::render(::ui::Renderer& r, int screen_w, int screen_h) {
    if (!open_.load()) return;

    const auto& th = r.mb_theme();

    // -----------------------------------------------------------------------
    // Full-screen translucent background — movie reads through at ~85% alpha.
    // -----------------------------------------------------------------------
    r.mb_fill_rect(0.0f, 0.0f,
                   static_cast<float>(screen_w),
                   static_cast<float>(screen_h),
                   th.bg_lift, kPanelAlpha);

    // -----------------------------------------------------------------------
    // TOP SECTION: Big poster on the left, info column on the right.
    // Mirrors DetailScreen's layout (kPosterX=60, kPosterY=144 offset
    // replaced here with kHeaderTopY=60 since we skip the chrome header).
    // -----------------------------------------------------------------------
    const int poster_x = kSafeInset;
    const int poster_y = kHeaderTopY;

    // Big poster (200×300 — smaller than Detail's 280×420 to give the
    // similar-films row room below).
    {
        const ::ui::Color tint = stable_tint_for_id(meta_.tmdb_id);
        r.mb_draw_poster_fit(meta_.poster_url,
                             static_cast<float>(poster_x),
                             static_cast<float>(poster_y),
                             static_cast<float>(kPosterW),
                             static_cast<float>(kPosterH),
                             tint, 1.0f);
        // 2px gold border.
        r.mb_stroke_rect(static_cast<float>(poster_x),
                         static_cast<float>(poster_y),
                         static_cast<float>(kPosterW),
                         static_cast<float>(kPosterH),
                         2.0f, th.accent, 0.95f);
    }

    // Info column: starts to the right of the poster.
    const int col_x = poster_x + kPosterW + kColumnGap;
    const int col_w = screen_w - col_x - kSafeInset;
    float cursor_y = static_cast<float>(poster_y);

    // "NOW PLAYING" accent label.
    {
        int sz = 16;
        cursor_y += static_cast<float>(r.mb_text_baseline(sz));
        r.mb_draw_title_text("NOW PLAYING",
                             static_cast<float>(col_x),
                             cursor_y,
                             sz, th.accent);
        cursor_y += static_cast<float>(sz) * 0.5f;
    }

    // Title — ZenDots, gold.
    if (!meta_.title.empty()) {
        int sz = th.font_title_size;
        int baseline = r.mb_title_text_baseline(sz);
        // Truncate to column width.
        std::string title_drawn = meta_.title;
        while (!title_drawn.empty() &&
               r.mb_title_text_width(title_drawn, sz) > col_w) {
            ::ui::utf8_pop_back(title_drawn);
        }
        if (title_drawn.size() < meta_.title.size() && !title_drawn.empty()) {
            // Back off 3 chars for "..." suffix.
            while (title_drawn.size() >= 3 &&
                   r.mb_title_text_width(title_drawn + "...", sz) > col_w) {
                ::ui::utf8_pop_back(title_drawn);
            }
            title_drawn += "...";
        }
        cursor_y += static_cast<float>(baseline);
        r.mb_draw_title_text(title_drawn,
                             static_cast<float>(col_x),
                             cursor_y,
                             sz, th.accent, 1.0f);
        // Gold underline matching title width.
        int title_w = r.mb_title_text_width(title_drawn, sz);
        float underline_y = cursor_y + 8.0f;
        r.mb_draw_line(static_cast<float>(col_x), underline_y,
                       static_cast<float>(col_x + title_w), underline_y,
                       2.0f, th.accent, 0.95f);
        cursor_y = underline_y + 12.0f;
    }

    // Year · runtime · genres meta line.
    {
        int sz = th.font_medium_size;
        int baseline = r.mb_text_baseline(sz);
        std::string meta_line;
        if (meta_.year > 0) {
            meta_line += std::to_string(meta_.year);
        }
        if (meta_.runtime_min > 0) {
            int h = meta_.runtime_min / 60;
            int m = meta_.runtime_min % 60;
            if (!meta_line.empty()) meta_line += "  \xE2\x80\xA2  ";
            if (h > 0) {
                meta_line += std::to_string(h) + "h";
                if (m > 0) meta_line += " " + std::to_string(m) + "m";
            } else {
                meta_line += std::to_string(m) + "m";
            }
        }
        if (!meta_.genres.empty()) {
            if (!meta_line.empty()) meta_line += "  \xE2\x80\xA2  ";
            meta_line += meta_.genres;
        }
        if (!meta_line.empty()) {
            cursor_y += static_cast<float>(baseline);
            r.mb_draw_text(meta_line,
                           static_cast<float>(col_x), cursor_y,
                           sz, th.fg, 0.92f);
            cursor_y += static_cast<float>(sz) * 0.5f;
        }
    }

    // Synopsis — word-wrapped, capped to available space before cast/director.
    // We reserve ~3 lines each for cast + director blocks below.
    if (!meta_.synopsis.empty()) {
        int sz = th.font_medium_size;
        float line_h = static_cast<float>(sz) * 1.4f;
        // Reserve space: cast label+line + director label+line + padding.
        float cast_reserve    = meta_.cast.empty()     ? 0.0f : (line_h * 2.2f + 8.0f);
        float dir_reserve     = meta_.director.empty() ? 0.0f : (line_h * 2.2f + 8.0f);
        float bottom_anchor   = static_cast<float>(poster_y + kPosterH);
        float available       = bottom_anchor - cursor_y - cast_reserve - dir_reserve - 12.0f;
        int max_lines = std::max(1, static_cast<int>(available / line_h));

        auto lines = wrap_text_overlay(r, meta_.synopsis, sz, static_cast<float>(col_w));
        cap_lines(r, lines, sz, static_cast<float>(col_w), max_lines);

        cursor_y += 10.0f;
        int baseline = r.mb_text_baseline(sz);
        cursor_y += static_cast<float>(baseline);
        for (size_t i = 0; i < lines.size(); ++i) {
            r.mb_draw_text(lines[i],
                           static_cast<float>(col_x),
                           cursor_y + static_cast<float>(i) * line_h,
                           sz, th.fg, 0.9f);
        }
        cursor_y += line_h * static_cast<float>(lines.size())
                  - static_cast<float>(baseline);
    }

    // Cast line.
    if (!meta_.cast.empty()) {
        int sz = th.font_small_size;
        int baseline = r.mb_text_baseline(sz);
        // Join cast names with " · ".
        std::string cast_str;
        for (size_t i = 0; i < meta_.cast.size(); ++i) {
            if (i > 0) cast_str += "  \xE2\x80\xA2  ";
            cast_str += meta_.cast[i];
        }
        cursor_y += 12.0f;
        cursor_y += static_cast<float>(baseline);
        // Label in dim, names in fg.
        r.mb_draw_text("CAST  ",
                       static_cast<float>(col_x), cursor_y,
                       sz, th.dim, 0.85f);
        int label_w = r.mb_text_width("CAST  ", sz);
        // Truncate cast string to fit remaining column width.
        float max_cast_w = static_cast<float>(col_w - label_w);
        while (!cast_str.empty() &&
               r.mb_text_width(cast_str, sz) > static_cast<int>(max_cast_w)) {
            ::ui::utf8_pop_back(cast_str);
        }
        if (!cast_str.empty() &&
            cast_str.size() < meta_.cast[0].size() + 2) {
            // Fell back too far — show truncated with ...
            while (cast_str.size() > 3 &&
                   r.mb_text_width(cast_str + "...", sz) > static_cast<int>(max_cast_w)) {
                ::ui::utf8_pop_back(cast_str);
            }
            cast_str += "...";
        }
        r.mb_draw_text(cast_str,
                       static_cast<float>(col_x + label_w), cursor_y,
                       sz, th.fg, 0.9f);
        cursor_y += static_cast<float>(sz) * 1.4f;
    }

    // Director line.
    if (!meta_.director.empty()) {
        int sz = th.font_small_size;
        int baseline = r.mb_text_baseline(sz);
        cursor_y += 8.0f;
        cursor_y += static_cast<float>(baseline);
        r.mb_draw_text("DIRECTOR  ",
                       static_cast<float>(col_x), cursor_y,
                       sz, th.dim, 0.85f);
        int label_w = r.mb_text_width("DIRECTOR  ", sz);
        // Truncate director name to fit.
        std::string dir = meta_.director;
        float max_dir_w = static_cast<float>(col_w - label_w);
        while (!dir.empty() && r.mb_text_width(dir, sz) > static_cast<int>(max_dir_w)) {
            ::ui::utf8_pop_back(dir);
        }
        r.mb_draw_text(dir,
                       static_cast<float>(col_x + label_w), cursor_y,
                       sz, th.fg, 0.9f);
        cursor_y += static_cast<float>(sz) * 1.4f;
    }

    // -----------------------------------------------------------------------
    // DIVIDER + SIMILAR FILMS SECTION
    // Positioned below the poster height (whichever is larger, poster or info col).
    // -----------------------------------------------------------------------

    // Footer hint band — we need its top to calculate spacing.
    const int footer_band_top = screen_h - kBezelInset - chrome::kFooterHeight_px - chrome::kPad2;

    // Section starts below the poster bottom, with a minimum gap.
    const int section_top = poster_y + kPosterH + 16;

    // Dim rule.
    r.mb_fill_rect(static_cast<float>(kSafeInset),
                   static_cast<float>(section_top),
                   static_cast<float>(screen_w - 2 * kSafeInset),
                   1.0f, th.dim, 0.4f);

    const int section_label_y = section_top + 14;
    r.mb_draw_title_text("SIMILAR FILMS",
                         static_cast<float>(kSafeInset),
                         static_cast<float>(section_label_y),
                         16, th.dim);

    // -----------------------------------------------------------------------
    // Single row of up to 9 poster cards.
    // -----------------------------------------------------------------------
    std::vector<::media_browser::TmdbSearchHit> snapshot;
    auto fs = fetch_state_.load();
    {
        std::lock_guard<std::mutex> lk(similar_mu_);
        snapshot = similar_;
    }
    // One-shot diagnostic: log snapshot size each time the overlay opens
    // so we can confirm whether the fetch result reached render().
    if (render_log_once_) {
        spdlog::info("[playback_overlay] render: similar snapshot size={} fetch_state={}",
                     snapshot.size(), static_cast<int>(fs));
        render_log_once_ = false;
    }

    // Grid geometry: identical to BrowseScreen's layout.
    const int content_w = screen_w - 2 * kSafeInset;
    const int cell_w    = (content_w - (kGridCols - 1) * kCellGap) / kGridCols;
    const int poster_cell_h = static_cast<int>(static_cast<float>(cell_w) * 1.5f);
    const int grid_top  = section_label_y + 18;
    const int grid_left = kSafeInset;

    if (fs == FetchState::InFlight && snapshot.empty()) {
        r.mb_draw_text("Loading related films\xE2\x80\xA6",
                       static_cast<float>(kSafeInset),
                       static_cast<float>(grid_top + poster_cell_h / 2),
                       th.font_medium_size, th.dim, 1.0f);
    } else if (snapshot.empty()) {
        r.mb_draw_text("No related films found",
                       static_cast<float>(kSafeInset),
                       static_cast<float>(grid_top + poster_cell_h / 2),
                       th.font_medium_size, th.dim, 1.0f);
    } else {
        // Single-row, page-based scroll. Each page = 1 row of kGridCols (9)
        // posters. When the cursor advances past position 8, the view snaps
        // to the next 9; going back below 0 snaps to the previous 9.
        // cursor_ is absolute; first_visible is the start of the cursor's page.
        const int n = static_cast<int>(snapshot.size());
        const bool row_fits = (grid_top + poster_cell_h + kMetaTotalH + chrome::kPad2) <= footer_band_top;

        const int first_visible = (cursor_ / kGridCols) * kGridCols;

        for (int col = 0; col < kGridCols && row_fits; ++col) {
            const int idx = first_visible + col;
            if (idx >= n) break;
            const auto& hit = snapshot[idx];
            const int x = grid_left + col * (cell_w + kCellGap);
            const int y = grid_top;

            const ::ui::Color tint = stable_tint_for_id(hit.tmdb_id);
            chrome::draw_poster_card(r, x, y, cell_w, poster_cell_h,
                                     hit.title, hit.year,
                                     tint, /*in_library=*/false,
                                     /*download_pct=*/-1,
                                     hit.poster_path);

            // Two-line title wrap below poster.
            const std::string& title = hit.title;
            const float max_w_f = static_cast<float>(cell_w);
            std::string line1, line2;
            if (r.mb_text_width(title, kMetaFontPx) <= static_cast<int>(max_w_f)) {
                line1 = title;
            } else {
                size_t split = std::string::npos;
                size_t pos   = 0;
                while (true) {
                    size_t next = title.find(' ', pos + 1);
                    if (next == std::string::npos) break;
                    if (r.mb_text_width(title.substr(0, next), kMetaFontPx) > static_cast<int>(max_w_f))
                        break;
                    split = next;
                    pos   = next;
                }
                if (split == std::string::npos) {
                    std::string candidate = title;
                    while (!candidate.empty() &&
                           r.mb_text_width(candidate + "...", kMetaFontPx) > static_cast<int>(max_w_f)) {
                        ::ui::utf8_pop_back(candidate);
                    }
                    line1 = candidate.empty() ? title : (candidate + "...");
                } else {
                    line1 = title.substr(0, split);
                    std::string rem = title.substr(split + 1);
                    if (r.mb_text_width(rem, kMetaFontPx) <= static_cast<int>(max_w_f)) {
                        line2 = rem;
                    } else {
                        std::string candidate = rem;
                        while (!candidate.empty() &&
                               r.mb_text_width(candidate + "...", kMetaFontPx) > static_cast<int>(max_w_f)) {
                            ::ui::utf8_pop_back(candidate);
                        }
                        line2 = candidate.empty() ? rem : (candidate + "...");
                    }
                }
            }
            const int meta_top = y + poster_cell_h + kMetaGap;
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

            // Gold focus ring on the cursor cell (cursor_ is absolute,
            // idx is also absolute — compare directly).
            if (idx == cursor_) {
                chrome::draw_focus_ring(r, x, y, cell_w, poster_cell_h);
            }
        }
    }

    // Toast confirmation: shown for 2 s overlaid near the top-right.
    if (toast_active_) {
        using namespace std::chrono;
        auto elapsed_ms = duration_cast<milliseconds>(
            steady_clock::now() - toast_started_at_).count();
        if (elapsed_ms > 2000) {
            toast_active_ = false;
        } else {
            const int tw = static_cast<int>(r.mb_text_width(toast_msg_, 14)) + 16;
            const int tx = screen_w - kSafeInset - tw;
            const int ty = kHeaderTopY;
            r.mb_fill_rect(static_cast<float>(tx), static_cast<float>(ty),
                           static_cast<float>(tw), 24.0f,
                           th.bg_lift, 1.0f);
            r.mb_fill_rect(static_cast<float>(tx), static_cast<float>(ty),
                           static_cast<float>(tw), 1.0f,
                           th.accent, 1.0f);
            r.mb_draw_text(toast_msg_,
                           static_cast<float>(tx + 8),
                           static_cast<float>(ty + 6),
                           14, th.accent, 1.0f);
        }
    }

    // Footer hint row (matches mb_chrome visual style, inside bezel).
    chrome::draw_footer_hints(r, screen_w, screen_h, {
        {chrome::HintIcon::RotaryNav,   "Browse Similar"},
        {chrome::HintIcon::Btn4Black,   "Close"},
        {chrome::HintIcon::RotaryPress, "Quick Add"},
    });
}

}  // namespace media_browser::ui
