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

constexpr float kTopBarHeight      = 56.0f;
constexpr float kBottomBarHeight   = 40.0f;
constexpr float kSidePadding       = 48.0f;
constexpr float kRowHeight         = 52.0f;
constexpr float kRowGap            = 8.0f;
constexpr float kIndexerRowHeight  = 28.0f;
constexpr int   kIndexerMaxVisible = 5;
constexpr float kDotRadius         = 8.0f;

// How long the placeholder banner stays on screen.
constexpr std::chrono::milliseconds kBannerMs{2500};

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

    // --- Top bar ------------------------------------------------------
    r.mb_fill_rect(0.0f, 0.0f, w, kTopBarHeight, th.bg, 0.75f);
    r.mb_fill_rect(0.0f, kTopBarHeight - 1.0f, w, 1.0f, th.dim, 0.6f);
    {
        int title_size = th.font_large_size;
        int title_baseline = r.mb_text_baseline(title_size);
        float title_y = (kTopBarHeight / 2.0f) - (title_size / 2.0f)
                      + static_cast<float>(title_baseline);
        r.mb_draw_text("Movies Settings", kSidePadding, title_y,
                       title_size, th.accent, 1.0f);
    }

    // --- Scroll window -----------------------------------------------
    float list_top    = kTopBarHeight + 16.0f;
    float list_bottom = h - kBottomBarHeight;
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

    // Auto-scroll to keep cursor visible.
    if (!rows_.empty()) {
        float c_top = row_y[cursor_];
        float c_bot = c_top + rows_[cursor_].height;
        if (c_top < scroll_y_) scroll_y_ = c_top;
        if (c_bot > scroll_y_ + list_h) {
            scroll_y_ = c_bot - list_h;
        }
        if (scroll_y_ < 0.0f) scroll_y_ = 0.0f;
    }

    // --- Rows ---------------------------------------------------------
    const float row_x   = kSidePadding;
    const float row_w   = w - 2.0f * kSidePadding;
    const int label_sz  = th.font_medium_size;
    const int value_sz  = th.font_medium_size;
    const int baseline  = r.mb_text_baseline(label_sz);

    for (size_t i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_[i];
        float y_top = list_top + row_y[i] - scroll_y_;
        float y_bot = y_top + row.height;
        if (y_bot < list_top || y_top > list_bottom) continue;  // culled

        bool focused = (static_cast<int>(i) == cursor_);

        // Row background + focus outline. AdvancedUrlHint is rendered as
        // plain fine-print without a frame.
        if (row.kind != RowKind::AdvancedUrlHint) {
            r.mb_fill_rect(row_x, y_top, row_w, row.height,
                           th.dim, focused ? 0.28f : 0.12f);
            if (focused) {
                r.mb_stroke_rect(row_x, y_top, row_w, row.height,
                                 2.0f, th.accent, 1.0f);
            } else {
                r.mb_stroke_rect(row_x, y_top, row_w, row.height,
                                 1.0f, th.dim, 0.4f);
            }
        }

        // Label (left-aligned, vertically centered in the first kRowHeight
        // slice — even for the tall IndexerToggles row).
        float label_x = row_x + 16.0f;
        float label_y = y_top + (kRowHeight / 2.0f) - (label_sz / 2.0f)
                      + static_cast<float>(baseline);
        if (row.kind != RowKind::AdvancedUrlHint) {
            r.mb_draw_text(row.label, label_x, label_y, label_sz,
                           th.fg, focused ? 1.0f : 0.9f);
        }

        // Right-side value column starts here.
        float value_x_right = row_x + row_w - 16.0f;

        // Per-row custom rendering.
        switch (row.kind) {
            case RowKind::ServiceStatus: {
                // Three labeled dots. Draw right-to-left so the qBittorrent
                // dot sits near the right edge.
                struct Entry { const char* name; bool up; };
                Entry entries[3] = {
                    {"qBittorrent", health_.qbittorrent},
                    {"Prowlarr",    health_.prowlarr},
                    {"Radarr",      health_.radarr},
                };
                float cursor_x = value_x_right;
                for (const auto& e : entries) {
                    int tw = r.mb_text_width(e.name, value_sz);
                    // Dot sits right of the name (conceptually) but we lay
                    // out right-to-left, so dot first.
                    ::ui::Color dot = e.up ? th.highlight1 : th.highlight2;
                    float dot_d = kDotRadius * 2.0f;
                    float dot_x = cursor_x - dot_d;
                    float dot_y = y_top + (kRowHeight / 2.0f) - kDotRadius;
                    r.mb_fill_rect(dot_x, dot_y, dot_d, dot_d, dot, 1.0f);
                    cursor_x -= dot_d + 6.0f;

                    float tx = cursor_x - static_cast<float>(tw);
                    r.mb_draw_text(e.name, tx, label_y, value_sz,
                                   th.fg, 0.9f);
                    cursor_x = tx - 18.0f;
                }
                break;
            }

            case RowKind::QualityProfile: {
                std::string val;
                if (quality_profiles_.empty()) {
                    val = "(Radarr not reachable)";
                } else {
                    val = "< " +
                        quality_profiles_[quality_profile_idx_].name + " >";
                }
                int tw = r.mb_text_width(val, value_sz);
                float tx = value_x_right - static_cast<float>(tw);
                r.mb_draw_text(val, tx, label_y, value_sz,
                               focused ? th.accent : th.fg,
                               focused ? 1.0f : 0.9f);
                break;
            }

            case RowKind::MinSeeders: {
                std::ostringstream os;
                os << "< " << prefs_.min_seeders << " >   (0-20)";
                std::string val = os.str();
                int tw = r.mb_text_width(val, value_sz);
                r.mb_draw_text(val,
                               value_x_right - static_cast<float>(tw),
                               label_y, value_sz,
                               focused ? th.accent : th.fg,
                               focused ? 1.0f : 0.9f);
                break;
            }

            case RowKind::StoragePath: {
                std::string val;
                if (root_folders_.empty()) {
                    val = "(no root folders configured)";
                } else {
                    const auto& rf = root_folders_.front();
                    val = rf.path + "  " +
                        format_free_space(rf.free_space_bytes,
                                          rf.total_space_bytes);
                }
                int tw = r.mb_text_width(val, value_sz);
                r.mb_draw_text(val,
                               value_x_right - static_cast<float>(tw),
                               label_y, value_sz, th.dim, 0.9f);
                break;
            }

            case RowKind::LowSpaceThresholdGb: {
                std::ostringstream os;
                os << "< " << prefs_.low_space_threshold_gb
                   << " GB >   (10-200)";
                std::string val = os.str();
                int tw = r.mb_text_width(val, value_sz);
                r.mb_draw_text(val,
                               value_x_right - static_cast<float>(tw),
                               label_y, value_sz,
                               focused ? th.accent : th.fg,
                               focused ? 1.0f : 0.9f);
                break;
            }

            case RowKind::MaxConcurrentDownloads: {
                std::ostringstream os;
                os << "< " << prefs_.max_concurrent_downloads
                   << " >   (1-5)";
                std::string val = os.str();
                int tw = r.mb_text_width(val, value_sz);
                r.mb_draw_text(val,
                               value_x_right - static_cast<float>(tw),
                               label_y, value_sz,
                               focused ? th.accent : th.fg,
                               focused ? 1.0f : 0.9f);
                break;
            }

            case RowKind::IndexerToggles: {
                if (!prowlarr_api_key_available_) {
                    std::string msg =
                        "Prowlarr API key not configured";
                    int tw = r.mb_text_width(msg, value_sz);
                    r.mb_draw_text(msg,
                                   value_x_right - static_cast<float>(tw),
                                   label_y, value_sz, th.dim, 0.85f);
                } else if (indexers_.empty()) {
                    std::string msg = "(no indexers returned)";
                    int tw = r.mb_text_width(msg, value_sz);
                    r.mb_draw_text(msg,
                                   value_x_right - static_cast<float>(tw),
                                   label_y, value_sz, th.dim, 0.85f);
                } else {
                    // Embedded sub-list, up to kIndexerMaxVisible rows.
                    int visible = std::min<int>(
                        static_cast<int>(indexers_.size()),
                        kIndexerMaxVisible);
                    float sub_top = y_top + kRowHeight;
                    int sub_sz = th.font_small_size;
                    int sub_baseline = r.mb_text_baseline(sub_sz);
                    for (int k = 0; k < visible; ++k) {
                        float sy = sub_top + k * kIndexerRowHeight;
                        bool sub_focused =
                            focused && (k == indexer_cursor_);
                        if (sub_focused) {
                            r.mb_fill_rect(row_x + 8.0f, sy,
                                           row_w - 16.0f,
                                           kIndexerRowHeight,
                                           th.accent, 0.15f);
                        }

                        // Left: [x] or [ ] + name
                        std::string prefix =
                            indexers_[k].enabled ? "[x] " : "[ ] ";
                        std::string line = prefix + indexers_[k].name;
                        float ty = sy + (kIndexerRowHeight / 2.0f)
                                 - (sub_sz / 2.0f)
                                 + static_cast<float>(sub_baseline);
                        ::ui::Color col = indexers_[k].enabled
                            ? th.fg : th.dim;
                        r.mb_draw_text(line, label_x, ty, sub_sz,
                                       col, sub_focused ? 1.0f : 0.85f);
                    }
                    // Hint on the header line.
                    std::string hint = "SELECT to toggle";
                    int tw = r.mb_text_width(hint, value_sz);
                    r.mb_draw_text(hint,
                                   value_x_right -
                                       static_cast<float>(tw),
                                   label_y, value_sz, th.dim, 0.8f);
                }
                break;
            }

            case RowKind::RetryAllFailed:
            case RowKind::PauseAllDownloads:
            case RowKind::ResumeAllDownloads: {
                std::string val = "[ Press SELECT ]";
                int tw = r.mb_text_width(val, value_sz);
                r.mb_draw_text(val,
                               value_x_right - static_cast<float>(tw),
                               label_y, value_sz,
                               focused ? th.accent : th.dim,
                               focused ? 1.0f : 0.85f);
                break;
            }

            case RowKind::HideMovies: {
                // Checkbox is always unchecked from this screen's POV —
                // if it's checked, the feature is hidden and we wouldn't
                // be here. "Off" draws dim, focused outline signals intent.
                std::string val = "[ ] Off   (SELECT to hide)";
                int tw = r.mb_text_width(val, value_sz);
                r.mb_draw_text(val,
                               value_x_right - static_cast<float>(tw),
                               label_y, value_sz,
                               focused ? th.accent : th.fg,
                               focused ? 1.0f : 0.85f);
                break;
            }

            case RowKind::AdvancedUrlHint: {
                // Draw plain centered fine-print.
                std::string hint =
                    "Advanced:  Radarr :7878  ·  Prowlarr :9696  ·  "
                    "qBittorrent :8080  "
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
    }

    // --- Transient banner --------------------------------------------
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
        float by = list_bottom - bh - 12.0f;
        r.mb_fill_rect(bx, by, bw, bh, th.bg, 0.9f);
        r.mb_stroke_rect(bx, by, bw, bh, 2.0f, th.accent, 1.0f);
        float tx = bx + pad_x;
        float ty = by + pad_y + static_cast<float>(b_baseline);
        r.mb_draw_text(banner_text_, tx, ty, b_size, th.fg, 1.0f);
    }

    // --- Bottom hint bar ---------------------------------------------
    float bar_y = h - kBottomBarHeight;
    r.mb_fill_rect(0.0f, bar_y, w, kBottomBarHeight, th.bg, 0.75f);
    r.mb_fill_rect(0.0f, bar_y, w, 1.0f, th.dim, 0.6f);
    {
        const std::string hint =
            "Rotate: nav   RCLICK: toggle   "
            "BTN2: refresh   BTN4: back (hold: exit)";
        int hint_size = th.font_small_size;
        int hint_baseline = r.mb_text_baseline(hint_size);
        int tw = r.mb_text_width(hint, hint_size);
        float hint_x = (w - static_cast<float>(tw)) / 2.0f;
        float hint_y = bar_y + (kBottomBarHeight / 2.0f)
                     - (hint_size / 2.0f)
                     + static_cast<float>(hint_baseline);
        r.mb_draw_text(hint, hint_x, hint_y, hint_size, th.fg, 0.85f);
    }
}

}  // namespace media_browser::ui
