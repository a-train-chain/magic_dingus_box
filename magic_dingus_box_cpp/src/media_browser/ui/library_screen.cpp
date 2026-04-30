#include "media_browser/ui/library_screen.h"
#include <filesystem>
#include <system_error>
#include <cstdio>
#include "media_browser/ui/mb_chrome.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>

#include "media_browser/radarr/radarr_client.h"
#include "ui/renderer.h"
#include "ui/theme.h"

namespace media_browser::ui {

namespace {

// Retro home-menu-inspired layout (target 1280x720), shared with
// detail_screen.cpp. The visual idiom mirrors the home menu:
//   - "LIBRARY" header in the Zen Dots title font, steel-blue (accent2),
//     underlined with a full-width 2px rule (same pattern as "Playlists")
//   - Section dividers in steel-blue (accent2)
//   - Gold-outlined chips (no fill) — matches the home-menu's
//     border-and-text aesthetic instead of filled blocks
//   - Blinking ◂ marker on the focused chip / poster — same 500ms cycle
//     and color (accent2) as the playlist-list selection cursor
//   - Status dots in green / gold / red corner badges (same palette as
//     the home menu's now-playing / missing indicators)
//
// All chrome dimensions (header rule Y, padding X, footer hint Y) MUST
// match detail_screen.cpp so the screens read as a single coherent UI
// when the user moves between them.
constexpr float kPaddingX        = 32.0f;
constexpr float kHeaderBaselineY = 38.0f;     // baseline of "LIBRARY"
constexpr float kHeaderRuleY     = 58.0f;     // 2px steel-blue rule below header

// Filter chip strip — sits just below the header rule. Outlined-only.
// kChipPadX is bumped to 18px (from 14) so each chip's body breathes a bit
// more — the strip only ever holds 4 fixed chips, so we have plenty of
// horizontal slack to spend on legibility.
constexpr float kStripTop        = 76.0f;     // top of chip strip
constexpr float kChipH           = 32.0f;
constexpr float kChipPadX        = 18.0f;
constexpr float kChipMinGap      = 14.0f;     // floor for the dynamic gap
constexpr float kChipBorderW     = 2.0f;

// "Marker zone" reserved at each chip's right edge for the blinking ◂
// focus cursor. The zone exists ALWAYS (focused or not) so chip widths
// don't shift on focus — the label is centered inside the chip's body
// MINUS this zone, which keeps the cursor from drawing over the last
// letter of long labels like "Missing Upgrades".
constexpr float kMarkerZoneW     = 22.0f;

// Section divider beneath the filter strip. Same 2px steel-blue rule
// idiom as kHeaderRuleY — bookends the chip strip on top and bottom.
constexpr float kStripRuleY      = 124.0f;

// Poster grid begins below the strip rule.
constexpr float kGridTop         = 144.0f;
constexpr float kCellGapX        = 20.0f;
constexpr float kCellGapY        = 24.0f;
constexpr float kLabelAreaH      = 52.0f;     // title + year below the poster
constexpr float kPosterFocusW    = 3.0f;      // focused gold outline thickness

// Status dot in the top-right corner of each poster. Drawn as a small
// filled square (matching the home menu's pixel-art status badges) with
// a dark halo so it stays legible regardless of poster artwork.
constexpr float kDotSize         = 14.0f;
constexpr float kDotInset        = 8.0f;

// Footer hint band — 12px from the bottom of the screen, mirroring
// detail_screen.cpp.
constexpr float kFooterMargin    = 12.0f;
constexpr float kFooterReserve   = 36.0f;     // vertical room reserved for hint

// Backdrop poster tint — same deterministic Knuth hash used by Browse
// and Detail, so a given movie has the same fallback color everywhere.
::ui::Color poster_tint_for_tmdb(int tmdb_id) {
    uint32_t h = static_cast<uint32_t>(tmdb_id) * 2654435761u;  // Knuth hash
    uint8_t r = 64 + static_cast<uint8_t>((h >>  0) & 0x7F);
    uint8_t g = 40 + static_cast<uint8_t>((h >>  8) & 0x5F);
    uint8_t b = 80 + static_cast<uint8_t>((h >> 16) & 0x7F);
    return {r, g, b, 255};
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

// True if the case-insensitive prefix of `s` matches `prefix`. Used by
// the file-state classifier (preserved verbatim from the previous
// revision — quality-string parsing is data-model logic, not visual).
bool starts_with_ci(const std::string& s, const char* prefix) {
    size_t i = 0;
    for (; prefix[i] != '\0'; ++i) {
        if (i >= s.size()) return false;
        char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
        char b = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[i])));
        if (a != b) return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

const char* LibraryScreen::label_for_filter(Filter f) {
    switch (f) {
        case Filter::All:             return "All";
        case Filter::Unwatched:       return "Unwatched";
        case Filter::MissingUpgrades: return "Missing Upgrades";
        case Filter::Recent:          return "Recent";
    }
    return "";
}

bool LibraryScreen::is_1080p_quality(const std::string& q) {
    // Treat "Bluray-*", "Bluray", and "WEBDL-1080p" / "WEB-DL-1080p" as
    // "good enough, no upgrade needed". Anything else (SDTV, DVD,
    // WEBDL-720p, HDTV-720p, etc.) counts as upgradeable. This is a
    // deliberate approximation — the real quality-profile cutoff check
    // lives in Radarr, but we don't have the cutoff id on the client side.
    if (q.empty()) return false;
    if (starts_with_ci(q, "Bluray")) return true;
    if (starts_with_ci(q, "WEBDL-1080p")) return true;
    if (starts_with_ci(q, "WEB-DL-1080p")) return true;
    return false;
}

LibraryScreen::FileState LibraryScreen::classify(const Movie& m) {
    if (!m.has_file) return FileState::MissingFile;
    if (is_1080p_quality(m.file_quality)) return FileState::HasGoodFile;
    return FileState::UpgradeAvailable;
}

// ---------------------------------------------------------------------------
// Construction / lifecycle
// ---------------------------------------------------------------------------

LibraryScreen::LibraryScreen(RadarrClient& radarr) : radarr_(radarr) {}

void LibraryScreen::enter() {
    // Always refresh on (re-)entry so the library list reflects any
    // adds/removes that happened in DetailScreen since we were last
    // visible. The call is cheap on the mock client and acceptable for
    // the MVP cadence on the real HTTP client.
    reload();
}

void LibraryScreen::reload() {
    library_ = radarr_.get_library();
    loaded_ = true;
    rebuild_view();
}

void LibraryScreen::rebuild_view() {
    view_.clear();
    view_.reserve(library_.size());

    for (const auto& m : library_) {
        switch (filter_) {
            case Filter::All:
                view_.push_back(&m);
                break;
            case Filter::Unwatched:
                // MVP-scope: we don't track "watched" state anywhere yet,
                // so this collapses to "has_file == true" (i.e., the
                // films you could watch right now). Swap this for a real
                // watched-state lookup when view tracking lands.
                if (m.has_file) view_.push_back(&m);
                break;
            case Filter::MissingUpgrades:
                if (m.has_file && !is_1080p_quality(m.file_quality)) {
                    view_.push_back(&m);
                }
                break;
            case Filter::Recent:
                view_.push_back(&m);
                break;
        }
    }

    if (filter_ == Filter::Recent) {
        // Sort by added_at descending. ISO-8601 timestamps sort
        // lexicographically in chronological order, so plain string
        // comparison is fine — no need to parse the date.
        std::sort(view_.begin(), view_.end(),
                  [](const Movie* a, const Movie* b) {
                      return a->added_at > b->added_at;
                  });
    }

    // Reset cursor/scroll if the filter shrank the view under them.
    int n = static_cast<int>(view_.size());
    if (grid_cursor_ >= n) grid_cursor_ = std::max(0, n - 1);
    if (grid_cursor_ < 0) grid_cursor_ = 0;
    int cursor_row = n == 0 ? 0 : grid_cursor_ / kGridCols;
    if (scroll_row_ > cursor_row) scroll_row_ = cursor_row;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

Screen LibraryScreen::handle_input(const std::vector<platform::InputEvent>& events) {
    // LibraryScreen sits at strip position 2 in the Marquee 5-tab strip:
    //   Popular(0) · Top Rated(1) · Library(2) · Search(3) · Settings(4)
    // BTN1 (PREV, yellow) returns to BrowseScreen — BrowseScreen retains
    // its category_ across transitions, so the user resumes on whatever
    // content tab they were on before navigating to Library (typically
    // TopRated, the immediate left neighbour). BTN3 (NEXT, green)
    // transitions to SearchScreen.
    for (const auto& e : events) {
        // BTN4 (SETTINGS_MENU, black) — opens the slide-in overlay.
        // Wired in Task 5. For now: no-op short-press; long-press
        // still exits MB via the input dispatcher.
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            continue;
        }

        // BTN1 (PREV, yellow) — previous tab. Library is at position 3 in
        // the 6-tab strip (Popular | Top Rated | Search | Library | Queue
        // | Settings), so PREV lands on Search at position 2.
        if (e.action == platform::InputAction::PREV && e.pressed) {
            return Screen::Search;
        }

        // BTN3 (NEXT, green) — next tab. Library is at position 3, NEXT
        // goes to Queue at position 4.
        if (e.action == platform::InputAction::NEXT && e.pressed) {
            return Screen::Queue;
        }

        // BTN2 (PLAY_PAUSE, red) — back. Library's parent in the back
        // stack is Browse; Browse retains its last-active content
        // category so the operator returns to wherever they were
        // (Popular / Top Rated) before opening Library.
        if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed) {
            return Screen::Browse;
        }

        // ROTATE (rotary CW/CCW + D-pad LEFT/RIGHT) — walk one cell at
        // a time, row-major.
        if (e.action == platform::InputAction::ROTATE) {
            if (view_.empty()) continue;
            const int n = static_cast<int>(view_.size());
            grid_cursor_ = std::clamp(grid_cursor_ + e.delta, 0, n - 1);
            continue;
        }

        // ROTATE_VERTICAL (D-pad UP/DOWN) — walk one row at a time.
        if (e.action == platform::InputAction::ROTATE_VERTICAL) {
            if (view_.empty()) continue;
            const int row = grid_cursor_ / kGridCols;
            const int col = grid_cursor_ % kGridCols;
            const int max_row = (static_cast<int>(view_.size()) - 1) / kGridCols;
            const int new_row = std::clamp(row + e.delta, 0, max_row);
            const int new_idx = new_row * kGridCols + col;
            if (new_idx < static_cast<int>(view_.size())) {
                grid_cursor_ = new_idx;
            } else if (!view_.empty()) {
                grid_cursor_ = static_cast<int>(view_.size()) - 1;
            }
            continue;
        }

        // SELECT (rotary click + gamepad A) — open detail.
        if (e.action == platform::InputAction::SELECT && e.pressed) {
            if (!view_.empty() && grid_cursor_ >= 0 &&
                grid_cursor_ < static_cast<int>(view_.size())) {
                selected_tmdb_id_ = view_[grid_cursor_]->tmdb_id;
                return Screen::Detail;
            }
        }
    }

    // Keep cursor visible. Render uses 2 visible rows; clamp lower bound
    // here, upper bound clamps in render once visible_rows is known.
    if (!view_.empty()) {
        const int row = grid_cursor_ / kGridCols;
        if (row < scroll_row_) scroll_row_ = row;
    }

    return Screen::Library;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

namespace {
// Format a byte count as a human-readable size string.
// "0 B", "23 KB", "1.4 MB", "12.3 GB", etc.
std::string format_bytes(int64_t bytes) {
    if (bytes < 1024) {
        return std::to_string(bytes) + " B";
    }
    constexpr double k = 1024.0;
    constexpr double m = k * 1024.0;
    constexpr double g = m * 1024.0;
    const double f = static_cast<double>(bytes);
    char buf[32];
    if (bytes < 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", f / k);
    } else if (bytes < 1024 * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", f / m);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f GB", f / g);
    }
    return buf;
}

// Deterministic colored tint for a movie's tmdb_id, matching BrowseScreen.
::ui::Color library_tint_for_tmdb(int tmdb_id) {
    uint32_t h = static_cast<uint32_t>(tmdb_id) * 2654435761u;
    uint8_t r = 64 + static_cast<uint8_t>((h >>  0) & 0x7F);
    uint8_t g = 40 + static_cast<uint8_t>((h >>  8) & 0x5F);
    uint8_t b = 80 + static_cast<uint8_t>((h >> 16) & 0x7F);
    return {r, g, b, 255};
}
}  // namespace

void LibraryScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    namespace chrome = ::media_browser::ui::chrome;
    const ::ui::Theme& th = r.mb_theme();

    r.mb_fill_background();

    // --- 5-tab Marquee header (same strip BrowseScreen renders, Library active here) ---
    // Labels are hardcoded so LibraryScreen doesn't need to peek at
    // BrowseScreen's private Category enum. They must stay in sync with
    // kVisibleTabs[] in browse_screen.cpp.
    // v1.6.x: Now Playing dropped (overlapped Popular), Settings added on
    // the right end of the strip — so Library shifts from position 3 → 2.
    static constexpr const char* kTabLabels[] = {
        "Popular", "Top Rated", "Search", "Library", "Queue", "Settings",
    };
    constexpr int kNumVisibleTabs = 6;
    constexpr int kLibraryStripPos = 3;

    std::vector<chrome::TabSpec> tabs;
    tabs.reserve(kNumVisibleTabs);
    for (int i = 0; i < kNumVisibleTabs; ++i) {
        chrome::TabSpec t;
        t.label = kTabLabels[i];
        t.state = (i == kLibraryStripPos)
                      ? chrome::TabState::Active
                      : chrome::TabState::Inactive;
        tabs.push_back(t);
    }
    const int content_top = chrome::draw_screen_header(
        r, screen_w, "Library", tabs, /*focused_tab=*/-1);

    // --- Stats line: "N titles · X.X GB · Y.Y GB free" ---
    int64_t used_bytes = 0;
    for (const Movie& m : library_) {
        if (m.has_file) used_bytes += m.file_size_bytes;
    }
    int64_t free_bytes = 0;
    {
        std::error_code ec;
        auto info = std::filesystem::space("/mnt/ssd/library", ec);
        if (!ec) free_bytes = static_cast<int64_t>(info.available);
    }
    const std::string stats =
        std::to_string(library_.size()) + " titles  ·  "
        + format_bytes(used_bytes) + " used  ·  "
        + (free_bytes > 0 ? format_bytes(free_bytes) + " free" : "");
    const int stats_y = content_top + chrome::kPad2;
    r.mb_draw_text(stats,
                   static_cast<float>(chrome::kSafeInset_px),
                   static_cast<float>(stats_y + 14),
                   14, th.dim);

    // Sort sub-tabs (Recent / Title / Year / Size) — ZenDots, matching
    // the font family used across the main playlist UI's section
    // headers and the Marquee header tabs. Active sort gets the gold
    // border + gold text via chrome::TabSpec rendered through the same
    // tab helper so visual styling stays in lockstep with the main
    // 5-tab strip above. Sort cycling is still deferred per operator
    // direction; "Recent" hardcoded as active.
    {
        static constexpr const char* kSortLabels[] = {
            "Recent", "Title", "Year", "Size",
        };
        constexpr int kNumSorts = 4;
        constexpr int kSortFontPx = 18;
        constexpr int kSortGap = chrome::kPad4;
        // Pre-measure each label's natural width (ZenDots) so we can
        // right-align the strip inside the safe area.
        int total_w = 0;
        for (int i = 0; i < kNumSorts; ++i) {
            total_w += r.mb_title_text_width(kSortLabels[i], kSortFontPx);
            if (i + 1 < kNumSorts) total_w += kSortGap;
        }
        int x = screen_w - chrome::kSafeInset_px - total_w;
        const int sort_baseline = stats_y + 14;  // align to stats line
        for (int i = 0; i < kNumSorts; ++i) {
            const ::ui::Color color =
                (i == 0) ? th.accent : th.dim;   // active = gold, others dim
            r.mb_draw_title_text(kSortLabels[i],
                                 static_cast<float>(x),
                                 static_cast<float>(sort_baseline),
                                 kSortFontPx, color);
            x += r.mb_title_text_width(kSortLabels[i], kSortFontPx) + kSortGap;
        }
    }

    // --- Empty state ---
    if (!loaded_) {
        const std::string msg = "Loading library...";
        const int tw = r.mb_text_width(msg, 18);
        r.mb_draw_text(msg,
                       static_cast<float>((screen_w - tw) / 2),
                       static_cast<float>(screen_h / 2),
                       18, th.dim);
        chrome::draw_footer_hints(r, screen_w, screen_h, {
            {"BTN1/3", "Tabs"},
            {"BTN4",   "Home"},
        });
        return;
    }
    if (view_.empty()) {
        const std::string msg = library_.empty()
            ? "Library is empty — add movies from Browse"
            : "No matches for the current filter";
        const int tw = r.mb_text_width(msg, 18);
        r.mb_draw_text(msg,
                       static_cast<float>((screen_w - tw) / 2),
                       static_cast<float>(screen_h / 2),
                       18, th.dim);
        chrome::draw_footer_hints(r, screen_w, screen_h, {
            {"BTN1/3", "Tabs"},
            {"BTN4",   "Home"},
        });
        return;
    }

    // --- 9-column poster grid ---
    // Same layout as BrowseScreen: meta area fits 2 lines so long
    // titles can wrap; year lives inside the poster card (bottom-right
    // pill) so the meta line shows the title only.
    constexpr int kCellGap       = 8;
    constexpr int kRowGap        = 22;
    constexpr int kVisibleRows   = 2;
    constexpr int kMetaFontPx    = 14;
    constexpr int kMetaLineGap   = 2;
    constexpr int kMetaTotalH    = kMetaFontPx + kMetaLineGap + kMetaFontPx;
    constexpr int kMetaGap       = 4;
    const int content_w = screen_w - 2 * chrome::kSafeInset_px;
    const int cell_w   = (content_w - (kGridCols - 1) * kCellGap) / kGridCols;
    const int poster_h = static_cast<int>(static_cast<float>(cell_w) * 1.5f);
    const int cell_h   = poster_h + kMetaGap + kMetaTotalH;
    const int grid_top = stats_y + 14 + chrome::kPad3;
    const int grid_left = chrome::kSafeInset_px;

    const int cursor_row = grid_cursor_ / kGridCols;
    if (cursor_row >= scroll_row_ + kVisibleRows) {
        scroll_row_ = cursor_row - kVisibleRows + 1;
    }
    const int total_rows =
        (static_cast<int>(view_.size()) + kGridCols - 1) / kGridCols;
    const int last_visible_row =
        std::min(scroll_row_ + kVisibleRows, total_rows);

    for (int row = scroll_row_; row < last_visible_row; ++row) {
        for (int col = 0; col < kGridCols; ++col) {
            const int idx = row * kGridCols + col;
            if (idx >= static_cast<int>(view_.size())) break;
            const Movie* mv = view_[idx];

            const int x = grid_left + col * (cell_w + kCellGap);
            const int y = grid_top + (row - scroll_row_) * (cell_h + kRowGap);

            // Poster CARD via shared chrome helper — real TMDB image
            // when loaded, deterministic tint placeholder while it
            // fetches, plus year pill + IN LIBRARY badge.
            const ::ui::Color tint = library_tint_for_tmdb(mv->tmdb_id);
            chrome::draw_poster_card(
                r, x, y, cell_w, poster_h,
                mv->title, mv->year,
                tint, /*in_library=*/true, /*download_pct=*/-1,
                /*poster_url=*/mv->poster_url);

            // Meta line below poster: title only, wrapped to 2 lines
            // when needed. Year now lives inside the poster card.
            // Line 1 = longest leading word chunk that fits on one
            // line; line 2 = remainder, truncated with ellipsis if it
            // also overflows. Same logic BrowseScreen uses — kept
            // inline (rather than factored into mb_chrome) because the
            // truncation pattern depends on the renderer instance.
            const std::string& title = mv->title;
            const float max_w_f = static_cast<float>(cell_w);
            std::string line1, line2;
            if (r.mb_text_width(title, kMetaFontPx) <= max_w_f) {
                line1 = title;
            } else {
                size_t split = std::string::npos;
                size_t pos = 0;
                while (true) {
                    size_t next = title.find(' ', pos + 1);
                    if (next == std::string::npos) break;
                    if (r.mb_text_width(title.substr(0, next), kMetaFontPx)
                            > max_w_f) break;
                    split = next;
                    pos = next;
                }
                if (split == std::string::npos) {
                    // No space to break at — character-level truncate
                    // with trailing ellipsis.
                    std::string candidate = title;
                    while (!candidate.empty()
                           && r.mb_text_width(candidate + "...", kMetaFontPx)
                              > max_w_f) {
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
                        while (!candidate.empty()
                               && r.mb_text_width(candidate + "...", kMetaFontPx)
                                  > max_w_f) {
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

            if (idx == grid_cursor_) {
                chrome::draw_focus_ring(r, x, y, cell_w, poster_h);
            }
        }
    }

    // --- Footer hints ---
    chrome::draw_footer_hints(r, screen_w, screen_h, {
        {"BTN1/3", "Tabs"},
        {"Rotary", "Scroll"},
        {"A",      "Open"},
        {"BTN4",   "Home"},
    });
}

}  // namespace media_browser::ui
