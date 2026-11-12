#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/http/http_request.h"

#include <curl/curl.h>
#include <json/json.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <strings.h>  // strncasecmp (POSIX, case-insensitive header match)

namespace media_browser {

namespace {

// qBittorrent is a localhost service (127.0.0.1:8080, published from
// Gluetun's netns): 2 s connect bound on every call.
constexpr long kConnectTimeoutSecs = 2;

// Extracts the SID from one captured response header line, or "" when the
// line is not qBit's session cookie. qBit's auth is a simple
// username+password POST that returns "Ok." in the body and a SID cookie in
// the headers; subsequent requests must echo the cookie.
std::string sid_from_header_line(const std::string& line) {
    static const char kKey[] = "Set-Cookie:";
    const size_t n = line.size();
    // CASE-INSENSITIVE compare — qBittorrent emits the header as
    // "set-cookie:" (lowercase) and HTTP headers are case-insensitive
    // per RFC 7230. The original strncmp here was case-sensitive,
    // which silently broke every login (header was never matched →
    // SID cookie never captured → "no SID cookie returned" warning
    // on every kiosk request → entire qBit live-overlay path dead).
    if (n <= sizeof(kKey) - 1 ||
        strncasecmp(line.c_str(), kKey, sizeof(kKey) - 1) != 0) {
        return {};
    }
    // Trim leading space after "Set-Cookie:" (the transport already
    // stripped the trailing CRLF).
    size_t start = sizeof(kKey) - 1;
    while (start < n && (line[start] == ' ' || line[start] == '\t')) ++start;
    // qBit's cookie is "SID=...; HttpOnly; path=/" — we only need
    // the "SID=..." part (everything before the first ';').
    std::string raw = line.substr(start);
    size_t semi = raw.find(';');
    if (semi != std::string::npos) raw.resize(semi);
    return raw.compare(0, 4, "SID=") == 0 ? raw : std::string{};
}

}  // namespace

QbittorrentClient::QbittorrentClient(Config cfg) : cfg_(std::move(cfg)) {}

QbittorrentClient::~QbittorrentClient() = default;

bool QbittorrentClient::login_locked() {
    // POST username + password as application/x-www-form-urlencoded
    // to /api/v2/auth/login. Returns "Ok." on success and the SID
    // cookie in the response headers.
    std::string body = "username=" + cfg_.username
                     + "&password=" + cfg_.password;

    http::Request req;
    req.method = http::Method::Post;
    req.url = cfg_.base_url + "/api/v2/auth/login";
    req.body = std::move(body);
    req.capture_headers = true;
    req.timeout_secs = cfg_.timeout_secs;
    req.connect_timeout_secs = kConnectTimeoutSecs;
    // qBit requires a Referer header that matches its own origin
    // — without it the login returns 403 even with valid credentials.
    // It validates the host portion only, not the full URL.
    req.headers = {"Referer: " + cfg_.base_url};
    auto resp = http::request(req);
    if (resp.curl_code == CURLE_FAILED_INIT) {
        set_error(resp.error);
        return false;
    }
    const CURLcode rc = resp.curl_code;
    const long http_code = resp.status;
    const std::string& resp_body = resp.body;
    // Last matching line wins, as the old per-header callback did.
    std::string set_cookie;
    for (const auto& line : resp.headers) {
        std::string sid = sid_from_header_line(line);
        if (!sid.empty()) set_cookie = std::move(sid);
    }

    if (rc != CURLE_OK) {
        set_error("qbit login: " + resp.error);
        return false;
    }
    if (http_code != 200 || resp_body != "Ok.") {
        set_error("qbit login HTTP " + std::to_string(http_code)
                    + " body='" + resp_body + "'");
        return false;
    }
    if (set_cookie.empty()) {
        set_error("qbit login: no SID cookie returned");
        return false;
    }

    sid_cookie_ = std::move(set_cookie);
    set_error({});
    return true;
}

std::string QbittorrentClient::http_get(const std::string& path) {
    std::string url = cfg_.base_url + path;

    auto perform = [&]() -> std::pair<long, std::string> {
        http::Request req;
        req.url = url;
        req.timeout_secs = cfg_.timeout_secs;
        req.connect_timeout_secs = kConnectTimeoutSecs;
        {
            std::lock_guard<std::mutex> lk(cookie_mtx_);
            if (!sid_cookie_.empty()) {
                req.headers.push_back("Cookie: " + sid_cookie_);
            }
        }
        req.headers.push_back("Referer: " + cfg_.base_url);
        auto resp = http::request(req);
        if (resp.curl_code == CURLE_FAILED_INIT) {
            return {0, {}};
        }
        if (!resp.transport_ok()) {
            std::lock_guard<std::mutex> lk(cookie_mtx_);
            set_error("curl: " + resp.error);
            return {0, {}};
        }
        return {resp.status, std::move(resp.body)};
    };

    auto [code, body] = perform();
    // qBit returns 403 when the SID expired (or was never set). One
    // re-login retry recovers transparently. We don't loop further
    // because if the second auth fails too the credentials are wrong
    // and looping won't help.
    if (code == 403) {
        // Scope the lock to the cookie reset + re-login only. The retry
        // perform() MUST run after the guard releases: perform() takes
        // cookie_mtx_ itself to read the SID (and again on error), and
        // std::mutex is non-recursive — calling it under the lock
        // self-deadlocked the worker thread, and the next render-thread
        // caller then blocked on the wedged mutex until the systemd
        // watchdog killed the kiosk.
        {
            std::lock_guard<std::mutex> lk(cookie_mtx_);
            sid_cookie_.clear();
            if (!login_locked()) return {};
        }
        auto [c2, b2] = perform();
        if (c2 >= 400) {
            std::lock_guard<std::mutex> lk(cookie_mtx_);
            set_error("qbit HTTP " + std::to_string(c2));
            return {};
        }
        return b2;
    }
    if (code >= 400) {
        std::lock_guard<std::mutex> lk(cookie_mtx_);
        set_error("qbit HTTP " + std::to_string(code));
        return {};
    }
    return body;
}

std::string QbittorrentClient::http_post(const std::string& path,
                                          const std::string& body) {
    // qBit's mutating endpoints (pause/resume/delete) all use POST with
    // form-urlencoded body. Mirrors http_get's status handling: this
    // used to check only the curl transport code, so a stale-but-present
    // SID (qBit's ~1h session expiry, or any qBit container restart
    // under the gluetun cascade) got a 403 that looked like success —
    // pause_all logged "paused all torrents" while torrents kept
    // hammering the drive, and delete_torrent counted purges that never
    // happened. Callers detect failure via last_error_, so every failure
    // path here must set it.
    std::string url = cfg_.base_url + path;

    auto perform = [&]() -> std::pair<long, std::string> {
        http::Request req;
        req.method = http::Method::Post;
        req.body = body;
        req.url = url;
        req.timeout_secs = cfg_.timeout_secs;
        req.connect_timeout_secs = kConnectTimeoutSecs;
        {
            std::lock_guard<std::mutex> lk(cookie_mtx_);
            if (!sid_cookie_.empty()) {
                req.headers.push_back("Cookie: " + sid_cookie_);
            }
        }
        req.headers.push_back("Referer: " + cfg_.base_url);
        auto resp = http::request(req);
        if (resp.curl_code == CURLE_FAILED_INIT) {
            std::lock_guard<std::mutex> lk(cookie_mtx_);
            set_error(resp.error);
            return {0, {}};
        }
        if (!resp.transport_ok()) {
            std::lock_guard<std::mutex> lk(cookie_mtx_);
            set_error("curl: " + resp.error);
            return {0, {}};
        }
        return {resp.status, std::move(resp.body)};
    };

    auto [code, resp] = perform();
    if (code == 403) {
        // One re-login retry, with the lock scoped exactly as in
        // http_get — the retry perform() relocks cookie_mtx_ and would
        // self-deadlock if called under the guard.
        {
            std::lock_guard<std::mutex> lk(cookie_mtx_);
            sid_cookie_.clear();
            if (!login_locked()) return {};
        }
        auto [c2, r2] = perform();
        if (c2 >= 400) {
            std::lock_guard<std::mutex> lk(cookie_mtx_);
            set_error("qbit HTTP " + std::to_string(c2));
            return {};
        }
        return r2;
    }
    if (code >= 400) {
        std::lock_guard<std::mutex> lk(cookie_mtx_);
        set_error("qbit HTTP " + std::to_string(code));
        return {};
    }
    return resp;
}

std::vector<QbitTorrent> QbittorrentClient::get_torrents() try {
    // Lazy-login: if we don't have a session yet, get one. Subsequent
    // calls reuse the cookie until qBit invalidates it (~1hr default).
    {
        std::lock_guard<std::mutex> lk(cookie_mtx_);
        if (sid_cookie_.empty()) {
            if (!login_locked()) {
                spdlog::warn("[qbit] login failed: {}", last_error());
                return {};
            }
        }
    }

    std::string body = http_get("/api/v2/torrents/info");
    if (body.empty()) return {};

    Json::CharReaderBuilder rb;
    Json::Value root;
    std::string err;
    std::istringstream is(body);
    if (!Json::parseFromStream(rb, is, &root, &err) || !root.isArray()) {
        set_error("qbit response not a JSON array");
        return {};
    }

    std::vector<QbitTorrent> out;
    out.reserve(root.size());
    for (const auto& t : root) {
        if (!t.isObject()) continue;
        QbitTorrent qt;
        qt.hash       = t.get("hash", "").asString();
        // Normalize hash to lowercase for stable comparison with Radarr
        // (Radarr's downloadId is uppercase; matching is case-insensitive
        // semantically but explicit lowercase avoids future surprises).
        std::transform(qt.hash.begin(), qt.hash.end(), qt.hash.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        qt.name           = t.get("name", "").asString();
        qt.progress       = t.get("progress", 0.0).asDouble();
        qt.size           = t.get("size", 0).asInt64();
        qt.downloaded     = t.get("downloaded", 0).asInt64();
        qt.dlspeed        = t.get("dlspeed", 0).asInt();
        qt.upspeed        = t.get("upspeed", 0).asInt();
        qt.num_seeds      = t.get("num_seeds", 0).asInt();
        qt.num_leechs     = t.get("num_leechs", 0).asInt();
        qt.num_complete   = t.get("num_complete", 0).asInt();
        qt.num_incomplete = t.get("num_incomplete", 0).asInt();
        qt.eta_seconds    = t.get("eta", 0).asInt();
        qt.state          = t.get("state", "").asString();
        out.push_back(std::move(qt));
    }
    set_error({});
    return out;
} catch (const std::exception& e) {
    // Unexpected JSON shape/type (Json::LogicError): report failure — the
    // empty/nullopt/false this returns is each method's normal failure
    // value — instead of letting it escape into a worker and terminate.
    spdlog::error("[qbit] get_torrents: unexpected response shape: {}", e.what());
    return {};
}

bool QbittorrentClient::delete_torrent(const std::string& hash,
                                        bool delete_files) {
    // Lazy-login if needed.
    {
        std::lock_guard<std::mutex> lk(cookie_mtx_);
        if (sid_cookie_.empty()) {
            if (!login_locked()) return false;
        }
    }

    // qBit's /api/v2/torrents/delete takes a comma-separated list of
    // hashes; we send a single one. Lowercase the input so the API
    // matches qBit's internal storage convention.
    std::string lower_hash = hash;
    std::transform(lower_hash.begin(), lower_hash.end(), lower_hash.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    std::string body = "hashes=" + lower_hash
                     + "&deleteFiles=" + (delete_files ? "true" : "false");
    std::string resp = http_post("/api/v2/torrents/delete", body);

    // qBit returns 200 with empty body on success, even when the hash
    // doesn't exist (no-op). last_error_ is set by http_post if the
    // call itself failed (network, 4xx, etc.).
    if (!last_error().empty()) {
        spdlog::warn("[qbit] delete_torrent({}) failed: {}",
                     lower_hash, last_error());
        return false;
    }
    spdlog::info("[qbit] delete_torrent({}, files={})",
                 lower_hash, delete_files);
    return true;
}

// try_post_with_fallback: hit modern_path first; if it sets
// last_error_ (transport failure or 4xx) try legacy_path. Returns
// true if either call cleared the error. Defined as a private
// member to access protected http_post.
bool QbittorrentClient::pause_all() {
    {
        std::lock_guard<std::mutex> lk(cookie_mtx_);
        if (sid_cookie_.empty()) {
            if (!login_locked()) return false;
        }
    }
    // hashes=all is qBit's documented "select all torrents" sentinel.
    set_error({});
    http_post("/api/v2/torrents/stop", "hashes=all");  // qBit 5.x
    if (!last_error().empty()) {
        // Fall back to legacy 4.x alias.
        set_error({});
        http_post("/api/v2/torrents/pause", "hashes=all");
    }
    if (!last_error().empty()) {
        spdlog::warn("[qbit] pause_all failed: {}", last_error());
        return false;
    }
    spdlog::info("[qbit] paused all torrents (playback start)");
    return true;
}

bool QbittorrentClient::resume_all() {
    {
        std::lock_guard<std::mutex> lk(cookie_mtx_);
        if (sid_cookie_.empty()) {
            if (!login_locked()) return false;
        }
    }
    set_error({});
    http_post("/api/v2/torrents/start", "hashes=all");  // qBit 5.x
    if (!last_error().empty()) {
        set_error({});
        http_post("/api/v2/torrents/resume", "hashes=all");  // qBit 4.x
    }
    if (!last_error().empty()) {
        spdlog::warn("[qbit] resume_all failed: {}", last_error());
        return false;
    }
    spdlog::info("[qbit] resumed all torrents (playback end)");
    return true;
}

bool QbittorrentClient::set_alt_speed_limits_enabled(bool on) {
    // Read-then-toggle (qBit has no explicit-set endpoint — see header).
    //
    // Deliberately NO explicit lazy-login block here, unlike pause_all():
    // http_get/http_post already recover a missing or expired session via
    // their 403 -> login_locked() -> retry path, and keeping this method
    // free of direct login_locked() calls means unit tests can drive the
    // full toggle logic through the virtual http seam alone (login_locked
    // does real curl I/O and is not part of the seam).
    auto read_mode = [this]() -> std::string {
        std::string mode = http_get("/api/v2/transfer/speedLimitsMode");
        // Plain-text "0"/"1"; tolerate trailing newline/CR from proxies.
        while (!mode.empty() &&
               (mode.back() == '\n' || mode.back() == '\r' ||
                mode.back() == ' ')) {
            mode.pop_back();
        }
        return mode;
    };

    set_error({});
    std::string mode = read_mode();
    if (mode != "0" && mode != "1") {
        // Empty (transport/auth failure — last_error_ already set by
        // http_get) or malformed (anything but the documented "0"/"1").
        // Either way we cannot know the current state, so a toggle could
        // flip it the WRONG way — fail instead.
        if (last_error().empty()) {
            set_error("qbit speedLimitsMode unrecognized: '" + mode + "'");
        }
        spdlog::warn("[qbit] set_alt_speed_limits_enabled({}) read failed: {}",
                     on, last_error());
        return false;
    }

    const bool current = (mode == "1");
    if (current == on) {
        return true;  // idempotent: already in the requested state
    }

    set_error({});
    http_post("/api/v2/transfer/toggleSpeedLimitsMode", "");
    if (!last_error().empty()) {
        spdlog::warn("[qbit] toggleSpeedLimitsMode failed: {}", last_error());
        return false;
    }

    // Success = the FINAL state matches the request, so verify with a
    // re-read rather than trusting the toggle's empty 200.
    set_error({});
    std::string mode2 = read_mode();
    if (mode2 != (on ? "1" : "0")) {
        if (last_error().empty()) {
            set_error("qbit speedLimitsMode still '" + mode2
                      + "' after toggle");
        }
        spdlog::warn("[qbit] set_alt_speed_limits_enabled({}) verify failed: {}",
                     on, last_error());
        return false;
    }
    spdlog::info("[qbit] alt speed limits (trickle) {}", on ? "ON" : "OFF");
    return true;
}

bool QbittorrentClient::configure_alt_speed_limits(int dl_bytes_s, int up_bytes_s) {
    // setPreferences takes a form-urlencoded body whose single field is
    // json=<object>. alt_dl_limit / alt_up_limit are in KiB/s (preference
    // units — NOT the byte/s of the /transfer/ limit endpoints).
    // No explicit lazy-login for the same seam-testability reason as
    // set_alt_speed_limits_enabled above.
    set_error({});
    const std::string body = "json={\"alt_dl_limit\":"
                           + std::to_string(dl_bytes_s)
                           + ",\"alt_up_limit\":"
                           + std::to_string(up_bytes_s) + "}";
    http_post("/api/v2/app/setPreferences", body);
    if (!last_error().empty()) {
        spdlog::warn("[qbit] configure_alt_speed_limits({}, {}) failed: {}",
                     dl_bytes_s, up_bytes_s, last_error());
        return false;
    }
    spdlog::info("[qbit] alt speed limits configured (dl={} KiB/s, up={} KiB/s)",
                 dl_bytes_s, up_bytes_s);
    return true;
}

std::unordered_map<std::string, QbitTorrent>
QbittorrentClient::get_torrents_by_hash() {
    auto list = get_torrents();
    std::unordered_map<std::string, QbitTorrent> map;
    map.reserve(list.size());
    for (auto& t : list) {
        std::string key = t.hash;
        map.emplace(std::move(key), std::move(t));
    }
    return map;
}

std::optional<QbitTorrent>
QbittorrentClient::get_torrent(const std::string& hash) {
    auto map = get_torrents_by_hash();
    auto it = map.find(hash);
    if (it == map.end()) return std::nullopt;
    return it->second;
}

}  // namespace media_browser
