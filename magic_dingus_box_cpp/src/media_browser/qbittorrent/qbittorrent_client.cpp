#include "media_browser/qbittorrent/qbittorrent_client.h"

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

size_t write_cb(char* ptr, size_t size, size_t nmemb, std::string* out) {
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// Captures Set-Cookie response headers so we can extract the SID
// returned by /api/v2/auth/login. qBit's auth is a simple
// username+password POST that returns "Ok." in the body and a SID
// cookie in the headers; subsequent requests must echo the cookie.
size_t header_cb(char* buffer, size_t size, size_t nitems,
                 std::string* out_set_cookie) {
    const size_t n = size * nitems;
    static const char kKey[] = "Set-Cookie:";
    // CASE-INSENSITIVE compare — qBittorrent emits the header as
    // "set-cookie:" (lowercase) and HTTP headers are case-insensitive
    // per RFC 7230. The original strncmp here was case-sensitive,
    // which silently broke every login (header was never matched →
    // SID cookie never captured → "no SID cookie returned" warning
    // on every kiosk request → entire qBit live-overlay path dead).
    if (n > sizeof(kKey) - 1
        && strncasecmp(buffer, kKey, sizeof(kKey) - 1) == 0) {
        // Trim leading space after "Set-Cookie:"
        size_t start = sizeof(kKey) - 1;
        while (start < n && (buffer[start] == ' ' || buffer[start] == '\t')) {
            ++start;
        }
        // Strip trailing CRLF
        size_t end = n;
        while (end > start && (buffer[end-1] == '\r' || buffer[end-1] == '\n')) {
            --end;
        }
        // qBit's cookie is "SID=...; HttpOnly; path=/" — we only need
        // the "SID=..." part (everything before the first ';').
        std::string raw(buffer + start, end - start);
        size_t semi = raw.find(';');
        if (semi != std::string::npos) raw.resize(semi);
        if (raw.compare(0, 4, "SID=") == 0) {
            *out_set_cookie = raw;
        }
    }
    return n;
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

    CURL* curl = curl_easy_init();
    if (!curl) {
        last_error_ = "curl init failed";
        return false;
    }

    std::string resp_body;
    std::string set_cookie;
    std::string url = cfg_.base_url + "/api/v2/auth/login";

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST,           1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &resp_body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA,     &set_cookie);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        static_cast<long>(cfg_.timeout_secs));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);
    // qBit requires a Referer header that matches its own origin
    // — without it the login returns 403 even with valid credentials.
    // It validates the host portion only, not the full URL.
    struct curl_slist* hdrs = nullptr;
    std::string referer = "Referer: " + cfg_.base_url;
    hdrs = curl_slist_append(hdrs, referer.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        last_error_ = std::string("qbit login: ") + curl_easy_strerror(rc);
        return false;
    }
    if (http_code != 200 || resp_body != "Ok.") {
        last_error_ = "qbit login HTTP " + std::to_string(http_code)
                    + " body='" + resp_body + "'";
        return false;
    }
    if (set_cookie.empty()) {
        last_error_ = "qbit login: no SID cookie returned";
        return false;
    }

    sid_cookie_ = std::move(set_cookie);
    last_error_.clear();
    return true;
}

std::string QbittorrentClient::http_get(const std::string& path) {
    std::string url = cfg_.base_url + path;

    auto perform = [&]() -> std::pair<long, std::string> {
        CURL* curl = curl_easy_init();
        if (!curl) return {0, {}};

        std::string body;
        curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,        static_cast<long>(cfg_.timeout_secs));
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);

        struct curl_slist* hdrs = nullptr;
        std::string cookie_hdr;
        {
            std::lock_guard<std::mutex> lk(cookie_mtx_);
            if (!sid_cookie_.empty()) {
                cookie_hdr = "Cookie: " + sid_cookie_;
                hdrs = curl_slist_append(hdrs, cookie_hdr.c_str());
            }
        }
        std::string referer = "Referer: " + cfg_.base_url;
        hdrs = curl_slist_append(hdrs, referer.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

        CURLcode rc = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK) {
            std::lock_guard<std::mutex> lk(cookie_mtx_);
            last_error_ = std::string("curl: ") + curl_easy_strerror(rc);
            return {0, {}};
        }
        return {http_code, body};
    };

    auto [code, body] = perform();
    // qBit returns 403 when the SID expired (or was never set). One
    // re-login retry recovers transparently. We don't loop further
    // because if the second auth fails too the credentials are wrong
    // and looping won't help.
    if (code == 403) {
        std::lock_guard<std::mutex> lk(cookie_mtx_);
        sid_cookie_.clear();
        if (!login_locked()) return {};
        auto [c2, b2] = perform();
        if (c2 >= 400) {
            last_error_ = "qbit HTTP " + std::to_string(c2);
            return {};
        }
        return b2;
    }
    if (code >= 400) {
        std::lock_guard<std::mutex> lk(cookie_mtx_);
        last_error_ = "qbit HTTP " + std::to_string(code);
        return {};
    }
    return body;
}

std::string QbittorrentClient::http_post(const std::string& path,
                                          const std::string& body) {
    // qBit's mutating endpoints (pause/resume/delete) all use POST with
    // form-urlencoded body. Not used by QueueScreen (which only reads),
    // but kept symmetric with http_get for future cancel-via-qBit
    // functionality and easier mocking.
    std::string url = cfg_.base_url + path;
    CURL* curl = curl_easy_init();
    if (!curl) {
        last_error_ = "curl init failed";
        return {};
    }
    std::string resp;
    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST,           1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        static_cast<long>(cfg_.timeout_secs));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);

    struct curl_slist* hdrs = nullptr;
    std::string cookie_hdr;
    {
        std::lock_guard<std::mutex> lk(cookie_mtx_);
        if (!sid_cookie_.empty()) {
            cookie_hdr = "Cookie: " + sid_cookie_;
            hdrs = curl_slist_append(hdrs, cookie_hdr.c_str());
        }
    }
    std::string referer = "Referer: " + cfg_.base_url;
    hdrs = curl_slist_append(hdrs, referer.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        std::lock_guard<std::mutex> lk(cookie_mtx_);
        last_error_ = std::string("curl: ") + curl_easy_strerror(rc);
        return {};
    }
    return resp;
}

std::vector<QbitTorrent> QbittorrentClient::get_torrents() {
    // Lazy-login: if we don't have a session yet, get one. Subsequent
    // calls reuse the cookie until qBit invalidates it (~1hr default).
    {
        std::lock_guard<std::mutex> lk(cookie_mtx_);
        if (sid_cookie_.empty()) {
            if (!login_locked()) {
                spdlog::warn("[qbit] login failed: {}", last_error_);
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
        last_error_ = "qbit response not a JSON array";
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
    last_error_.clear();
    return out;
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
    if (!last_error_.empty()) {
        spdlog::warn("[qbit] delete_torrent({}) failed: {}",
                     lower_hash, last_error_);
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
    last_error_.clear();
    http_post("/api/v2/torrents/stop", "hashes=all");  // qBit 5.x
    if (!last_error_.empty()) {
        // Fall back to legacy 4.x alias.
        last_error_.clear();
        http_post("/api/v2/torrents/pause", "hashes=all");
    }
    if (!last_error_.empty()) {
        spdlog::warn("[qbit] pause_all failed: {}", last_error_);
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
    last_error_.clear();
    http_post("/api/v2/torrents/start", "hashes=all");  // qBit 5.x
    if (!last_error_.empty()) {
        last_error_.clear();
        http_post("/api/v2/torrents/resume", "hashes=all");  // qBit 4.x
    }
    if (!last_error_.empty()) {
        spdlog::warn("[qbit] resume_all failed: {}", last_error_);
        return false;
    }
    spdlog::info("[qbit] resumed all torrents (playback end)");
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

}  // namespace media_browser
