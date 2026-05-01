#include "media_browser/ui/playback_overlay.h"

#include <spdlog/spdlog.h>

#include "media_browser/tmdb_client.h"
#include "media_browser/ui/mb_chrome.h"
#include "ui/renderer.h"
#include "ui/theme.h"

#include <algorithm>

namespace media_browser::ui {

namespace {

// Layout constants — designed for 1280x720. The overlay occupies the lower
// 360 px of the screen (the bottom ~50%), leaving the upper half for the
// playing video to remain visible.
constexpr int kPanelHeightPx    = 360;
constexpr float kPanelAlpha     = 0.88f;

// Header band inside the panel: shows movie meta (poster thumb + text).
constexpr int kHeaderHeightPx   = 110;
constexpr int kPosterThumbW     = 66;
constexpr int kPosterThumbH     = 100;
constexpr int kPaddingX         = chrome::kSafeInset_px;  // 60 px
constexpr int kPaddingY         = 12;

// Carousel card dimensions.
constexpr int kCardW            = 110;
constexpr int kCardH            = 165;
constexpr int kCardGap          = 14;
constexpr int kVisibleCards     = 7;   // at 1280 px wide, up to ~7 cards fit
constexpr int kMaxSimilar       = 20;

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
    // Panel background
    // -----------------------------------------------------------------------
    const int panel_y = screen_h - kPanelHeightPx;
    r.mb_fill_rect(static_cast<float>(0),
                   static_cast<float>(panel_y),
                   static_cast<float>(screen_w),
                   static_cast<float>(kPanelHeightPx),
                   th.bg_lift, kPanelAlpha);

    // Top rule (accent color, full width).
    r.mb_fill_rect(0.0f, static_cast<float>(panel_y),
                   static_cast<float>(screen_w), 2.0f,
                   th.accent, 1.0f);

    // -----------------------------------------------------------------------
    // Header: poster thumbnail placeholder + movie meta text
    // -----------------------------------------------------------------------
    const int hx = kPaddingX;
    const int hy = panel_y + kPaddingY;

    // Poster thumbnail placeholder (filled rect with dim border).
    r.mb_fill_rect(static_cast<float>(hx), static_cast<float>(hy),
                   static_cast<float>(kPosterThumbW),
                   static_cast<float>(kPosterThumbH),
                   th.bg, 1.0f);
    // Top border
    r.mb_fill_rect(static_cast<float>(hx), static_cast<float>(hy),
                   static_cast<float>(kPosterThumbW), 1.0f, th.dim, 0.8f);
    // Bottom border
    r.mb_fill_rect(static_cast<float>(hx),
                   static_cast<float>(hy + kPosterThumbH - 1),
                   static_cast<float>(kPosterThumbW), 1.0f, th.dim, 0.8f);
    // Left border
    r.mb_fill_rect(static_cast<float>(hx), static_cast<float>(hy),
                   1.0f, static_cast<float>(kPosterThumbH), th.dim, 0.8f);
    // Right border
    r.mb_fill_rect(static_cast<float>(hx + kPosterThumbW - 1),
                   static_cast<float>(hy),
                   1.0f, static_cast<float>(kPosterThumbH), th.dim, 0.8f);

    // Meta text: title line
    const int tx = hx + kPosterThumbW + 16;
    std::string meta_line = meta_.title;
    if (meta_.year > 0)        meta_line += "  ·  " + std::to_string(meta_.year);
    if (meta_.runtime_min > 0) meta_line += "  ·  " + std::to_string(meta_.runtime_min) + "m";
    if (!meta_.genres.empty()) meta_line += "  ·  " + meta_.genres;
    r.mb_draw_text(meta_line,
                   static_cast<float>(tx), static_cast<float>(hy + 4),
                   th.font_medium_size, th.fg, 1.0f);

    // Synopsis (truncated).
    std::string syn = meta_.synopsis;
    if (syn.size() > kSynopsisMaxChars) syn = syn.substr(0, kSynopsisMaxChars) + "\xE2\x80\xA6";
    r.mb_draw_text(syn,
                   static_cast<float>(tx), static_cast<float>(hy + 32),
                   th.font_small_size, th.dim, 1.0f);

    // "Similar films" label
    r.mb_draw_text("Similar films",
                   static_cast<float>(tx), static_cast<float>(hy + 68),
                   th.font_small_size, th.accent, 1.0f);

    // -----------------------------------------------------------------------
    // Divider between header and carousel strip
    // -----------------------------------------------------------------------
    const int strip_y = panel_y + kHeaderHeightPx + 8;
    r.mb_fill_rect(0.0f, static_cast<float>(panel_y + kHeaderHeightPx),
                   static_cast<float>(screen_w), 1.0f, th.dim, 0.4f);

    // -----------------------------------------------------------------------
    // Carousel strip — snapshot under lock to avoid holding it during render
    // -----------------------------------------------------------------------
    std::vector<::media_browser::TmdbSearchHit> snapshot;
    auto fs = fetch_state_.load();
    {
        std::lock_guard<std::mutex> lk(similar_mu_);
        snapshot = similar_;
    }

    if (fs == FetchState::InFlight && snapshot.empty()) {
        r.mb_draw_text("Loading related films\xE2\x80\xA6",
                       static_cast<float>(kPaddingX),
                       static_cast<float>(strip_y + 60),
                       th.font_medium_size, th.dim, 1.0f);
        return;
    }
    if (snapshot.empty()) {
        r.mb_draw_text("No related films found",
                       static_cast<float>(kPaddingX),
                       static_cast<float>(strip_y + 60),
                       th.font_medium_size, th.dim, 1.0f);
        return;
    }

    // Scroll window: keep cursor centred where possible.
    int n = static_cast<int>(snapshot.size());
    int first_visible = 0;
    if (cursor_ >= kVisibleCards) {
        first_visible = cursor_ - kVisibleCards + 1;
    }

    for (int i = 0; i < kVisibleCards && (first_visible + i) < n; ++i) {
        int idx = first_visible + i;
        const auto& hit = snapshot[idx];
        int cx = kPaddingX + i * (kCardW + kCardGap);
        int cy = strip_y;

        const ::ui::Color tint = poster_tint_for_tmdb(hit.tmdb_id);

        // draw_poster_card(r, x, y, w, h, title, year, tint, in_library,
        //                  download_pct, poster_url)
        // poster_path already contains the full URL from TmdbClient.
        chrome::draw_poster_card(r, cx, cy, kCardW, kCardH,
                                 hit.title,
                                 hit.year,
                                 tint,
                                 /*in_library=*/false,
                                 /*download_pct=*/-1,
                                 hit.poster_path);

        // Focus ring around the selected card.
        if (idx == cursor_) {
            // Top bar
            r.mb_fill_rect(static_cast<float>(cx - 2),
                           static_cast<float>(cy - 2),
                           static_cast<float>(kCardW + 4), 2.0f,
                           th.accent, 1.0f);
            // Bottom bar
            r.mb_fill_rect(static_cast<float>(cx - 2),
                           static_cast<float>(cy + kCardH),
                           static_cast<float>(kCardW + 4), 2.0f,
                           th.accent, 1.0f);
            // Left bar
            r.mb_fill_rect(static_cast<float>(cx - 2),
                           static_cast<float>(cy - 2),
                           2.0f, static_cast<float>(kCardH + 4),
                           th.accent, 1.0f);
            // Right bar
            r.mb_fill_rect(static_cast<float>(cx + kCardW),
                           static_cast<float>(cy - 2),
                           2.0f, static_cast<float>(kCardH + 4),
                           th.accent, 1.0f);
        }
    }

    // Toast confirmation: shown for 2 s next to the focused poster card
    // after a quick-add action (wired by Task 9 in playback_screen.cpp).
    if (toast_active_) {
        using namespace std::chrono;
        auto elapsed_ms = duration_cast<milliseconds>(
            steady_clock::now() - toast_started_at_).count();
        if (elapsed_ms > 2000) {
            toast_active_ = false;
        } else {
            int focused_screen_idx = cursor_ - first_visible;
            if (focused_screen_idx >= 0 && focused_screen_idx < kVisibleCards) {
                int tx = kPaddingX + focused_screen_idx * (kCardW + kCardGap);
                int ty = strip_y + kCardH + 8;
                int tw = static_cast<int>(r.mb_text_width(toast_msg_, 14)) + 16;
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
    }

    // Footer hint row inside the panel (matches mb_chrome visual style).
    // "Rotary twist = scroll · BTN4 = close · Rotary press = add"
    chrome::draw_hint_row(r,
                          kPaddingX,
                          panel_y + kPanelHeightPx - chrome::kPad2,
                          {
                              {chrome::HintIcon::RotaryNav,   "Scroll"},
                              {chrome::HintIcon::Btn4Black,   "Close"},
                              {chrome::HintIcon::RotaryPress, "Add"},
                          });
}

}  // namespace media_browser::ui
