#include "media_browser/ui/mb_settings_screen.h"
#include "media_browser/ui/mb_chrome.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include <curl/curl.h>
#include <json/json.h>

#include "app/settings_persistence.h"
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

// Horizontal safe-area inset. Matches mb_chrome::kSafeInset_px (60) so
// the row label/value columns and the footer-hint row stay clear of the
// 40 px wood-frame overlay that wraps every Marquee screen, with 20 px
// breathing room between content and the inner edge of the frame.
// Pre-v1.6.x this was 32, which let rows extend under the wood frame.
constexpr float kPaddingX        = 60.0f;   // matches chrome::kSafeInset_px

// Top header strip is now drawn by chrome::draw_screen_header (the
// Marquee 5-tab strip with "Settings" marked active). The legacy
// kHeaderBaselineY / kHeaderRuleY constants were retired here as part
// of the v1.6.x chrome unification — see search_screen.cpp for the
// same migration pattern.

// Settings list — first row baseline. Anchored below the chrome header
// (which ends at y = kSafeInset_px + kHeaderHeight_px = 120) with
// generous breathing room so the rows don't crowd the tab strip.
//   y = 60 (safe inset) + 60 (header band) + 40 (kPad5 breathing room)
constexpr float kListTopY        = 160.0f;  // first row label baseline lives here
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

// Bottom-hint footer is now drawn by chrome::draw_footer_hints, which
// owns its own offset-from-bottom math (anchored to the wood frame, not
// the screen edge). The legacy kFooterPadY constant retired in v1.6.x.

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

// Draw the gold blinking cursor marker at `(tip_x, center_y)`. By default
// the triangle points LEFT — same orientation as the home-menu playlist
// cursor and the DetailScreen action-button cursor — and is used for
// markers that sit INSIDE a button on its right edge (so the tip points
// inward toward the button label).
//
// When `point_right` is true, the geometry is mirrored: the triangle
// points RIGHT, with its base on the left and tip on the right. This is
// used for row-level markers that sit to the LEFT of the row's label
// (so the tip visually "points at" the label text, like an arrowhead
// indicating where focus is). Same horizontal footprint either way:
// the triangle occupies [tip_x, tip_x + kMarkerW], so callers can pass
// the same `tip_x` regardless of orientation.
void draw_cursor_marker(::ui::Renderer& r,
                        float tip_x, float center_y,
                        ::ui::Color color, float alpha,
                        bool point_right = false) {
    constexpr float kMarkerSize = 8.0f;          // half-height of the triangle
    constexpr float kMarkerW    = kMarkerSize * 1.2f;  // horizontal extent
    if (point_right) {
        // Base on the left edge, tip on the right (pointing → at text).
        r.mb_fill_triangle(
            tip_x,            center_y - kMarkerSize,  // top-left  (base)
            tip_x,            center_y + kMarkerSize,  // bottom-left (base)
            tip_x + kMarkerW, center_y,                // pointing tip (right)
            color, alpha);
    } else {
        // Base on the right edge, tip on the left (pointing ← into a
        // button or away from the row). Original orientation.
        r.mb_fill_triangle(
            tip_x + kMarkerW, center_y - kMarkerSize,  // top-right (base)
            tip_x + kMarkerW, center_y + kMarkerSize,  // bottom-right (base)
            tip_x,            center_y,                // pointing tip (left)
            color, alpha);
    }
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

// Resolve the Prowlarr API key with the same 3-stage chain main.cpp
// uses for both Radarr and Prowlarr:
//   1. MDB_PROWLARR_API_KEY env var (kiosk override)
//   2. PROWLARR_API_KEY env var (systemd EnvironmentFile, set by the
//      v1.6.x systemd unit which loads /opt/magic_dingus_box/services/.env)
//   3. Parse /opt/magic_dingus_box/services/.env directly (fallback
//      for unprovisioned Pis / dev workflows where the systemd env
//      propagation isn't wired up yet)
// Mirrors main.cpp's resolution exactly so operators don't need to keep
// duplicate env entries in sync — and so the Settings screen's
// "is the key configured?" check matches what the rest of the kiosk
// actually sees at runtime.
namespace {
std::string read_env_file_key(const std::string& path,
                              const std::string& key) {
    std::ifstream f(path);
    if (!f) return "";
    std::string line;
    const std::string prefix = key + "=";
    while (std::getline(f, line)) {
        if (line.rfind(prefix, 0) == 0) {
            std::string v = line.substr(prefix.size());
            while (!v.empty() &&
                   (v.back() == '\n' || v.back() == '\r' || v.back() == ' '))
                v.pop_back();
            return v;
        }
    }
    return "";
}

std::string resolve_prowlarr_key() {
    std::string k = env_or_empty("MDB_PROWLARR_API_KEY");
    if (!k.empty()) return k;
    k = env_or_empty("PROWLARR_API_KEY");
    if (!k.empty()) return k;
    return read_env_file_key("/opt/magic_dingus_box/services/.env",
                             "PROWLARR_API_KEY");
}
}  // namespace

MbSettingsScreen::MbSettingsScreen(RadarrClient& radarr,
                                   ::app::AppState& state,
                                   std::function<void()> on_hide_feature)
    : radarr_(radarr),
      state_(state),
      on_hide_feature_(std::move(on_hide_feature)) {
    // Tag availability of the Prowlarr API key up front — used by the
    // Indexer row to show a "not configured" fallback message.
    prowlarr_api_key_available_ = !resolve_prowlarr_key().empty();
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

    // Section header height — gold ZenDots heading + 8 px gap above the
    // first row in the section. Matches the Marquee design mockup where
    // each section reads as a clear group with breathing room.
    constexpr float kSectionHeaderH = 38.0f;

    // ---- Library ----------------------------------------------------
    // Quality + storage-related preferences. Top-of-list because these
    // are the settings most operators touch on every fresh install.
    add(RowKind::SectionHeader,        "Library", kSectionHeaderH);
    add(RowKind::QualityProfile,       "Quality preference");
    add(RowKind::AutoGrabOnAdd,        "Auto-grab on add");
    add(RowKind::PlaybackShowFrame,    "Wood frame during playback");
    add(RowKind::StoragePath,          "Storage path");
    add(RowKind::RefreshLibrary,       "Refresh library");

    // ---- Downloads --------------------------------------------------
    // Per-download tuning + bulk actions on the active queue.
    add(RowKind::SectionHeader,        "Downloads", kSectionHeaderH);
    add(RowKind::MinSeeders,           "Minimum seeders");
    add(RowKind::LowSpaceThresholdGb,  "Low-space pause threshold (GB)");
    add(RowKind::MaxConcurrentDownloads, "Max concurrent downloads");
    add(RowKind::RetryAllFailed,       "Retry all failed downloads");
    add(RowKind::PauseAllDownloads,    "Pause all downloads");
    add(RowKind::ResumeAllDownloads,   "Resume all downloads");

    // ---- Sources ----------------------------------------------------
    // Where Radarr looks for releases. Indexer row expands to fit up to
    // kIndexerMaxVisible rows of its embedded sub-list.
    add(RowKind::SectionHeader,        "Sources", kSectionHeaderH);
    int visible_indexers = std::min<int>(static_cast<int>(indexers_.size()),
                                         kIndexerMaxVisible);
    if (visible_indexers <= 0) visible_indexers = 1;  // room for "not configured"
    float indexer_height = kRowHeight
        + static_cast<float>(visible_indexers) * kIndexerRowHeight;
    add(RowKind::IndexerToggles,       "Indexers", indexer_height);

    // ---- CRT overlay ------------------------------------------------
    // Independent CRT intensity stack for the Marquee menu screens —
    // each row cycles OFF / Low / Medium / High on SELECT and writes
    // back to state.display_settings.mb_<name>. The kiosk's home-menu
    // CRT settings (in the main Settings menu) stay untouched, so
    // operators can dial in different looks for the two contexts.
    // Skipped during movie playback (operator direction).
    add(RowKind::SectionHeader,        "CRT overlay", kSectionHeaderH);
    add(RowKind::CrtScanlines,         "Scanlines");
    add(RowKind::CrtWarmth,            "Warmth");
    add(RowKind::CrtGlow,              "Glow");
    add(RowKind::CrtRgbMask,           "RGB mask");
    add(RowKind::CrtBloom,             "Bloom");
    add(RowKind::CrtInterlacing,       "Interlacing");
    add(RowKind::CrtFlicker,           "Flicker");

    // ---- Diagnostics ------------------------------------------------
    // Live service-health dots. SELECT re-pings (and BTN2 is a global
    // refresh shortcut anywhere on this screen).
    add(RowKind::SectionHeader,        "Diagnostics", kSectionHeaderH);
    add(RowKind::ServiceStatus,        "Services");

    // ---- Danger Zone ------------------------------------------------
    // Hide-feature is destructive (re-locks the Media Browser back
    // behind the secret-sequence unlock); it gets its own section so
    // it doesn't mingle with everyday tuning.
    add(RowKind::SectionHeader,        "Danger zone", kSectionHeaderH);
    add(RowKind::HideMovies,           "Hide Movies feature");

    // ---- Footer fine-print -----------------------------------------
    add(RowKind::AdvancedUrlHint,      "Advanced", 36.0f);

    // After rebuilding, push cursor onto a focusable row if it landed
    // on a non-focusable one (SectionHeader / AdvancedUrlHint).
    cursor_ = std::clamp(cursor_, 0, static_cast<int>(rows_.size()) - 1);
    int guard = static_cast<int>(rows_.size());
    while (guard-- > 0 &&
           (rows_[cursor_].kind == RowKind::SectionHeader ||
            rows_[cursor_].kind == RowKind::AdvancedUrlHint)) {
        if (cursor_ + 1 < static_cast<int>(rows_.size())) ++cursor_;
        else if (cursor_ > 0) --cursor_;
        else break;
    }
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
    const std::string api_key = resolve_prowlarr_key();
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

        // BTN1 (PREV, yellow) — walk one tab left in the Marquee strip.
        // v1.6.x 6-tab strip: Popular | Top Rated | Search | Library |
        // Queue | Settings. Settings sits at index 5 (rightmost), so PREV
        // always lands on Queue at index 4.
        if (e.action == platform::InputAction::PREV && e.pressed) {
            return Screen::Queue;
        }

        // BTN3 (NEXT, green) — would walk one tab right, but Settings is
        // the rightmost tab — eat the event so it doesn't fall through.
        // Same vocabulary as Search's NEXT dead-end pre-v1.6.x.
        if (e.action == platform::InputAction::NEXT && e.pressed) {
            continue;
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

        // Row-kind classifiers — keep the navigation/edit/action vocabulary
        // in one place so adding a new row only requires updating these
        // and the build_rows() / render() switches.
        auto is_non_focusable = [](RowKind k) {
            return k == RowKind::SectionHeader ||
                   k == RowKind::AdvancedUrlHint;
        };
        auto is_cyclable = [](RowKind k) {
            // Rows whose value is adjusted by rotary CW/CCW once the row
            // is "highlighted" (editing_=true). SELECT enters and exits
            // the edit mode for these.
            return k == RowKind::QualityProfile ||
                   k == RowKind::MinSeeders ||
                   k == RowKind::LowSpaceThresholdGb ||
                   k == RowKind::MaxConcurrentDownloads;
        };

        // Vertical movement (D-pad UP/DOWN): always vertical row nav,
        // and always exits editing mode if it was active. ROTATE_VERTICAL
        // is the unambiguous "I want to leave this row" gesture.
        const bool is_vertical_event =
            (e.action == platform::InputAction::ROTATE_VERTICAL);
        // ROTATE (rotary CW/CCW + D-pad LEFT/RIGHT) acts as vertical row
        // nav when not editing, and as a value adjuster when editing.
        // This is the v1.6.x "rotary walks rows by default; click to
        // adjust" behavior the operator asked for — D-pad LEFT/RIGHT
        // inherits the same dual mode for parity.
        const bool is_rotate_nav =
            (e.action == platform::InputAction::ROTATE && !editing_);

        if (is_vertical_event || is_rotate_nav) {
            int delta = e.delta;
            if (delta == 0) continue;
            // Any vertical-style movement leaves edit mode — cleaner than
            // making the user click again to exit.
            editing_ = false;
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

            // Skip any non-focusable row (SectionHeader, AdvancedUrlHint).
            // We step in the direction we were already moving — if the
            // caller walked DOWN onto a non-focusable row we keep walking
            // DOWN; if we hit the end we reverse. This keeps the cursor
            // off non-focusable rows even if more get inserted later.
            {
                int step = (delta > 0) ? +1 : -1;
                // Bound the scan to n iterations so we can't infinite-loop
                // if every row is marked non-focusable (shouldn't happen,
                // but defensive).
                int guard = n;
                while (guard-- > 0 && is_non_focusable(rows_[cursor_].kind)) {
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

        // ROTATE while editing: adjust the focused row's value. Only
        // cyclable rows have an editing mode; toggles flip inline on
        // SELECT and don't enter edit mode at all.
        if (e.action == platform::InputAction::ROTATE && editing_) {
            int delta = e.delta;
            if (delta == 0) continue;
            switch (rows_[cursor_].kind) {
                case RowKind::QualityProfile: {
                    if (quality_profiles_.empty()) break;
                    int n = static_cast<int>(quality_profiles_.size());
                    quality_profile_idx_ =
                        (((quality_profile_idx_ + delta) % n) + n) % n;
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
                    // Non-cyclable rows shouldn't have entered editing —
                    // belt-and-suspenders.
                    editing_ = false;
                    break;
            }
            continue;
        }

        // SELECT (rotary click + gamepad A): commit/exit editing on
        // cyclables, toggle on booleans, execute on actions.
        if (e.action == platform::InputAction::SELECT && e.pressed) {
            // Already editing? SELECT exits — value is committed by the
            // ROTATE adjustments while editing was active.
            if (editing_) {
                editing_ = false;
                continue;
            }

            // Cyclable rows: SELECT enters editing (next ROTATE adjusts).
            if (is_cyclable(rows_[cursor_].kind)) {
                // For QualityProfile, only enter editing if there's
                // something to cycle through; otherwise show a banner.
                if (rows_[cursor_].kind == RowKind::QualityProfile &&
                    quality_profiles_.empty()) {
                    show_banner("No Radarr quality profiles loaded");
                    continue;
                }
                editing_ = true;
                continue;
            }

            // Boolean toggles: flip inline, no edit mode needed.
            if (rows_[cursor_].kind == RowKind::AutoGrabOnAdd) {
                prefs_.auto_grab_on_add = !prefs_.auto_grab_on_add;
                continue;
            }
            if (rows_[cursor_].kind == RowKind::PlaybackShowFrame) {
                // Toggle THE persisted display setting (lives in
                // state.display_settings.mb_playback_show_frame so the
                // render path in main.cpp can read it directly). Save
                // immediately so the choice survives reboots — same
                // pattern enhanced_crt_enabled uses.
                state_.display_settings.mb_playback_show_frame =
                    !state_.display_settings.mb_playback_show_frame;
                ::app::SettingsPersistence::save_settings(state_);
                continue;
            }

            // CRT-overlay intensity rows: SELECT cycles OFF / Low /
            // Medium / High via DisplaySettings::cycle_setting (the
            // same helper the kiosk's main Display settings use).
            // Each row writes back to the corresponding mb_<name>
            // float and persists immediately so the choice survives
            // reboots. Independent from the kiosk's intensities —
            // adjusting "Scanlines" here only changes the Marquee
            // menu look, not the home menu.
            {
                auto& s = state_.display_settings;
                float* target = nullptr;
                switch (rows_[cursor_].kind) {
                    case RowKind::CrtScanlines:   target = &s.mb_scanline_intensity;    break;
                    case RowKind::CrtWarmth:      target = &s.mb_warmth_intensity;      break;
                    case RowKind::CrtGlow:        target = &s.mb_glow_intensity;        break;
                    case RowKind::CrtRgbMask:     target = &s.mb_rgb_mask_intensity;    break;
                    case RowKind::CrtBloom:       target = &s.mb_bloom_intensity;       break;
                    case RowKind::CrtInterlacing: target = &s.mb_interlacing_intensity; break;
                    case RowKind::CrtFlicker:     target = &s.mb_flicker_intensity;     break;
                    default: break;
                }
                if (target) {
                    s.cycle_setting(*target);
                    ::app::SettingsPersistence::save_settings(state_);
                    continue;
                }
            }

            // Action / display rows: dispatch immediately.
            switch (rows_[cursor_].kind) {
                case RowKind::ServiceStatus:
                    // SELECT on the services row re-checks them.
                    refresh_service_health();
                    break;
                case RowKind::IndexerToggles:
                    if (!prowlarr_api_key_available_ || indexers_.empty()) {
                        show_banner(
                            "Prowlarr API key not configured — "
                            "set PROWLARR_API_KEY in services/.env");
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
                case RowKind::RefreshLibrary:
                    show_banner("Library refresh not implemented in MVP — "
                                "use Radarr web UI");
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
                    // SectionHeader / AdvancedUrlHint / StoragePath are
                    // non-actionable; SELECT is a no-op.
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

    // --- Top header (Marquee design) ---------------------------------
    // "Settings" title (left, ZenDots) + the same 5-tab Marquee strip
    // BrowseScreen / LibraryScreen / SearchScreen all render, with
    // Settings marked active (gold border + gold label). Keeps visual
    // continuity across the Marquee section so the user sees one
    // consistent UI when walking BTN1/BTN3 across the strip.
    //
    // The previous "Movies · Magic Dingus Box" sub-info string is
    // dropped — the active "Settings" tab in the strip is already the
    // unambiguous "you are here" cue, and the sub-info would fight the
    // tabs for header real estate.
    namespace chrome = ::media_browser::ui::chrome;
    {
        // v1.6.x strip: Now Playing dropped, Settings added on the right.
        // Settings sits at index 4 (rightmost). Labels must stay in sync
        // with kVisibleTabs[] in browse_screen.cpp.
        const std::vector<chrome::TabSpec> tabs = {
            {"Popular",   chrome::TabState::Inactive},
            {"Top Rated", chrome::TabState::Inactive},
            {"Search",    chrome::TabState::Inactive},
            {"Library",   chrome::TabState::Inactive},
            {"Queue",     chrome::TabState::Inactive},
            {"Settings",  chrome::TabState::Active},
        };
        chrome::draw_screen_header(r, screen_w, "Settings",
                                   tabs, /*focused_tab=*/-1);
    }

    // --- Scroll window -----------------------------------------------
    // List sits between the chrome header (bottom = 120) and the chrome
    // footer-hints row (drawn at y = screen_h - kFrameInset_px(40)
    // - kPad3(16) = h-56, with the keyhint glyphs sitting above that
    // baseline). We reserve enough at the bottom for the footer + the
    // wood-frame inset so no row is clipped by either.
    //   list_bottom = h - 40 (frame inset) - 30 (footer band) - 16 (pad)
    //               = h - 86
    // Pre-v1.6.x list_bottom was h-32 which let the last row render
    // under the wood frame.
    float list_top    = kListTopY;
    float list_bottom = h - 86.0f;
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
            // Sits in the gap between the chrome header (ends at y=120)
            // and the first row (kListTopY=160). Anchored at y=130 so it
            // reads as a transient sub-line below the title rather than
            // overlapping the tab strip.
            float my = 130.0f
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

        // ---- Section headers --------------------------------------
        // Gold ZenDots heading with a steel-blue underline rule. Owned
        // visually by the rows below it — no value column, no marker,
        // not focusable. Matches the Library / Network / Sources groups
        // in the Marquee design mockup.
        if (row.kind == RowKind::SectionHeader) {
            constexpr int kSectionFontPx = 20;
            const int sh_baseline = r.mb_text_baseline(kSectionFontPx);
            // Heading sits ~10 px down from the section top, so the
            // gap above it reads as separation from the previous group.
            const float sh_y = y_top + 10.0f
                             + static_cast<float>(sh_baseline);
            r.mb_draw_title_text(row.label, row_x, sh_y,
                                 kSectionFontPx, th.accent);
            // Thin steel-blue rule sits at the section's bottom edge so
            // it visually anchors the heading to the rows beneath.
            const float rule_y = y_top + row.height - 1.0f;
            r.mb_draw_line(row_x, rule_y, row_x + row_w, rule_y,
                           1.0f, th.dim, 0.5f);
            continue;
        }

        // Label x is the same for every row; value column right-edge is
        // at row_x + row_w. We keep the label_x flush left (no inset)
        // because there's no row fill to inset against.
        float label_x = row_x;
        float center_y = y_top + (kRowHeight / 2.0f);
        float label_y = center_y - (label_sz / 2.0f)
                      + static_cast<float>(baseline);
        float value_x_right = row_x + row_w;

        // ---- Edit-mode highlight ----------------------------------
        // When the focused row is in editing mode, draw a 2 px gold
        // rectangle around the whole row body — same vocabulary as the
        // active-tab rectangle on the chrome strip. The user enters
        // edit mode via SELECT on a cyclable row; ROTATE then adjusts
        // the value, and another SELECT exits. Visual state mirrors the
        // Marquee design mockup's gold-bordered "Quality preference"
        // row.
        if (focused && editing_) {
            r.mb_stroke_rect(row_x - 6.0f, y_top + 2.0f,
                             row_w + 12.0f, kRowHeight - 4.0f,
                             2.0f, th.accent, 0.95f);
        }

        // Focused-row decoration: blinking gold ◂ marker 18 px left of
        // the label baseline-vertical-center. Drawn only in NAVIGATION
        // mode (not editing) — when editing, the gold border above is
        // the unambiguous "you are here" cue and the marker would feel
        // redundant. Also skipped for AdvancedUrlHint (non-focusable
        // fine print) and HideMovies (uses a red/orange variant below
        // to underline its destructive nature).
        ::ui::Color label_color = focused ? th.accent : th.fg;
        if (focused && blink_on && !editing_
            && row.kind != RowKind::AdvancedUrlHint
            && row.kind != RowKind::HideMovies) {
            // Row marker sits to the LEFT of the label and points RIGHT
            // → at the label text (like an arrowhead indicating focus).
            draw_cursor_marker(r,
                               label_x - kCursorMarkerOffsetX, center_y,
                               th.accent, 1.0f,
                               /*point_right=*/true);
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

            case RowKind::AutoGrabOnAdd: {
                // Boolean toggle. Renders "On" in green / "Off" in dim
                // on the right, no ◀ ▶ hints (single-click toggle, no
                // edit mode). Same value-column right-edge as cyclable
                // rows so the column reads as a coherent vertical band.
                const std::string val = prefs_.auto_grab_on_add ? "On" : "Off";
                const ::ui::Color val_color = prefs_.auto_grab_on_add
                    ? th.highlight1     // green
                    : th.dim;
                int tw = r.mb_text_width(val, value_sz);
                r.mb_draw_text(val,
                               value_x_right - static_cast<float>(tw),
                               label_y, value_sz, val_color,
                               focused ? 1.0f : 0.9f);
                break;
            }

            case RowKind::PlaybackShowFrame: {
                // Boolean toggle for the wood-frame overlay during
                // movie playback. Reads / writes
                // state.display_settings.mb_playback_show_frame (saved
                // to config/settings.json on each toggle). Same On/Off
                // visual treatment as AutoGrabOnAdd for consistency.
                const bool on = state_.display_settings.mb_playback_show_frame;
                const std::string val = on ? "On" : "Off";
                const ::ui::Color val_color = on ? th.highlight1 : th.dim;
                int tw = r.mb_text_width(val, value_sz);
                r.mb_draw_text(val,
                               value_x_right - static_cast<float>(tw),
                               label_y, value_sz, val_color,
                               focused ? 1.0f : 0.9f);
                break;
            }

            case RowKind::CrtScanlines:
            case RowKind::CrtWarmth:
            case RowKind::CrtGlow:
            case RowKind::CrtRgbMask:
            case RowKind::CrtBloom:
            case RowKind::CrtInterlacing:
            case RowKind::CrtFlicker: {
                // CRT intensity rows. Render OFF/Low/Medium/High based
                // on the corresponding mb_*_intensity float. Same
                // bucket boundaries the kiosk's Display settings use
                // (matches DisplaySettings::cycle_setting threshold
                // pattern: 0.05 / 0.32 / 0.55 / 0.75).
                const auto& s = state_.display_settings;
                float v = 0.0f;
                switch (row.kind) {
                    case RowKind::CrtScanlines:   v = s.mb_scanline_intensity;    break;
                    case RowKind::CrtWarmth:      v = s.mb_warmth_intensity;      break;
                    case RowKind::CrtGlow:        v = s.mb_glow_intensity;        break;
                    case RowKind::CrtRgbMask:     v = s.mb_rgb_mask_intensity;    break;
                    case RowKind::CrtBloom:       v = s.mb_bloom_intensity;       break;
                    case RowKind::CrtInterlacing: v = s.mb_interlacing_intensity; break;
                    case RowKind::CrtFlicker:     v = s.mb_flicker_intensity;     break;
                    default: break;
                }
                const char* label =
                    (v <= 0.05f)  ? "Off"
                  : (v <= 0.32f)  ? "Low"
                  : (v <= 0.55f)  ? "Medium"
                                  : "High";
                ::ui::Color val_color = (v <= 0.05f) ? th.dim : th.highlight1;
                int tw = r.mb_text_width(label, value_sz);
                r.mb_draw_text(label,
                               value_x_right - static_cast<float>(tw),
                               label_y, value_sz, val_color,
                               focused ? 1.0f : 0.9f);
                break;
            }

            case RowKind::RefreshLibrary: {
                // Action row. Right side: a steel-blue "▶ rescans <path>"
                // hint that doubles as a description of what the action
                // will do. The arrow is the same U+25B6 glyph the rest
                // of the Marquee design uses for "active control" cues.
                std::string root = "/mnt/movies";
                if (!root_folders_.empty() && !root_folders_[0].path.empty()) {
                    root = root_folders_[0].path;
                }
                const std::string hint =
                    std::string("\xE2\x96\xB6 rescans ") + root;
                int tw = r.mb_text_width(hint, small_sz);
                int sb = r.mb_text_baseline(small_sz);
                float hy = center_y - (small_sz / 2.0f)
                         + static_cast<float>(sb);
                r.mb_draw_text(hint,
                               value_x_right - static_cast<float>(tw),
                               hy, small_sz, th.dim,
                               focused ? 1.0f : 0.85f);
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

                    // Sub-cursor blinking marker, drawn just inside the
                    // row's left edge. Same right-pointing orientation
                    // as the parent row marker so the tip arrows at
                    // the indexer name text.
                    if (sub_focused && blink_on) {
                        draw_cursor_marker(
                            r,
                            label_x, sub_center_y,
                            th.accent, 1.0f,
                            /*point_right=*/true);
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
                                       th.dim, 1.0f);
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

        // Inter-group dividers retired in v1.6.x — SectionHeader rows
        // (Library / Downloads / Sources / Diagnostics / Danger zone)
        // now carry the visual separation. The draw_section_divider
        // helper above stays declared in case future row kinds need
        // ad-hoc dividers (e.g. a one-off rule above HideMovies for
        // emphasis); right now nothing calls it.
        (void)draw_section_divider;
    }

    // --- Footer hint (Marquee design) --------------------------------
    // Hints differ by mode so the user always sees what the controls do
    // RIGHT NOW. BTN1 surfaces the tab nav (Settings → Search). No BTN3
    // hint — Settings is the rightmost tab and BTN3 is a dead-end here.
    if (editing_) {
        chrome::draw_footer_hints(r, screen_w, screen_h, {
            {"Rotary", "Adjust"},
            {"A",      "Save"},
            {"Down",   "Cancel"},
            {"BTN4",   "Back"},
        });
    } else {
        chrome::draw_footer_hints(r, screen_w, screen_h, {
            {"BTN1",   "Search"},
            {"Rotary", "Nav"},
            {"A",      "Edit"},
            {"BTN2",   "Refresh"},
            {"BTN4",   "Back"},
        });
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
