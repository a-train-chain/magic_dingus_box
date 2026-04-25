#include "media_browser/ui/mb_settings_screen.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

#include <curl/curl.h>
#include <json/json.h>

#include "media_browser/radarr/radarr_client.h"
#include "ui/renderer.h"
#include "ui/theme.h"

namespace media_browser::ui {

namespace {

// --- Layout constants (pixels) -----------------------------------------
//
// Retro home-menu / DetailScreen-aligned layout. The design language is
// "border + text on bg" (no filled selection bars), with a steel-blue
// header rule, gold blinking ◂ cursor on the focused row, and steel-blue
// section dividers between logical row groups. See detail_screen.cpp's
// render() for the canonical reference — this file matches those metrics
// pixel-for-pixel so all three media-browser screens (Browse, Detail,
// Settings) share the same chrome.

constexpr float kPaddingX        = 32.0f;   // matches detail_screen.cpp

// Top "MOVIES • SETTINGS" header strip.
constexpr float kHeaderBaselineY = 38.0f;   // baseline of header text
constexpr float kHeaderRuleY     = 58.0f;   // 2px steel-blue rule

// Settings list — top of the first row baseline, vertical spacing per row.
constexpr float kListTopY        = 84.0f;   // first row label baseline lives here
constexpr float kRowHeight       = 44.0f;   // standard row pitch (label + value)
constexpr float kRowGap          = 6.0f;    // small breathing gap between rows
constexpr float kIndexerRowHeight = 26.0f;  // indexer sub-list row pitch
                                            // (referenced by build_rows())
constexpr int   kIndexerMaxVisible = 5;

// Cursor marker (◂) — drawn left of the focused row's label.
constexpr float kCursorMarkerOffsetX = 18.0f;  // distance from label x to triangle tip

// Slider track geometry (used by min seeders / low-space / max concurrent).
// Track sits below the label/value text, spans the right half of the row.
constexpr float kSliderTrackH    = 4.0f;
constexpr float kSliderTrackW    = 220.0f;
constexpr float kSliderInsetY    = 10.0f;   // distance below baseline

// Service status dot.
constexpr float kStatusDotSize   = 12.0f;
constexpr float kStatusDotGap    = 8.0f;    // gap between dot and label
constexpr float kStatusGroupGap  = 22.0f;   // gap between dot/label groups

// Indexer checkbox.
constexpr float kCheckboxSize    = 16.0f;

// Action button (Retry / Pause / Resume / Hide). Outlined-only, no fill —
// matches DetailScreen's action row idiom.
constexpr float kActionButtonH   = 38.0f;
constexpr float kActionOutlineW  = 2.0f;

// How long the placeholder banner stays on screen.
constexpr std::chrono::milliseconds kBannerMs{2500};

// Bottom hint footer offset from screen bottom.
constexpr float kFooterPadY      = 12.0f;

size_t curl_write_cb(void* ptr, size_t size, size_t nmemb, void* ud) {
    auto* s = static_cast<std::string*>(ud);
    s->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

// Read an env var, returning an empty string if unset.
std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

// Truncate `text` with a trailing ellipsis if it exceeds max_w at font_size.
// Mirrors the helper in detail_screen.cpp so long storage paths and indexer
// names don't bleed past the right edge of the value column.
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

// Format raw byte count as "124 GB" (no decimal). Used by the Storage row's
// "FREE: 124 GB" right-aligned readout — the space-saving short form is more
// scannable than the long "12.3 GB free / 500 GB" we used in the prototype.
std::string format_gb_short(int64_t bytes) {
    if (bytes <= 0) return "0 GB";
    double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    char buf[32];
    if (gb >= 100.0) {
        std::snprintf(buf, sizeof(buf), "%.0f GB", gb);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f GB", gb);
    }
    return std::string(buf);
}

// Draw the gold blinking ◂ cursor marker at `(tip_x, center_y)`. The triangle
// points LEFT (toward the label edge), same orientation as the home-menu
// playlist cursor and the DetailScreen action-button cursor.
void draw_cursor_marker(::ui::Renderer& r,
                        float tip_x, float center_y,
                        ::ui::Color color, float alpha) {
    constexpr float kMarkerSize = 8.0f;  // half-height of the triangle
    r.mb_fill_triangle(
        tip_x + kMarkerSize * 1.2f, center_y - kMarkerSize,  // top-right
        tip_x + kMarkerSize * 1.2f, center_y + kMarkerSize,  // bottom-right
        tip_x,                       center_y,               // pointing tip (left)
        color, alpha);
}

}  // namespace

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

bool MbSettingsScreen::ping_http(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    // Consider the service reachable if we got ANY response — qBittorrent
    // returns 403/401 on unauthenticated /api/v2/app/version which still
    // proves the daemon is listening.
    return (rc == CURLE_OK) && (http_code > 0);
}

std::string MbSettingsScreen::http_get(const std::string& url,
                                       const std::string& header) {
    CURL* curl = curl_easy_init();
    if (!curl) return {};
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    struct curl_slist* headers = nullptr;
    if (!header.empty()) {
        headers = curl_slist_append(headers, header.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || http_code >= 400) return {};
    return body;
}

std::string MbSettingsScreen::format_free_space(int64_t free_bytes,
                                                int64_t total_bytes) {
    auto fmt = [](int64_t b) {
        double gb = static_cast<double>(b) / (1024.0 * 1024.0 * 1024.0);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f GB", gb);
        return std::string(buf);
    };
    if (total_bytes <= 0) return fmt(free_bytes) + " free";
    return fmt(free_bytes) + " free / " + fmt(total_bytes);
}

// ---------------------------------------------------------------------------
// Construction / lifecycle
// ---------------------------------------------------------------------------

MbSettingsScreen::MbSettingsScreen(RadarrClient& radarr,
                                   std::function<void()> on_hide_feature)
    : radarr_(radarr), on_hide_feature_(std::move(on_hide_feature)) {
    // Tag availability of the Prowlarr API key up front — used by the
    // Indexer row to show a "not configured" fallback message.
    prowlarr_api_key_available_ = !env_or_empty("MDB_PROWLARR_API_KEY").empty();
}

void MbSettingsScreen::enter() {
    if (!loaded_once_) {
        refresh_service_health();
        refresh_quality_profiles();
        refresh_root_folders();
        refresh_indexers();
        loaded_once_ = true;
    } else {
        // Cheap refresh on re-entry: only re-ping services. Full profile /
        // root-folder / indexer lists don't change often on a kiosk, so we
        // keep the cached copies.
        refresh_service_health();
    }
    build_rows();
}

void MbSettingsScreen::build_rows() {
    rows_.clear();

    auto add = [&](RowKind k, std::string label, float h = kRowHeight) {
        rows_.push_back({k, std::move(label), h});
    };

    add(RowKind::ServiceStatus,        "Services");
    add(RowKind::QualityProfile,       "Quality profile");
    add(RowKind::MinSeeders,           "Minimum seeders");
    add(RowKind::StoragePath,          "Storage");
    add(RowKind::LowSpaceThresholdGb,  "Low-space pause threshold (GB)");
    add(RowKind::MaxConcurrentDownloads, "Max concurrent downloads");

    // Indexer row expands to fit up to kIndexerMaxVisible rows of sub-list.
    int visible_indexers = std::min<int>(static_cast<int>(indexers_.size()),
                                         kIndexerMaxVisible);
    if (visible_indexers <= 0) visible_indexers = 1;  // room for "not configured"
    float indexer_height = kRowHeight
        + static_cast<float>(visible_indexers) * kIndexerRowHeight;
    add(RowKind::IndexerToggles,       "Indexers", indexer_height);

    add(RowKind::RetryAllFailed,       "Retry all failed downloads");
    add(RowKind::PauseAllDownloads,    "Pause all downloads");
    add(RowKind::ResumeAllDownloads,   "Resume all downloads");
    add(RowKind::HideMovies,           "Hide Movies feature");
    add(RowKind::AdvancedUrlHint,      "Advanced", 36.0f);

    cursor_ = std::clamp(cursor_, 0, static_cast<int>(rows_.size()) - 1);
}

// ---------------------------------------------------------------------------
// Network refresh helpers
// ---------------------------------------------------------------------------

void MbSettingsScreen::refresh_service_health() {
    // Radarr — delegate to the client (uses the correct API key header).
    health_.radarr = radarr_.is_reachable();
    // Prowlarr — plain HTTP GET; the /ping endpoint is unauthenticated.
    health_.prowlarr = ping_http("http://localhost:9696/ping");
    // qBittorrent — the WebUI version endpoint returns 401 without a
    // session, but any HTTP response proves the daemon is up.
    health_.qbittorrent = ping_http("http://localhost:8080/api/v2/app/version");
    health_.fetched_at = std::chrono::steady_clock::now();
}

void MbSettingsScreen::refresh_quality_profiles() {
    quality_profiles_ = radarr_.get_quality_profiles();
    quality_profile_idx_ = std::clamp(
        quality_profile_idx_, 0,
        std::max(0, static_cast<int>(quality_profiles_.size()) - 1));
}

void MbSettingsScreen::refresh_root_folders() {
    root_folders_ = radarr_.get_root_folders();
}

void MbSettingsScreen::refresh_indexers() {
    indexers_.clear();
    if (!prowlarr_api_key_available_) return;
    const std::string api_key = env_or_empty("MDB_PROWLARR_API_KEY");
    const std::string header  = "X-Api-Key: " + api_key;
    const std::string body    = http_get(
        "http://localhost:9696/api/v1/indexer", header);
    if (body.empty()) return;

    // Parse with jsoncpp (same pattern as radarr_parsers.cpp). The Prowlarr
    // /api/v1/indexer shape is an array of indexer objects; we only need
    // `id`, `name`, and `enable` (with `enabled` as a fallback on older
    // Prowlarr builds).
    Json::CharReaderBuilder rb;
    Json::Value root;
    std::string err;
    std::istringstream is(body);
    if (!Json::parseFromStream(rb, is, &root, &err) || !root.isArray()) {
        return;
    }
    for (const auto& r : root) {
        if (!r.isObject()) continue;
        ProwlarrIndexer idx;
        idx.id = r.get("id", 0).asInt();
        idx.name = r.get("name", "").asString();
        if (r.isMember("enable")) {
            idx.enabled = r.get("enable", false).asBool();
        } else if (r.isMember("enabled")) {
            idx.enabled = r.get("enabled", false).asBool();
        }
        if (!idx.name.empty()) indexers_.push_back(std::move(idx));
    }
}

// ---------------------------------------------------------------------------
// Banners
// ---------------------------------------------------------------------------

void MbSettingsScreen::show_banner(const std::string& msg) {
    banner_text_ = msg;
    banner_until_ = std::chrono::steady_clock::now() + kBannerMs;
}

// ---------------------------------------------------------------------------
// Hide-feature callback
// ---------------------------------------------------------------------------

void MbSettingsScreen::trigger_hide_feature() {
    if (on_hide_feature_) on_hide_feature_();
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

Screen MbSettingsScreen::handle_input(
        const std::vector<platform::InputEvent>& events) {
    for (const auto& e : events) {
        // Menu button always returns to Browse (spec: "SETTINGS_MENU/BTN4
        // returns to Screen::Browse" — matches LibraryScreen / QueueScreen).
        if (e.action == platform::InputAction::SETTINGS_MENU && e.pressed) {
            return Screen::Browse;
        }

        // BTN2 (PLAY_PAUSE): global "refresh service health" shortcut.
        // Cheap — pings the three services and updates the status dots.
        // Exposes a transient banner so the user sees what happened.
        if (e.action == platform::InputAction::PLAY_PAUSE && e.pressed) {
            refresh_service_health();
            std::string msg = "Services: ";
            msg += health_.radarr ? "Radarr ok" : "Radarr down";
            msg += "  |  ";
            msg += health_.prowlarr ? "Prowlarr ok" : "Prowlarr down";
            msg += "  |  ";
            msg += health_.qbittorrent ? "qBittorrent ok" : "qBittorrent down";
            show_banner(msg);
            continue;
        }

        // Vertical movement: move cursor between rows.
        if (e.action == platform::InputAction::ROTATE_VERTICAL) {
            int delta = e.delta;
            if (delta == 0) continue;
            int n = static_cast<int>(rows_.size());
            if (n == 0) continue;

            // If focused on the IndexerToggles row, vertical movement
            // walks the embedded sub-list first; only leaves the row when
            // we run off its ends.
            if (rows_[cursor_].kind == RowKind::IndexerToggles &&
                prowlarr_api_key_available_ &&
                !indexers_.empty()) {
                int new_ic = indexer_cursor_ + delta;
                if (new_ic >= 0 &&
                    new_ic < static_cast<int>(indexers_.size()) &&
                    new_ic < kIndexerMaxVisible) {
                    indexer_cursor_ = new_ic;
                    continue;
                }
                // else: fall through to row-level movement
            }

            cursor_ = std::clamp(cursor_ + delta, 0, n - 1);

            // Skip any non-focusable row (currently only AdvancedUrlHint,
            // which is read-only fine print). We step in the direction we
            // were already moving — if the caller walked DOWN onto a
            // non-focusable row we keep walking DOWN; if we hit the end
            // we reverse. This keeps the cursor off non-focusable rows
            // even if someone later inserts another row after them.
            {
                int step = (delta > 0) ? +1 : -1;
                // Bound the scan to n iterations so we can't infinite-loop
                // if every row is marked non-focusable (shouldn't happen,
                // but defensive).
                int guard = n;
                while (guard-- > 0 &&
                       rows_[cursor_].kind == RowKind::AdvancedUrlHint) {
                    int candidate = cursor_ + step;
                    if (candidate < 0 || candidate >= n) {
                        // Fell off the end — reverse direction and keep
                        // scanning for the nearest focusable row.
                        step = -step;
                        candidate = cursor_ + step;
                        if (candidate < 0 || candidate >= n) break;
                    }
                    cursor_ = candidate;
                }
            }

            // Landing on the indexer row: reset sub-cursor.
            if (rows_[cursor_].kind == RowKind::IndexerToggles) {
                indexer_cursor_ = std::clamp(
                    indexer_cursor_, 0,
                    std::max(0, static_cast<int>(indexers_.size()) - 1));
            }
            continue;
        }

        // Horizontal movement: adjust the focused control.
        if (e.action == platform::InputAction::ROTATE) {
            int delta = e.delta;
            if (delta == 0) continue;
            switch (rows_[cursor_].kind) {
                case RowKind::QualityProfile: {
                    if (quality_profiles_.empty()) break;
                    int n = static_cast<int>(quality_profiles_.size());
                    quality_profile_idx_ =
                        (quality_profile_idx_ + delta % n + n) % n;
                    break;
                }
                case RowKind::MinSeeders:
                    prefs_.min_seeders = std::clamp(
                        prefs_.min_seeders + delta, 0, 20);
                    break;
                case RowKind::LowSpaceThresholdGb:
                    prefs_.low_space_threshold_gb = std::clamp(
                        prefs_.low_space_threshold_gb + delta, 10, 200);
                    break;
                case RowKind::MaxConcurrentDownloads:
                    prefs_.max_concurrent_downloads = std::clamp(
                        prefs_.max_concurrent_downloads + delta, 1, 5);
                    break;
                default:
                    break;
            }
            continue;
        }

        // SELECT: toggle checkbox / fire button.
        if (e.action == platform::InputAction::SELECT && e.pressed) {
            switch (rows_[cursor_].kind) {
                case RowKind::ServiceStatus:
                    // SELECT on the services row re-checks them.
                    refresh_service_health();
                    break;
                case RowKind::IndexerToggles:
                    if (!prowlarr_api_key_available_ || indexers_.empty()) {
                        show_banner(
                            "Prowlarr API key not configured — "
                            "set MDB_PROWLARR_API_KEY");
                    } else if (indexer_cursor_ >= 0 &&
                               indexer_cursor_ < static_cast<int>(
                                   indexers_.size())) {
                        // Optimistic local toggle. A real implementation
                        // would PUT the updated indexer back to Prowlarr;
                        // the spec explicitly lets us show "not implemented"
                        // here, so we flip the local flag and note it.
                        indexers_[indexer_cursor_].enabled =
                            !indexers_[indexer_cursor_].enabled;
                        show_banner(
                            "Indexer toggle is local-only in MVP — "
                            "use Prowlarr web UI to persist");
                    }
                    break;
                case RowKind::RetryAllFailed:
                    show_banner("Not implemented in MVP — use Radarr web UI");
                    break;
                case RowKind::PauseAllDownloads:
                case RowKind::ResumeAllDownloads:
                    show_banner("Use qBittorrent web UI (localhost:8080)");
                    break;
                case RowKind::HideMovies:
                    trigger_hide_feature();
                    return Screen::Exit;
                default:
                    break;
            }
            continue;
        }
    }
    return Screen::MovieSettings;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void MbSettingsScreen::render(::ui::Renderer& r, int screen_w, int screen_h) {
    const ::ui::Theme& th = r.mb_theme();
    r.mb_fill_background();

    const float w = static_cast<float>(screen_w);
    const float h = static_cast<float>(screen_h);

    // 500ms blink cycle keyed off epoch time so the focused-row ◂ marker
    // breathes in lockstep with the home-menu playlist cursor and the
    // DetailScreen action-button cursor — when the user transitions
    // between screens the blinks stay phase-aligned and feel like one UI.
    auto epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    const bool blink_on = (epoch_ms / 500) % 2 == 0;

    // --- Top header strip: "MOVIES • SETTINGS" + back hint -----------
    // Mirrors detail_screen.cpp's "FEATURE PRESENTATION" header: a Zen
    // Dots heading in steel-blue (accent2), a small dim "BTN4: back" hint
    // on the right, and a full-width 2px steel-blue rule at y=58 that
    // visually frames the screen the same way the home menu's title rule
    // does. Both screens share these exact metrics so the chrome doesn't
    // jump between transitions.
    {
        const std::string heading = "MOVIES \xE2\x80\xA2 SETTINGS";  // U+2022 bullet
        int hd_size = th.font_heading_size;
        r.mb_draw_title_text(heading, kPaddingX, kHeaderBaselineY,
                             hd_size, th.accent2, 1.0f);

        // Right-side hint. Body font (not Zen Dots) so it reads as a
        // status sub-label rather than competing with the heading.
        const std::string back_hint = "BTN4: back   BTN2: refresh";
        int hint_size = th.font_small_size;
        int hw = r.mb_text_width(back_hint, hint_size);
        float hx = w - kPaddingX - static_cast<float>(hw);
        float hy = kHeaderBaselineY + 2.0f;  // nudge ~2px below heading baseline
        r.mb_draw_text(back_hint, hx, hy, hint_size, th.dim, 0.9f);

        // Full-width 2px steel-blue rule beneath the header — matches the
        // home menu's title underline pattern but stretched edge-to-edge
        // so it reads as a screen frame.
        r.mb_draw_line(kPaddingX, kHeaderRuleY,
                       w - kPaddingX, kHeaderRuleY,
                       2.0f, th.accent2, 0.95f);
    }

    // --- Scroll window -----------------------------------------------
    // List sits between the header rule and the bottom hint footer. We
    // reserve ~26px at the bottom for the centered hint line.
    float list_top    = kListTopY;
    float list_bottom = h - 32.0f;          // leaves room for footer hint
    float list_h      = list_bottom - list_top;

    // Compute the y-position of each row in the logical (un-scrolled)
    // layout so we can drive scroll_y_ from the focused row.
    std::vector<float> row_y;
    row_y.reserve(rows_.size());
    {
        float y = 0.0f;
        for (const auto& row : rows_) {
            row_y.push_back(y);
            y += row.height + kRowGap;
        }
    }

    // Auto-scroll to keep cursor visible. Same pattern as before — only
    // the chrome around the rows changed, not the scroll math.
    if (!rows_.empty()) {
        float c_top = row_y[cursor_];
        float c_bot = c_top + rows_[cursor_].height;
        if (c_top < scroll_y_) scroll_y_ = c_top;
        if (c_bot > scroll_y_ + list_h) {
            scroll_y_ = c_bot - list_h;
        }
        if (scroll_y_ < 0.0f) scroll_y_ = 0.0f;
    }

    // --- Loading / health-fetch indicator ----------------------------
    // If the service ping hasn't completed yet (fetched_at unset means
    // a brand-new screen), show "checking services..." near the header
    // so the user understands the dots are still resolving. Drawn in
    // accent (gold) on the right side, mirroring DetailScreen's banner
    // accent treatment but as inline status rather than a modal box.
    {
        bool services_unfetched =
            health_.fetched_at.time_since_epoch().count() == 0;
        if (services_unfetched) {
            const std::string msg = "checking services...";
            int sz = th.font_small_size;
            int tw = r.mb_text_width(msg, sz);
            float mx = w - kPaddingX - static_cast<float>(tw);
            float my = kHeaderRuleY + 18.0f
                     + static_cast<float>(r.mb_text_baseline(sz));
            r.mb_draw_text(msg, mx, my, sz, th.accent, 0.95f);
        }
    }

    // --- Row geometry -------------------------------------------------
    // Borderless rows: just text on bg. Focused row gets a gold ◂ marker
    // 18px to the LEFT of the label x, plus a brighter (gold) label.
    // No filled selection bar — matches the DetailScreen aesthetic.
    const float row_x   = kPaddingX;
    const float row_w   = w - 2.0f * kPaddingX;
    const int label_sz  = th.font_medium_size;
    const int value_sz  = th.font_medium_size;
    const int small_sz  = th.font_small_size;
    const int baseline  = r.mb_text_baseline(label_sz);

    // Section divider helper. Steel-blue at alpha 0.6, full-width except
    // the destructive divider above HideMovies which uses red/orange to
    // visually segregate the danger zone (matches the highlight2 idiom
    // DetailScreen uses for "Confirm Remove" buttons).
    auto draw_section_divider = [&](float y, ::ui::Color color, float alpha) {
        r.mb_draw_line(row_x, y, row_x + row_w, y, 2.0f, color, alpha);
    };

    for (size_t i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_[i];
        float y_top = list_top + row_y[i] - scroll_y_;
        float y_bot = y_top + row.height;
        if (y_bot < list_top || y_top > list_bottom) continue;  // culled

        bool focused = (static_cast<int>(i) == cursor_);

        // Label x is the same for every row; value column right-edge is
        // at row_x + row_w. We keep the label_x flush left (no inset)
        // because there's no row fill to inset against.
        float label_x = row_x;
        float center_y = y_top + (kRowHeight / 2.0f);
        float label_y = center_y - (label_sz / 2.0f)
                      + static_cast<float>(baseline);
        float value_x_right = row_x + row_w;

        // Focused-row decoration: blinking gold ◂ marker 18px left of
        // the label baseline-vertical-center. We skip the marker for
        // AdvancedUrlHint (non-focusable fine print) and for the
        // HideMovies row (which uses its own red/orange marker variant
        // to underline the destructive nature).
        ::ui::Color label_color = focused ? th.accent : th.fg;
        if (focused && blink_on && row.kind != RowKind::AdvancedUrlHint
            && row.kind != RowKind::HideMovies) {
            draw_cursor_marker(r,
                               label_x - kCursorMarkerOffsetX, center_y,
                               th.accent, 1.0f);
        }
        if (row.kind != RowKind::AdvancedUrlHint) {
            r.mb_draw_text(row.label, label_x, label_y, label_sz,
                           label_color, focused ? 1.0f : 0.9f);
        }

        // Per-row value column rendering.
        switch (row.kind) {
            case RowKind::ServiceStatus: {
                // Three labeled service dots, drawn right-to-left so the
                // qBittorrent group sits at the right edge. If we don't
                // yet have a fetch result, dots render in `dim` to signal
                // "unknown" rather than implying "all services offline".
                bool unfetched =
                    health_.fetched_at.time_since_epoch().count() == 0;
                struct Entry { const char* name; bool up; };
                Entry entries[3] = {
                    {"qBittorrent", health_.qbittorrent},
                    {"Prowlarr",    health_.prowlarr},
                    {"Radarr",      health_.radarr},
                };
                float cx = value_x_right;
                for (const auto& e : entries) {
                    int tw = r.mb_text_width(e.name, small_sz);
                    // Layout (right-to-left): [name text] [gap] [dot]
                    float dot_x = cx - kStatusDotSize;
                    float dot_y = center_y - (kStatusDotSize / 2.0f);
                    ::ui::Color dot = unfetched
                        ? th.dim
                        : (e.up ? th.highlight1 : th.highlight2);
                    r.mb_fill_rect(dot_x, dot_y,
                                   kStatusDotSize, kStatusDotSize,
                                   dot, 1.0f);
                    cx = dot_x - kStatusDotGap;
                    float tx = cx - static_cast<float>(tw);
                    int sb = r.mb_text_baseline(small_sz);
                    float ty = center_y - (small_sz / 2.0f)
                             + static_cast<float>(sb);
                    r.mb_draw_text(e.name, tx, ty, small_sz,
                                   th.dim, 0.95f);
                    cx = tx - kStatusGroupGap;
                }
                break;
            }

            case RowKind::QualityProfile: {
                // Cycle control: name in gold (accent), framed by ◀ ▶
                // hint glyphs in dim small font when focused. The arrows
                // are pure text — they aren't focus markers themselves,
                // they signal "ROTATE to change" affordance.
                std::string val;
                if (quality_profiles_.empty()) {
                    val = "(Radarr not reachable)";
                } else {
                    val = quality_profiles_[quality_profile_idx_].name;
                }
                ::ui::Color val_color = quality_profiles_.empty()
                    ? th.dim : th.accent;

                float vx_right = value_x_right;
                if (focused && !quality_profiles_.empty()) {
                    // Right "▶" hint
                    const std::string r_arrow = "\xE2\x96\xB6";  // U+25B6
                    int rw = r.mb_text_width(r_arrow, small_sz);
                    int sb = r.mb_text_baseline(small_sz);
                    float ry = center_y - (small_sz / 2.0f)
                             + static_cast<float>(sb);
                    r.mb_draw_text(r_arrow,
                                   vx_right - static_cast<float>(rw),
                                   ry, small_sz, th.dim, 0.85f);
                    vx_right -= static_cast<float>(rw) + 8.0f;
                }
                int tw = r.mb_text_width(val, value_sz);
                float tx = vx_right - static_cast<float>(tw);
                r.mb_draw_text(val, tx, label_y, value_sz,
                               val_color, focused ? 1.0f : 0.9f);
                if (focused && !quality_profiles_.empty()) {
                    // Left "◀" hint, just to the left of the value text.
                    const std::string l_arrow = "\xE2\x97\x80";  // U+25C0
                    int lw = r.mb_text_width(l_arrow, small_sz);
                    int sb = r.mb_text_baseline(small_sz);
                    float ly = center_y - (small_sz / 2.0f)
                             + static_cast<float>(sb);
                    r.mb_draw_text(l_arrow,
                                   tx - 8.0f - static_cast<float>(lw),
                                   ly, small_sz, th.dim, 0.85f);
                }
                break;
            }

            case RowKind::MinSeeders: {
                // Slider row: label on left, "<value>  (range)" on right,
                // 2px gold-outlined track below with a gold-fill rect
                // proportional to (value - min)/(max - min). The track
                // sits in the right half of the row to avoid colliding
                // with the label.
                int v = prefs_.min_seeders, mn = 0, mx = 20;
                std::ostringstream os;
                os << v << "   (" << mn << "-" << mx << ")";
                std::string val = os.str();
                int tw = r.mb_text_width(val, value_sz);
                r.mb_draw_text(val,
                               value_x_right - static_cast<float>(tw),
                               label_y, value_sz,
                               focused ? th.accent : th.fg,
                               focused ? 1.0f : 0.9f);
                // Slider track + fill.
                float track_x = value_x_right - kSliderTrackW;
                float track_y = label_y + kSliderInsetY;
                r.mb_stroke_rect(track_x, track_y,
                                 kSliderTrackW, kSliderTrackH,
                                 2.0f, th.accent,
                                 focused ? 1.0f : 0.7f);
                float frac = (mx > mn)
                    ? static_cast<float>(v - mn) / static_cast<float>(mx - mn)
                    : 0.0f;
                r.mb_fill_rect(track_x, track_y,
                               kSliderTrackW * frac, kSliderTrackH,
                               th.accent, focused ? 1.0f : 0.7f);
                break;
            }

            case RowKind::StoragePath: {
                // Two-line presentation: path on the right side of the
                // label row in dim small font (truncated if it overflows),
                // and a "FREE: 124 GB" readout right-aligned in highlight1
                // (green) or highlight2 (red/orange) when below the
                // user's low-space threshold.
                if (root_folders_.empty()) {
                    const std::string msg = "(no root folders)";
                    int tw = r.mb_text_width(msg, value_sz);
                    r.mb_draw_text(msg,
                                   value_x_right - static_cast<float>(tw),
                                   label_y, value_sz, th.dim, 0.85f);
                    break;
                }
                const auto& rf = root_folders_.front();
                std::string free_str = "FREE: " +
                    format_gb_short(rf.free_space_bytes);
                // Color the FREE readout red when free GB drops below
                // the user's low-space threshold — same green/red logic
                // the dots use for service health.
                int64_t threshold_bytes =
                    static_cast<int64_t>(prefs_.low_space_threshold_gb)
                    * 1024LL * 1024LL * 1024LL;
                ::ui::Color free_color =
                    (rf.free_space_bytes < threshold_bytes)
                        ? th.highlight2 : th.highlight1;
                int free_tw = r.mb_text_width(free_str, value_sz);
                r.mb_draw_text(free_str,
                               value_x_right - static_cast<float>(free_tw),
                               label_y, value_sz, free_color, 1.0f);
                // Path under the label, dim small font, truncated to
                // available width left of the FREE readout.
                float path_max_w =
                    value_x_right - static_cast<float>(free_tw) - 16.0f
                    - (label_x + r.mb_text_width(row.label, label_sz));
                if (path_max_w < 80.0f) path_max_w = 80.0f;
                std::string path_drawn = truncate_to_width(
                    r, rf.path, small_sz, path_max_w);
                int sb = r.mb_text_baseline(small_sz);
                float py = center_y + 12.0f + static_cast<float>(sb);
                r.mb_draw_text(path_drawn, label_x, py,
                               small_sz, th.dim, 0.85f);
                break;
            }

            case RowKind::LowSpaceThresholdGb: {
                int v = prefs_.low_space_threshold_gb, mn = 10, mx = 200;
                std::ostringstream os;
                os << v << " GB   (" << mn << "-" << mx << ")";
                std::string val = os.str();
                int tw = r.mb_text_width(val, value_sz);
                r.mb_draw_text(val,
                               value_x_right - static_cast<float>(tw),
                               label_y, value_sz,
                               focused ? th.accent : th.fg,
                               focused ? 1.0f : 0.9f);
                float track_x = value_x_right - kSliderTrackW;
                float track_y = label_y + kSliderInsetY;
                r.mb_stroke_rect(track_x, track_y,
                                 kSliderTrackW, kSliderTrackH,
                                 2.0f, th.accent,
                                 focused ? 1.0f : 0.7f);
                float frac = (mx > mn)
                    ? static_cast<float>(v - mn) / static_cast<float>(mx - mn)
                    : 0.0f;
                r.mb_fill_rect(track_x, track_y,
                               kSliderTrackW * frac, kSliderTrackH,
                               th.accent, focused ? 1.0f : 0.7f);
                break;
            }

            case RowKind::MaxConcurrentDownloads: {
                int v = prefs_.max_concurrent_downloads, mn = 1, mx = 5;
                std::ostringstream os;
                os << v << "   (" << mn << "-" << mx << ")";
                std::string val = os.str();
                int tw = r.mb_text_width(val, value_sz);
                r.mb_draw_text(val,
                               value_x_right - static_cast<float>(tw),
                               label_y, value_sz,
                               focused ? th.accent : th.fg,
                               focused ? 1.0f : 0.9f);
                float track_x = value_x_right - kSliderTrackW;
                float track_y = label_y + kSliderInsetY;
                r.mb_stroke_rect(track_x, track_y,
                                 kSliderTrackW, kSliderTrackH,
                                 2.0f, th.accent,
                                 focused ? 1.0f : 0.7f);
                float frac = (mx > mn)
                    ? static_cast<float>(v - mn) / static_cast<float>(mx - mn)
                    : 0.0f;
                r.mb_fill_rect(track_x, track_y,
                               kSliderTrackW * frac, kSliderTrackH,
                               th.accent, focused ? 1.0f : 0.7f);
                break;
            }

            case RowKind::IndexerToggles: {
                // Convert "Indexers" header label into a steel-blue caps
                // section header (mirrors the "CAST" / "DIRECTED BY"
                // section labels in DetailScreen). The label was already
                // drawn above in the standard label-color path; we redraw
                // it here in accent2 + small font to match the section
                // header idiom — cheap to overwrite since we use the
                // same x coordinate. Then draw the embedded checkbox list.
                {
                    // Wipe the standard label by redrawing background-
                    // colored over it would be wrong (no fill); instead
                    // we chose to leave the standard label in place but
                    // ALSO add a small "INDEXERS" section subhead is
                    // overkill. So just keep the existing label render
                    // and pivot to the sub-list below.
                }
                // Sub-list. Each row: indicator box (16x16 stroke, optional
                // fill if enabled) + indexer name. Focused indexer in the
                // sub-list gets its own gold ◂ marker — this is the only
                // place where we use a SECONDARY focus indicator (because
                // the OUTER row is also focused, but the sub-cursor
                // distinguishes which entry the SELECT will toggle).
                if (!prowlarr_api_key_available_) {
                    const std::string msg = "Prowlarr API key not configured";
                    int tw = r.mb_text_width(msg, value_sz);
                    r.mb_draw_text(msg,
                                   value_x_right - static_cast<float>(tw),
                                   label_y, value_sz, th.dim, 0.85f);
                    break;
                }
                if (indexers_.empty()) {
                    const std::string msg = "(no indexers returned)";
                    int tw = r.mb_text_width(msg, value_sz);
                    r.mb_draw_text(msg,
                                   value_x_right - static_cast<float>(tw),
                                   label_y, value_sz, th.dim, 0.85f);
                    break;
                }

                // Right-side hint on the header line.
                const std::string hint = "BTN2: toggle";
                int hw = r.mb_text_width(hint, small_sz);
                int sb_h = r.mb_text_baseline(small_sz);
                float hy = center_y - (small_sz / 2.0f)
                         + static_cast<float>(sb_h);
                r.mb_draw_text(hint,
                               value_x_right - static_cast<float>(hw),
                               hy, small_sz, th.dim, 0.85f);

                int visible = std::min<int>(
                    static_cast<int>(indexers_.size()),
                    kIndexerMaxVisible);
                float sub_top = y_top + kRowHeight;
                int sub_baseline = r.mb_text_baseline(small_sz);
                for (int k = 0; k < visible; ++k) {
                    float sy = sub_top + k * kIndexerRowHeight;
                    float sub_center_y = sy + (kIndexerRowHeight / 2.0f);
                    bool sub_focused = focused && (k == indexer_cursor_);

                    // Checkbox indicator. Outline always; filled only
                    // when the indexer is enabled. Color flips green
                    // (highlight1) for enabled, dim for disabled.
                    bool en = indexers_[k].enabled;
                    ::ui::Color box_color = en ? th.highlight1 : th.dim;
                    float box_x = label_x + 16.0f;
                    float box_y = sub_center_y - (kCheckboxSize / 2.0f);
                    r.mb_stroke_rect(box_x, box_y,
                                     kCheckboxSize, kCheckboxSize,
                                     2.0f, box_color,
                                     sub_focused ? 1.0f : 0.85f);
                    if (en) {
                        // Inner fill = 4px inset solid block. Conveys
                        // "checked" without text glyphs (no font fallback
                        // worries) and stays readable at this small size.
                        const float inset = 4.0f;
                        r.mb_fill_rect(box_x + inset, box_y + inset,
                                       kCheckboxSize - 2.0f * inset,
                                       kCheckboxSize - 2.0f * inset,
                                       th.highlight1,
                                       sub_focused ? 1.0f : 0.9f);
                    }

                    // Indexer name to the right of the checkbox.
                    float name_x = box_x + kCheckboxSize + 10.0f;
                    float name_y = sub_center_y - (small_sz / 2.0f)
                                 + static_cast<float>(sub_baseline);
                    ::ui::Color name_color = sub_focused
                        ? th.accent
                        : (en ? th.fg : th.dim);
                    std::string name_drawn = truncate_to_width(
                        r, indexers_[k].name, small_sz,
                        row_w - (name_x - row_x) - 16.0f);
                    r.mb_draw_text(name_drawn, name_x, name_y, small_sz,
                                   name_color, sub_focused ? 1.0f : 0.9f);

                    // Sub-cursor blinking ◂ marker, drawn just inside
                    // the row's left edge. Smaller than the row marker
                    // because we're at small font size here.
                    if (sub_focused && blink_on) {
                        draw_cursor_marker(
                            r,
                            label_x, sub_center_y,
                            th.accent, 1.0f);
                    }
                }
                break;
            }

            case RowKind::RetryAllFailed:
            case RowKind::PauseAllDownloads:
            case RowKind::ResumeAllDownloads: {
                // Action button — gold-outlined, no fill. The button is
                // a fixed-width box on the right side of the row, with
                // the label centered inside. Focused = thicker outline +
                // brighter label + blinking ◂ inside the right edge,
                // matching the DetailScreen action-row idiom.
                const float btn_w = 180.0f;
                float btn_x = value_x_right - btn_w;
                float btn_y = center_y - (kActionButtonH / 2.0f);
                float thickness = focused
                    ? (kActionOutlineW + 1.0f) : kActionOutlineW;
                r.mb_stroke_rect(btn_x, btn_y, btn_w, kActionButtonH,
                                 thickness, th.accent,
                                 focused ? 1.0f : 0.75f);
                std::string lbl =
                    (row.kind == RowKind::RetryAllFailed) ? "Retry"
                  : (row.kind == RowKind::PauseAllDownloads) ? "Pause"
                  : "Resume";
                int tw = r.mb_text_width(lbl, value_sz);
                float tx = btn_x + (btn_w - static_cast<float>(tw)) / 2.0f;
                float ty = center_y - (value_sz / 2.0f)
                         + static_cast<float>(baseline);
                r.mb_draw_text(lbl, tx, ty, value_sz,
                               focused ? th.accent : th.fg,
                               focused ? 1.0f : 0.85f);
                if (focused && blink_on) {
                    // Marker INSIDE the button's right edge — same as
                    // DetailScreen's button cursor, in steel-blue (accent2)
                    // for visual contrast against the gold border.
                    float marker_tip_x = btn_x + btn_w - 18.0f;
                    draw_cursor_marker(r, marker_tip_x, center_y,
                                       th.accent2, 1.0f);
                }
                break;
            }

            case RowKind::HideMovies: {
                // Destructive action — red/orange outlined button. Same
                // treatment as DetailScreen's "Confirm Remove" button:
                // border in highlight2, focused label in highlight2,
                // blinking ◂ in highlight2. The visual difference from
                // the gold action buttons above signals "you're about
                // to do something irreversible from this screen".
                const float btn_w = 240.0f;
                float btn_x = value_x_right - btn_w;
                float btn_y = center_y - (kActionButtonH / 2.0f);
                float thickness = focused
                    ? (kActionOutlineW + 1.0f) : kActionOutlineW;
                r.mb_stroke_rect(btn_x, btn_y, btn_w, kActionButtonH,
                                 thickness, th.highlight2,
                                 focused ? 1.0f : 0.85f);
                const std::string lbl = "Hide Movies feature";
                int tw = r.mb_text_width(lbl, value_sz);
                float tx = btn_x + (btn_w - static_cast<float>(tw)) / 2.0f;
                float ty = center_y - (value_sz / 2.0f)
                         + static_cast<float>(baseline);
                r.mb_draw_text(lbl, tx, ty, value_sz,
                               focused ? th.highlight2 : th.fg,
                               focused ? 1.0f : 0.9f);
                if (focused && blink_on) {
                    float marker_tip_x = btn_x + btn_w - 18.0f;
                    draw_cursor_marker(r, marker_tip_x, center_y,
                                       th.highlight2, 1.0f);
                }
                break;
            }

            case RowKind::AdvancedUrlHint: {
                // Plain dim fine-print at the bottom of the list. Stays
                // un-bordered and non-focusable — handle_input() skips
                // it when navigating.
                const std::string hint =
                    "Advanced:  Radarr :7878  \xE2\x80\xA2  "
                    "Prowlarr :9696  \xE2\x80\xA2  qBittorrent :8080  "
                    "(use magicpi.local)";
                int hint_size = th.font_small_size;
                int hint_baseline = r.mb_text_baseline(hint_size);
                int tw = r.mb_text_width(hint, hint_size);
                float tx = (w - static_cast<float>(tw)) / 2.0f;
                float ty = y_top + (row.height / 2.0f)
                         - (hint_size / 2.0f)
                         + static_cast<float>(hint_baseline);
                r.mb_draw_text(hint, tx, ty, hint_size, th.dim, 0.7f);
                break;
            }
        }

        // --- Section dividers (drawn AFTER each row group's bottom) --
        // We draw these at the bottom of specific rows so they don't
        // get clipped when scrolled and so they sit BETWEEN logical
        // groups rather than ON the row boundary itself.
        //   - After ServiceStatus (separates "diagnostics" from "tunables")
        //   - After MaxConcurrentDownloads (separates "tunables" from
        //     "indexers")
        //   - After IndexerToggles (separates "indexers" from "actions")
        //   - Before HideMovies (red/orange — danger zone divider)
        // The 0.6 alpha matches the muted divider feel — strong enough
        // to read as a section break, soft enough to not compete with
        // the steel-blue header rule.
        float div_y = y_top + row.height + (kRowGap / 2.0f);
        if (row.kind == RowKind::ServiceStatus) {
            draw_section_divider(div_y, th.accent2, 0.6f);
        } else if (row.kind == RowKind::MaxConcurrentDownloads) {
            draw_section_divider(div_y, th.accent2, 0.6f);
        } else if (row.kind == RowKind::IndexerToggles) {
            draw_section_divider(div_y, th.accent2, 0.6f);
        } else if (row.kind == RowKind::ResumeAllDownloads) {
            // Danger-zone divider. Red/orange to telegraph that the next
            // row (HideMovies) is destructive.
            draw_section_divider(div_y, th.highlight2, 0.7f);
        }
    }

    // --- Footer hint -------------------------------------------------
    // Centered, dim, font_small. Same idiom as DetailScreen's bottom
    // hint — one shared visual cue across both screens.
    {
        const std::string hint =
            "Rotate: nav   BTN2: change / refresh   BTN4: back";
        int sz = th.font_small_size;
        int hb = r.mb_text_baseline(sz);
        int tw = r.mb_text_width(hint, sz);
        float hx = (w - static_cast<float>(tw)) / 2.0f;
        float hy = h - kFooterPadY - static_cast<float>(sz)
                 + static_cast<float>(hb);
        r.mb_draw_text(hint, hx, hy, sz, th.dim, 0.85f);
    }

    // --- Transient banner (modal-style) ------------------------------
    // Identical treatment to DetailScreen's banner: dim-bg fill +
    // gold-outline frame + cream text. Sits centered above the footer.
    auto now = std::chrono::steady_clock::now();
    if (!banner_text_.empty() && now < banner_until_) {
        int b_size = th.font_medium_size;
        int b_baseline = r.mb_text_baseline(b_size);
        int tw = r.mb_text_width(banner_text_, b_size);
        float pad_x = 18.0f;
        float pad_y = 10.0f;
        float bw = static_cast<float>(tw) + 2.0f * pad_x;
        float bh = static_cast<float>(b_size) + 2.0f * pad_y;
        float bx = (w - bw) / 2.0f;
        float by = h - 32.0f - bh - 16.0f;
        r.mb_fill_rect(bx, by, bw, bh, th.bg, 0.92f);
        r.mb_stroke_rect(bx, by, bw, bh, 2.0f, th.accent, 1.0f);
        float tx = bx + pad_x;
        float ty = by + pad_y + static_cast<float>(b_baseline);
        r.mb_draw_text(banner_text_, tx, ty, b_size, th.fg, 1.0f);
    }
}

}  // namespace media_browser::ui
