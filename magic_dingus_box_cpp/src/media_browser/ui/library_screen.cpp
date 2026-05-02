#include "media_browser/ui/library_screen.h"
#include <filesystem>
#include <system_error>
#include <cstdio>
#include "media_browser/ui/mb_chrome.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <sstream>
#include <string>
#include <strings.h>  // strcasecmp
#include <unordered_map>

#include "app/settings_persistence.h"
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

// Slide-in overlay (v1.6.x).
//   Panel sits 20 px inside the wood-frame's right inner edge so the
//   right gold rule reads as a closed 4-sided box (right edge x=1220,
//   bezel inner edge x=1240).
//   Width 480 px → left edge at x=740 when fully open.
//   Vertical extent: y=120 (chrome header bottom) → y=634 (chrome
//   footer hint band top), 514 px tall.
constexpr int kOverlayPanelW         = 480;
constexpr int kOverlayPanelRightGap  = 20;      // Gap between right edge and bezel inner edge
constexpr int kOverlayPanelOpenX     = 1280 - 40 - kOverlayPanelRightGap - kOverlayPanelW;  // 740
constexpr int kOverlayPanelClosedX   = 1280;    // Left edge when fully closed (off-screen right)
constexpr int kOverlayPanelTopY      = 120;
constexpr int kOverlayPanelBottomY   = 634;
constexpr int kOverlayPanelH         = kOverlayPanelBottomY - kOverlayPanelTopY;
constexpr int kOverlayPanelInnerPadX = 24;
constexpr int kOverlayPanelInnerPadY = 16;

// Block layout inside the panel — three logical sections, top to bottom.
constexpr int kOverlaySectionGap     = 18;   // Vertical gap between section blocks
constexpr int kOverlayRowHeight      = 32;   // Per row in Sort + Filter sections
constexpr int kOverlaySectionHeaderH = 28;   // Gold ZenDots heading + gap

// Ease curve for the slide-in animation. Cubic ease-out: 1 - (1-t)^3.
inline float ease_out_cubic(float t) {
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}
// Ease-in for slide-out: t^3.
inline float ease_in_cubic(float t) {
    return t * t * t;
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

LibraryScreen::LibraryScreen(RadarrClient& radarr, ::app::AppState& state)
    : radarr_(radarr), state_(state) {}

// ---------------------------------------------------------------------------
// Slide-in overlay state machine (Task 4 of v1.6.x library-overlay plan).
// Render + input wiring land in Tasks 5 + 6; for now these helpers exist so
// the state machine ticks every frame and the open/close transitions are
// callable from input handling once it's wired up.
// ---------------------------------------------------------------------------

void LibraryScreen::start_open_overlay() {
    overlay_state_ = OverlayState::SlidingIn;
    overlay_anim_started_at_ = std::chrono::steady_clock::now();
    // Cursor lands on the currently-active sort row so a single A
    // re-confirms the existing choice (zero-effort cancel).
    using S = ::app::AppState::DisplaySettings::MbLibrarySort;
    switch (state_.display_settings.mb_library_sort) {
        case S::Recent: overlay_focus_row_ = 0; break;
        case S::Title:  overlay_focus_row_ = 1; break;
        case S::Year:   overlay_focus_row_ = 2; break;
        case S::Size:   overlay_focus_row_ = 3; break;
    }
}

void LibraryScreen::start_close_overlay() {
    overlay_state_ = OverlayState::SlidingOut;
    overlay_anim_started_at_ = std::chrono::steady_clock::now();
}

void LibraryScreen::tick_overlay_animation() {
    if (overlay_state_ == OverlayState::Closed ||
        overlay_state_ == OverlayState::Open) return;
    const auto elapsed_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - overlay_anim_started_at_)
        .count();
    if (overlay_state_ == OverlayState::SlidingIn && elapsed_ms >= kOverlaySlideInMs) {
        overlay_state_ = OverlayState::Open;
    } else if (overlay_state_ == OverlayState::SlidingOut && elapsed_ms >= kOverlaySlideOutMs) {
        overlay_state_ = OverlayState::Closed;
    }
}

int LibraryScreen::compute_overlay_left_x(
        OverlayState state,
        std::chrono::steady_clock::time_point anim_started) {
    if (state == OverlayState::Closed)  return kOverlayPanelClosedX;
    if (state == OverlayState::Open)    return kOverlayPanelOpenX;
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(now - anim_started).count();
    if (state == OverlayState::SlidingIn) {
        const float t = std::min(1.0f,
            static_cast<float>(elapsed_ms) / static_cast<float>(kOverlaySlideInMs));
        const float eased = ease_out_cubic(t);
        return static_cast<int>(
            kOverlayPanelClosedX + eased *
                (kOverlayPanelOpenX - kOverlayPanelClosedX));
    }
    // SlidingOut.
    const float t = std::min(1.0f,
        static_cast<float>(elapsed_ms) / static_cast<float>(kOverlaySlideOutMs));
    const float eased = ease_in_cubic(t);
    return static_cast<int>(
        kOverlayPanelOpenX + eased *
            (kOverlayPanelClosedX - kOverlayPanelOpenX));
}

void LibraryScreen::enter() {
    // Always refresh on (re-)entry so the library list reflects any
    // adds/removes that happened in DetailScreen since we were last
    // visible. The call is cheap on the mock client and acceptable for
    // the MVP cadence on the real HTTP client.
    reload();
}

void LibraryScreen::reload() {
    library_ = radarr_.get_library();
    // Build radarr_id → tmdb_id map so we can cross-reference the queue.
    std::unordered_map<int, int> radarr_to_tmdb;
    for (const auto& m : library_) {
        if (m.tmdb_id > 0) radarr_to_tmdb[m.radarr_id] = m.tmdb_id;
    }
    // Populate the downloading + stuck sets from the current Radarr queue.
    // A queue item is "stuck" when it has finished downloading (state ==
    // "completed") but Radarr can't import it (trackedDownloadState ==
    // "importBlocked"). Those items get a BAD RELEASE badge instead of
    // DOWNLOADING so the user can pick a different source.
    downloading_tmdb_ids_.clear();
    stuck_tmdb_ids_.clear();
    for (const auto& qi : radarr_.get_queue()) {
        auto it = radarr_to_tmdb.find(qi.movie_id);
        if (it == radarr_to_tmdb.end()) continue;
        const int tmdb_id = it->second;
        const bool is_stuck = (qi.state == "completed" &&
                               qi.tracked_download_state == "importBlocked");
        if (is_stuck) {
            stuck_tmdb_ids_.insert(tmdb_id);
        } else {
            downloading_tmdb_ids_.insert(tmdb_id);
        }
    }
    loaded_ = true;
    rebuild_view();
}

void LibraryScreen::rebuild_view() {
    view_.clear();
    view_.reserve(library_.size());

    // ---- Filter pass ----
    // Driven by display_settings.mb_library_filter (the v1.6.x persisted
    // overlay choice), NOT the legacy Filter enum member filter_ which
    // is now dead config. The legacy enum stays in the header for any
    // ABI compat we haven't audited but does not influence the view.
    using F = ::app::AppState::DisplaySettings::MbLibraryFilter;
    const F filter = state_.display_settings.mb_library_filter;
    // "Recently added" cutoff: 30 days ago, formatted as an ISO-8601
    // string so we can compare lexicographically against Movie.added_at
    // (which Radarr emits as ISO-8601, e.g. "2024-12-31T08:15:42Z").
    // ISO-8601 strings sort chronologically as plain strings — no
    // parsing required.
    std::string thirty_days_ago_iso;
    {
        const auto now = std::chrono::system_clock::now();
        const auto cutoff = now - std::chrono::hours(24 * 30);
        const std::time_t tt = std::chrono::system_clock::to_time_t(cutoff);
        std::tm tm_utc{};
        gmtime_r(&tt, &tm_utc);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
        thirty_days_ago_iso = buf;
    }
    for (const Movie& m : library_) {
        bool keep = true;
        switch (filter) {
            case F::All:
                keep = true;
                break;
            case F::Unwatched:
                // Placeholder: kiosk doesn't track watched-history yet.
                // Accept all rows so the operator sees something while
                // the (soon) feature is in development. Will switch to
                // `keep = !m.watched;` once Movie.watched lands.
                keep = true;
                break;
            case F::MissingFiles:
                keep = !m.has_file;
                break;
            case F::RecentlyAdded:
                // Movie.added_at is a Radarr ISO-8601 string. Empty
                // strings (which Radarr should never emit but we guard
                // anyway) compare less-than the cutoff and are dropped.
                keep = (!m.added_at.empty() && m.added_at >= thirty_days_ago_iso);
                break;
        }
        if (keep) view_.push_back(&m);
    }

    // ---- Sort pass ----
    using S = ::app::AppState::DisplaySettings::MbLibrarySort;
    const S sort = state_.display_settings.mb_library_sort;
    auto cmp_recent = [](const Movie* a, const Movie* b) {
        return a->added_at > b->added_at;  // newest first (ISO-8601 lex sort)
    };
    auto cmp_title = [](const Movie* a, const Movie* b) {
        return ::strcasecmp(a->title.c_str(), b->title.c_str()) < 0;
    };
    auto cmp_year = [](const Movie* a, const Movie* b) {
        if (a->year != b->year) return a->year > b->year;  // newest first
        return ::strcasecmp(a->title.c_str(), b->title.c_str()) < 0;
    };
    auto cmp_size = [](const Movie* a, const Movie* b) {
        return a->file_size_bytes > b->file_size_bytes;  // largest first
    };
    switch (sort) {
        case S::Recent: std::sort(view_.begin(), view_.end(), cmp_recent); break;
        case S::Title:  std::sort(view_.begin(), view_.end(), cmp_title);  break;
        case S::Year:   std::sort(view_.begin(), view_.end(), cmp_year);   break;
        case S::Size:   std::sort(view_.begin(), view_.end(), cmp_size);   break;
    }

    // Clamp the grid cursor + scroll row to the new view's bounds.
    // After a sort/filter change, the cursor may land far outside the
    // new view's row range — clamp grid_cursor_ to [0, n-1] first, then
    // ensure scroll_row_ keeps the cursor visible (i.e. scroll_row_ ≤
    // cursor_row). Without the visibility clamp, an operator scrolled
    // down before applying a filter that shrinks the view would see an
    // empty grid until the next render's render-side clamp catches up.
    const int n = static_cast<int>(view_.size());
    if (grid_cursor_ >= n) grid_cursor_ = std::max(0, n - 1);
    // Page-based: snap scroll to the page that contains the (possibly clamped) cursor.
    // 2 = kPageRows (same value as kVisibleRows in render()).
    const int cursor_row = (n == 0) ? 0 : grid_cursor_ / kGridCols;
    scroll_row_ = (cursor_row / 2) * 2;
    const int max_scroll_row = std::max(0, (n - 1) / kGridCols);
    if (scroll_row_ > max_scroll_row) scroll_row_ = max_scroll_row;
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
        // ============================================================
        // Overlay input capture: when the overlay is open or animating,
        // it consumes ROTATE / SELECT / SETTINGS_MENU inputs. The
        // underlying grid does NOT see these events while the panel is
        // visible. SETTINGS_MENU is handled separately below as a
        // toggle (open/close). BTN2 (PLAY_PAUSE) is owned globally by
        // the exit modal in main.cpp and never arrives here.
        // ============================================================
        const bool overlay_active = (overlay_state_ != OverlayState::Closed);
        if (overlay_active) {
            // Rotary CW/CCW — walk through the 8 focusable rows.
            if (e.action == platform::InputAction::ROTATE) {
                if (e.delta == 0) continue;
                overlay_focus_row_ = std::clamp(
                    overlay_focus_row_ + e.delta,
                    0, kOverlayFocusableRows - 1);
                continue;
            }
            // D-pad UP / DOWN — also walks through the 8 focusable rows
            // so the operator can use either rotary OR D-pad to move
            // around the overlay. Without this clause the underlying
            // grid's ROTATE_VERTICAL handler would silently mutate
            // grid_cursor_ while the overlay is occluding the grid.
            if (e.action == platform::InputAction::ROTATE_VERTICAL) {
                if (e.delta == 0) continue;
                overlay_focus_row_ = std::clamp(
                    overlay_focus_row_ + e.delta,
                    0, kOverlayFocusableRows - 1);
                continue;
            }
            // SELECT — apply sort/filter, persist, close overlay.
            if (e.action == platform::InputAction::SELECT && e.pressed) {
                if (overlay_state_ != OverlayState::Open) continue;
                // Map focused row → sort or filter mutation.
                using S = ::app::AppState::DisplaySettings::MbLibrarySort;
                using F = ::app::AppState::DisplaySettings::MbLibraryFilter;
                bool changed = false;
                switch (overlay_focus_row_) {
                    case 0: state_.display_settings.mb_library_sort = S::Recent;        changed = true; break;
                    case 1: state_.display_settings.mb_library_sort = S::Title;         changed = true; break;
                    case 2: state_.display_settings.mb_library_sort = S::Year;          changed = true; break;
                    case 3: state_.display_settings.mb_library_sort = S::Size;          changed = true; break;
                    case 4: state_.display_settings.mb_library_filter = F::All;           changed = true; break;
                    case 5: /* Unwatched is a placeholder — accept the click but render the row as "(soon)"; no real filter wired yet. */
                            state_.display_settings.mb_library_filter = F::Unwatched;    changed = true; break;
                    case 6: state_.display_settings.mb_library_filter = F::MissingFiles;  changed = true; break;
                    case 7: state_.display_settings.mb_library_filter = F::RecentlyAdded; changed = true; break;
                }
                if (changed) {
                    ::app::SettingsPersistence::save_settings(state_);
                    rebuild_view();
                    start_close_overlay();
                }
                continue;
            }
            // BTN1 / BTN3 are no-ops while overlay is open (prevents
            // tab-jumping the underlying grid).
            if (e.action == platform::InputAction::PREV ||
                e.action == platform::InputAction::NEXT) {
                continue;
            }
            // SETTINGS_MENU is handled by the toggle branch below; let
            // it through.
        }

        // BTN4 (SETTINGS_MENU, black) — toggles the slide-in overlay.
        // Re-pressing closes it. While SlidingIn or SlidingOut, ignore —
        // operator will wait ~200 ms for the animation to settle. Prevents
        // mid-animation state thrash.
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            if (overlay_state_ == OverlayState::Closed) {
                start_open_overlay();
            } else if (overlay_state_ == OverlayState::Open) {
                start_close_overlay();
            }
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

        // BTN2 (PLAY_PAUSE, red) — intercepted globally by the exit modal
        // in main.cpp. It never reaches here; no per-screen handler needed.

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

    // Page-based scroll: snap scroll_row_ to the page boundary that contains
    // the cursor row. Each page is 2 rows (= kVisibleRows in render()).
    if (!view_.empty()) {
        const int cursor_row = grid_cursor_ / kGridCols;
        scroll_row_ = (cursor_row / 2) * 2;
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
    tick_overlay_animation();

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

    // Legacy sort sub-tab strip (Recent / Title / Year / Size) was
    // dropped in v1.6.4 — the slide-in overlay (BTN4) is the canonical
    // place to switch sort + filter now, and the active selection is
    // surfaced by the small "Sort: … · Filter: …" indicator drawn at
    // the bottom-left of the body, just above the chrome footer hints.

    // --- Empty state ---
    if (!loaded_) {
        const std::string msg = "Loading library...";
        const int tw = r.mb_text_width(msg, 18);
        r.mb_draw_text(msg,
                       static_cast<float>((screen_w - tw) / 2),
                       static_cast<float>(screen_h / 2),
                       18, th.dim);
        chrome::draw_footer_hints(r, screen_w, screen_h, {
            {chrome::HintIcon::Btn1Yellow,  "Tab \xE2\x86\x90"},
            {chrome::HintIcon::Btn2Red,     "Exit"},
            {chrome::HintIcon::Btn3Green,   "Tab \xE2\x86\x92"},
            {chrome::HintIcon::Btn4Black,   "Sort+Filter"},
            {chrome::HintIcon::RotaryNav,   "Browse"},
            {chrome::HintIcon::RotaryPress, "Detail"},
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
            {chrome::HintIcon::Btn1Yellow,  "Tab \xE2\x86\x90"},
            {chrome::HintIcon::Btn2Red,     "Exit"},
            {chrome::HintIcon::Btn3Green,   "Tab \xE2\x86\x92"},
            {chrome::HintIcon::Btn4Black,   "Sort+Filter"},
            {chrome::HintIcon::RotaryNav,   "Browse"},
            {chrome::HintIcon::RotaryPress, "Detail"},
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

    // Page-based: scroll_row_ is set in handle_input(). No smooth adjustment.
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
            // fetches, plus year pill + IN LIBRARY badge (or
            // DOWNLOADING / BAD RELEASE badge when the movie is in the
            // queue or stuck on importBlocked).
            const ::ui::Color tint = library_tint_for_tmdb(mv->tmdb_id);
            const bool is_stuck =
                stuck_tmdb_ids_.count(mv->tmdb_id) > 0;
            const bool is_downloading =
                !is_stuck && downloading_tmdb_ids_.count(mv->tmdb_id) > 0;
            chrome::draw_poster_card(
                r, x, y, cell_w, poster_h,
                mv->title, mv->year,
                tint, /*in_library=*/true,
                /*download_pct=*/is_downloading ? 0 : -1,
                /*poster_url=*/mv->poster_url,
                /*is_stuck=*/is_stuck);

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

    // --- Active filter indicator (bottom-left of body, above footer) ---
    // Replaces the legacy sort sub-tab strip — surfaces what's currently
    // in effect from the slide-in overlay so the operator never has to
    // open the panel just to check.
    {
        using S = ::app::AppState::DisplaySettings::MbLibrarySort;
        using F = ::app::AppState::DisplaySettings::MbLibraryFilter;
        const char* sort_label = "Recent";
        switch (state_.display_settings.mb_library_sort) {
            case S::Recent: sort_label = "Recent"; break;
            case S::Title:  sort_label = "Title";  break;
            case S::Year:   sort_label = "Year";   break;
            case S::Size:   sort_label = "Size";   break;
        }
        const char* filter_label = "All";
        switch (state_.display_settings.mb_library_filter) {
            case F::All:           filter_label = "All";              break;
            case F::Unwatched:     filter_label = "Unwatched (soon)"; break;
            case F::MissingFiles:  filter_label = "Missing files";    break;
            case F::RecentlyAdded: filter_label = "Recently added";   break;
        }
        std::string indicator = std::string("Sort: ") + sort_label
                              + "  \xC2\xB7  Filter: " + filter_label;
        // Sit ~24 px above the footer-hint baseline so we don't crash
        // into the icons when long filter labels render. The chrome
        // footer baseline = screen_h - kFrameInset_px - kPad3 = h-64;
        // putting our line at h-64-24 = h-88 keeps a clean gap.
        const int indicator_y = screen_h - chrome::kFrameInset_px
                              - chrome::kPad3 - 24;
        r.mb_draw_text(indicator,
                       static_cast<float>(chrome::kSafeInset_px),
                       static_cast<float>(indicator_y),
                       th.font_small_size, th.dim);
    }

    // --- Footer hints ---
    chrome::draw_footer_hints(r, screen_w, screen_h, {
        {chrome::HintIcon::Btn1Yellow,  "Tab \xE2\x86\x90"},
        {chrome::HintIcon::Btn2Red,     "Exit"},
        {chrome::HintIcon::Btn3Green,   "Tab \xE2\x86\x92"},
        {chrome::HintIcon::Btn4Black,   "Sort+Filter"},
        {chrome::HintIcon::RotaryNav,   "Browse"},
        {chrome::HintIcon::RotaryPress, "Detail"},
    });

    // ============================================================
    // Slide-in overlay (v1.6.x)
    // ============================================================
    if (overlay_state_ != OverlayState::Closed) {
        const auto& th = r.mb_theme();

        const int panel_x = compute_overlay_left_x(
            overlay_state_, overlay_anim_started_at_);
        const float fpanel_x = static_cast<float>(panel_x);
        const float fpanel_y = static_cast<float>(kOverlayPanelTopY);
        const float fpanel_w = static_cast<float>(kOverlayPanelW);
        const float fpanel_h = static_cast<float>(kOverlayPanelH);

        // Panel background — bg_lift, fully opaque.
        r.mb_fill_rect(fpanel_x, fpanel_y, fpanel_w, fpanel_h, th.bg_lift);
        // Top edge: 2 px gold rule.
        r.mb_fill_rect(fpanel_x, fpanel_y, fpanel_w, 2.0f, th.accent);
        // Bottom edge: 2 px gold rule.
        r.mb_fill_rect(fpanel_x, fpanel_y + fpanel_h - 2.0f,
                       fpanel_w, 2.0f, th.accent);
        // Left edge: 2 px gold rule.
        r.mb_fill_rect(fpanel_x, fpanel_y, 2.0f, fpanel_h, th.accent);
        // Right edge: 2 px gold rule (panel sits 20 px in from bezel so it's visible).
        r.mb_fill_rect(fpanel_x + fpanel_w - 2.0f, fpanel_y,
                       2.0f, fpanel_h, th.accent);

        // Panel content x — inset from left rule by panel inner pad.
        const int content_x = panel_x + kOverlayPanelInnerPadX;

        // ---- Stats block ----
        // Gold ZenDots heading: "LIBRARY"
        constexpr int kPanelTitleFontPx = 22;
        const int title_baseline = kOverlayPanelTopY + kOverlayPanelInnerPadY +
                                   kPanelTitleFontPx - 2;
        r.mb_draw_title_text("LIBRARY",
                             static_cast<float>(content_x),
                             static_cast<float>(title_baseline),
                             kPanelTitleFontPx, th.accent);

        // Three stat lines in cream body font, 14 px each.
        constexpr int kStatFontPx = 14;
        const int stat_y0 = title_baseline + 24;
        char buf_titles[64], buf_used[64], buf_free[64];
        std::snprintf(buf_titles, sizeof(buf_titles), "%zu titles",
                      library_.size());
        // Reuse the format_bytes helper already in library_screen.cpp.
        int64_t used_bytes_ov = 0;
        for (const Movie& m : library_) {
            if (m.has_file) used_bytes_ov += m.file_size_bytes;
        }
        std::snprintf(buf_used, sizeof(buf_used), "%s used",
                      format_bytes(used_bytes_ov).c_str());
        int64_t free_bytes_ov = 0;
        {
            std::error_code ec;
            auto info = std::filesystem::space("/mnt/ssd/library", ec);
            if (!ec) free_bytes_ov = static_cast<int64_t>(info.available);
        }
        std::snprintf(buf_free, sizeof(buf_free), "%s free",
                      free_bytes_ov > 0 ? format_bytes(free_bytes_ov).c_str() : "—");

        r.mb_draw_text(buf_titles,
                       static_cast<float>(content_x),
                       static_cast<float>(stat_y0),
                       kStatFontPx, th.fg);
        r.mb_draw_text(buf_used,
                       static_cast<float>(content_x),
                       static_cast<float>(stat_y0 + 18),
                       kStatFontPx, th.fg);
        r.mb_draw_text(buf_free,
                       static_cast<float>(content_x),
                       static_cast<float>(stat_y0 + 36),
                       kStatFontPx, th.fg);

        // 1 px dim divider under stats.
        const int divider1_y = stat_y0 + 56;
        r.mb_draw_line(static_cast<float>(content_x),
                       static_cast<float>(divider1_y),
                       static_cast<float>(panel_x + kOverlayPanelW - kOverlayPanelInnerPadX),
                       static_cast<float>(divider1_y),
                       1.0f, th.dim, 0.5f);

        // ---- Sort by section ----
        constexpr int kSectionHeadingFontPx = 14;
        constexpr int kRowFontPx = 16;
        const int sort_heading_y = divider1_y + kOverlaySectionGap;
        r.mb_draw_title_text("SORT BY",
                             static_cast<float>(content_x),
                             static_cast<float>(sort_heading_y),
                             kSectionHeadingFontPx, th.accent);

        struct Row { const char* label; bool is_active; bool is_focused; };
        using S = ::app::AppState::DisplaySettings::MbLibrarySort;
        const S active_sort = state_.display_settings.mb_library_sort;
        const Row sort_rows[4] = {
            {"Recent", active_sort == S::Recent, overlay_focus_row_ == 0},
            {"Title",  active_sort == S::Title,  overlay_focus_row_ == 1},
            {"Year",   active_sort == S::Year,   overlay_focus_row_ == 2},
            {"Size",   active_sort == S::Size,   overlay_focus_row_ == 3},
        };

        const int sort_rows_y0 = sort_heading_y + kOverlaySectionHeaderH;
        for (int i = 0; i < 4; ++i) {
            const int row_y = sort_rows_y0 + i * kOverlayRowHeight;
            const ::ui::Color label_color =
                (sort_rows[i].is_active || sort_rows[i].is_focused)
                    ? th.accent : th.dim;
            // Focused row gets a blinking gold ▶ marker 18 px to the
            // left of the label x. Reuses the same draw-cursor-marker
            // pattern from MbSettingsScreen (right-pointing variant).
            if (sort_rows[i].is_focused) {
                const float marker_x = static_cast<float>(content_x);
                const float marker_cy = static_cast<float>(row_y + kOverlayRowHeight / 2);
                constexpr float kMarkerHalfH = 6.0f;
                constexpr float kMarkerW = 7.2f;
                r.mb_fill_triangle(
                    marker_x,           marker_cy - kMarkerHalfH,
                    marker_x,           marker_cy + kMarkerHalfH,
                    marker_x + kMarkerW, marker_cy,
                    th.accent, 1.0f);
            }
            r.mb_draw_text(sort_rows[i].label,
                           static_cast<float>(content_x + 18),
                           static_cast<float>(row_y + kOverlayRowHeight - 10),
                           kRowFontPx, label_color);
        }

        // 1 px divider under sort.
        const int divider2_y = sort_rows_y0 + 4 * kOverlayRowHeight + 6;
        r.mb_draw_line(static_cast<float>(content_x),
                       static_cast<float>(divider2_y),
                       static_cast<float>(panel_x + kOverlayPanelW - kOverlayPanelInnerPadX),
                       static_cast<float>(divider2_y),
                       1.0f, th.dim, 0.5f);

        // ---- Filter section ----
        const int filter_heading_y = divider2_y + kOverlaySectionGap;
        r.mb_draw_title_text("FILTER",
                             static_cast<float>(content_x),
                             static_cast<float>(filter_heading_y),
                             kSectionHeadingFontPx, th.accent);

        using F = ::app::AppState::DisplaySettings::MbLibraryFilter;
        const F active_filter = state_.display_settings.mb_library_filter;
        struct FilterRow { const char* label; bool is_active; bool is_focused; bool is_soon; };
        const FilterRow filter_rows[4] = {
            {"All",            active_filter == F::All,            overlay_focus_row_ == 4, false},
            {"Unwatched",      active_filter == F::Unwatched,      overlay_focus_row_ == 5, true},
            {"Missing files",  active_filter == F::MissingFiles,   overlay_focus_row_ == 6, false},
            {"Recently added", active_filter == F::RecentlyAdded,  overlay_focus_row_ == 7, false},
        };
        const int filter_rows_y0 = filter_heading_y + kOverlaySectionHeaderH;
        for (int i = 0; i < 4; ++i) {
            const int row_y = filter_rows_y0 + i * kOverlayRowHeight;
            const ::ui::Color label_color =
                filter_rows[i].is_soon ? th.dim
              : (filter_rows[i].is_active || filter_rows[i].is_focused) ? th.accent
              : th.dim;
            if (filter_rows[i].is_focused) {
                const float marker_x = static_cast<float>(content_x);
                const float marker_cy = static_cast<float>(row_y + kOverlayRowHeight / 2);
                constexpr float kMarkerHalfH = 6.0f;
                constexpr float kMarkerW = 7.2f;
                r.mb_fill_triangle(
                    marker_x,           marker_cy - kMarkerHalfH,
                    marker_x,           marker_cy + kMarkerHalfH,
                    marker_x + kMarkerW, marker_cy,
                    th.accent, 1.0f);
            }
            std::string label = filter_rows[i].label;
            if (filter_rows[i].is_soon) label += "  (soon)";
            r.mb_draw_text(label,
                           static_cast<float>(content_x + 18),
                           static_cast<float>(row_y + kOverlayRowHeight - 10),
                           kRowFontPx, label_color);
        }

        // ---- Footer hint inside the panel ----
        // Match the chrome footer's icon vocabulary so the operator
        // sees the same visual language inside the overlay. Six-input
        // row, but constrained to the panel's content_x so it never
        // bleeds outside the gold-framed area.
        const int hint_y = kOverlayPanelBottomY - kOverlayPanelInnerPadY;
        chrome::draw_hint_row(r, content_x, hint_y, {
            {chrome::HintIcon::Btn1Yellow,  "\xE2\x80\x94"},
            {chrome::HintIcon::Btn2Red,     "Close"},
            {chrome::HintIcon::Btn3Green,   "\xE2\x80\x94"},
            {chrome::HintIcon::Btn4Black,   "Close"},
            {chrome::HintIcon::RotaryNav,   "Rows"},
            {chrome::HintIcon::RotaryPress, "Apply"},
        });
    }
}

}  // namespace media_browser::ui
