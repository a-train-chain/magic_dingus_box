#include "media_browser/ui/playback_overlay.h"

#include <spdlog/spdlog.h>

#include "media_browser/tmdb_client.h"
#include "media_browser/ui/mb_chrome.h"
#include "ui/renderer.h"
#include "ui/theme.h"

#include <algorithm>

namespace media_browser::ui {

namespace {

// Full-screen overlay. The overlay covers the entire playback area at
// ~85% alpha so the movie reads through. Layout (top→bottom):
//
//   [40px bezel guard]
//   Detail header band: title · year · runtime · genres + synopsis
//   "SIMILAR FILMS" section heading
//   9-column poster grid (matching BrowseScreen density)
//   Footer hint row
//   [40px bezel guard]

constexpr float kPanelAlpha     = 0.85f;

// Safe-area insets — match chrome::kSafeInset_px and kFrameInset_px.
constexpr int kSafeInset    = chrome::kSafeInset_px;  // 60 px
constexpr int kBezelInset   = chrome::kFrameInset_px; // 40 px

// Header band height (title + meta line + synopsis).
constexpr int kHeaderTopY   = 56;  // starts just below bezel guard
constexpr int kHeaderH      = 110;

// Grid layout — mirrors BrowseScreen exactly.
constexpr int kGridCols     = 9;
constexpr int kCellGap      = 8;
constexpr int kRowGap       = 22;
constexpr int kMetaFontPx   = 14;
constexpr int kMetaLineGap  = 2;
constexpr int kMetaTotalH   = kMetaFontPx + kMetaLineGap + kMetaFontPx; // 30
constexpr int kMetaGap      = 4;

constexpr int kMaxSimilar   = 18;  // up to 2 rows of 9

// Maximum characters of synopsis to render.
constexpr size_t kSynopsisMaxChars = 160;

// Deterministic tint color for a TMDB id — same Knuth-hash formula as
// browse_screen.cpp (local copy; that function is file-scoped there).
inline ::ui::Color poster_tint_for_tmdb(int tmdb_id) {
    uint32_t h = static_cast<uint32_t>(tmdb_id) * 2654435761u;
    uint8_t r = 64  + static_cast<uint8_t>((h >>  0) & 0x7F);
    uint8_t g = 40  + static_cast<uint8_t>((h >>  8) & 0x5F);
    uint8_t b = 80  + static_cast<uint8_t>((h >> 16) & 0x7F);
    return {r, g, b, 255};
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

    spdlog::info("[playback_overlay] starting similar-films prefetch for tmdb_id={}", meta.tmdb_id);

    fetch_thread_ = std::thread([this, &tmdb, id = meta.tmdb_id]() {
        auto results = tmdb.get_similar(id, /*page=*/1);
        if (cancel_requested_.load()) {
            spdlog::debug("[playback_overlay] prefetch cancelled for tmdb_id={}", id);
            return;
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
        spdlog::info("[playback_overlay] prefetch complete: {} similar films for tmdb_id={}",
                     count, id);
    });
}

void PlaybackOverlay::cancel_prefetch() {
    cancel_requested_.store(true);
}

void PlaybackOverlay::open() {
    open_.store(true);
    cursor_ = 0;
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
    // Header band: NOW PLAYING title/year/runtime/genres + synopsis
    // Anchored inside the 40 px bezel guard on all sides.
    // -----------------------------------------------------------------------
    const int hx = kSafeInset;
    const int hy = kHeaderTopY;

    // "NOW PLAYING" accent label.
    r.mb_draw_title_text("NOW PLAYING",
                         static_cast<float>(hx),
                         static_cast<float>(hy),
                         16, th.accent);

    // Title + year/runtime/genres meta line.
    std::string meta_line = meta_.title;
    if (meta_.year > 0)        meta_line += "  \xC2\xB7  " + std::to_string(meta_.year);
    if (meta_.runtime_min > 0) meta_line += "  \xC2\xB7  " + std::to_string(meta_.runtime_min) + "m";
    if (!meta_.genres.empty()) meta_line += "  \xC2\xB7  " + meta_.genres;
    r.mb_draw_text(meta_line,
                   static_cast<float>(hx),
                   static_cast<float>(hy + 26),
                   th.font_medium_size, th.fg, 1.0f);

    // Synopsis (truncated).
    std::string syn = meta_.synopsis;
    if (syn.size() > kSynopsisMaxChars) syn = syn.substr(0, kSynopsisMaxChars) + "\xE2\x80\xA6";
    r.mb_draw_text(syn,
                   static_cast<float>(hx),
                   static_cast<float>(hy + 54),
                   th.font_small_size, th.dim, 1.0f);

    // 1 px dim rule between header and grid.
    const int rule_y = hy + kHeaderH;
    r.mb_fill_rect(static_cast<float>(hx),
                   static_cast<float>(rule_y),
                   static_cast<float>(screen_w - 2 * kSafeInset),
                   1.0f, th.dim, 0.4f);

    // "SIMILAR FILMS" section label.
    const int section_label_y = rule_y + 16;
    r.mb_draw_title_text("SIMILAR FILMS",
                         static_cast<float>(hx),
                         static_cast<float>(section_label_y),
                         16, th.dim);

    // -----------------------------------------------------------------------
    // 9-column poster grid — snapshot under lock to avoid holding it during render
    // -----------------------------------------------------------------------
    std::vector<::media_browser::TmdbSearchHit> snapshot;
    auto fs = fetch_state_.load();
    {
        std::lock_guard<std::mutex> lk(similar_mu_);
        snapshot = similar_;
    }

    // Grid geometry: identical to BrowseScreen's layout.
    const int content_w = screen_w - 2 * kSafeInset;
    const int cell_w    = (content_w - (kGridCols - 1) * kCellGap) / kGridCols;
    const int poster_h  = static_cast<int>(static_cast<float>(cell_w) * 1.5f);
    const int cell_h    = poster_h + kMetaGap + kMetaTotalH;
    const int grid_top  = section_label_y + 18;
    const int grid_left = kSafeInset;

    // Footer hint band — reserve space so we don't clip the grid into it.
    const int footer_band_top = screen_h - kBezelInset - chrome::kFooterHeight_px - chrome::kPad2;

    if (fs == FetchState::InFlight && snapshot.empty()) {
        r.mb_draw_text("Loading related films\xE2\x80\xA6",
                       static_cast<float>(hx),
                       static_cast<float>(grid_top + poster_h / 2),
                       th.font_medium_size, th.dim, 1.0f);
    } else if (snapshot.empty()) {
        r.mb_draw_text("No related films found",
                       static_cast<float>(hx),
                       static_cast<float>(grid_top + poster_h / 2),
                       th.font_medium_size, th.dim, 1.0f);
    } else {
        // Two-row scroll window. Determine which row the cursor is in,
        // then compute the visible-row range (at most 2 rows visible).
        // 1-D cursor wraps left/right through the full list.
        const int n          = static_cast<int>(snapshot.size());
        const int cursor_row = cursor_ / kGridCols;
        // scroll_row_ is maintained by on_rotate; clamp here to be safe.
        const int max_rows     = (n + kGridCols - 1) / kGridCols;
        const int visible_rows = 2;
        // Keep cursor row in view (handled in on_rotate; belt-and-suspenders).
        int start_row = cursor_row;
        if (start_row > max_rows - 1) start_row = max_rows - 1;
        // Only scroll down when the second row would exist and cursor is on it.
        if (cursor_row >= 1) start_row = cursor_row - (cursor_row % visible_rows);
        start_row = std::max(0, std::min(start_row, max_rows - visible_rows));
        const int end_row = std::min(start_row + visible_rows, max_rows);

        for (int row = start_row; row < end_row; ++row) {
            for (int col = 0; col < kGridCols; ++col) {
                const int idx = row * kGridCols + col;
                if (idx >= n) break;
                const auto& hit = snapshot[idx];

                const int x = grid_left + col * (cell_w + kCellGap);
                const int y = grid_top  + (row - start_row) * (cell_h + kRowGap);

                // Stop rendering rows that would bleed into the footer.
                if (y + poster_h > footer_band_top) break;

                const ::ui::Color tint = poster_tint_for_tmdb(hit.tmdb_id);
                chrome::draw_poster_card(r, x, y, cell_w, poster_h,
                                         hit.title, hit.year,
                                         tint, /*in_library=*/false,
                                         /*download_pct=*/-1,
                                         hit.poster_path);

                // Two-line title wrap below poster (matches BrowseScreen logic).
                const std::string& title = hit.title;
                const float max_w_f = static_cast<float>(cell_w);
                std::string line1, line2;
                if (r.mb_text_width(title, kMetaFontPx) <= max_w_f) {
                    line1 = title;
                } else {
                    size_t split = std::string::npos;
                    size_t pos   = 0;
                    while (true) {
                        size_t next = title.find(' ', pos + 1);
                        if (next == std::string::npos) break;
                        if (r.mb_text_width(title.substr(0, next), kMetaFontPx) > max_w_f)
                            break;
                        split = next;
                        pos   = next;
                    }
                    if (split == std::string::npos) {
                        // No word boundary — truncate by characters.
                        std::string candidate = title;
                        while (!candidate.empty() &&
                               r.mb_text_width(candidate + "...", kMetaFontPx) > max_w_f) {
                            candidate.pop_back();
                        }
                        line1 = candidate.empty() ? title : (candidate + "...");
                    } else {
                        line1 = title.substr(0, split);
                        std::string rem = title.substr(split + 1);
                        if (r.mb_text_width(rem, kMetaFontPx) <= max_w_f) {
                            line2 = rem;
                        } else {
                            std::string candidate = rem;
                            while (!candidate.empty() &&
                                   r.mb_text_width(candidate + "...", kMetaFontPx) > max_w_f) {
                                candidate.pop_back();
                            }
                            line2 = candidate.empty() ? rem : (candidate + "...");
                        }
                    }
                }
                const int meta_top = y + poster_h + kMetaGap;
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

                // Gold focus ring on the cursor cell.
                if (idx == cursor_) {
                    chrome::draw_focus_ring(r, x, y, cell_w, poster_h);
                }
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
