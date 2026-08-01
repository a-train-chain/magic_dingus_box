#include "media_browser/sonarr/sonarr_client.h"
#include "media_browser/sonarr/sonarr_parsers.h"
#include "media_browser/radarr/radarr_client.h"   // normalize_prefix (one implementation)
#include "media_browser/radarr/radarr_parsers.h"  // parse_system_status

#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include <sstream>

namespace media_browser {

namespace {

size_t curl_write_cb(void* ptr, size_t size, size_t nmemb, void* ud) {
    auto* s = static_cast<std::string*>(ud);
    s->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

// RFC 3986 percent-encoding: unreserved characters pass through, everything
// else becomes %HH. Copied in spirit from tmdb_client.cpp rather than from
// RadarrClient::lookup's minimal encoder — that one allocates a CURL handle
// per call in ProwlarrClient's variant and percent-encodes '.'/'-' needlessly.
std::string url_encode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3 / 2);
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned char c : s) {
        const bool unreserved = (c >= 'A' && c <= 'Z')
                              || (c >= 'a' && c <= 'z')
                              || (c >= '0' && c <= '9')
                              || c == '-' || c == '_'
                              || c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

}  // namespace

SonarrClient::SonarrClient(Config config) : cfg_(std::move(config)) {
    cfg_.container_library_prefix = normalize_prefix(cfg_.container_library_prefix);
    cfg_.host_library_prefix      = normalize_prefix(cfg_.host_library_prefix);
    // libcurl reference-counts init/cleanup pairs, and RadarrClient/TmdbClient
    // already run their own — matching them here keeps the client usable on
    // its own (the test_media_browser CLI constructs one with nothing else
    // alive).
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

SonarrClient::~SonarrClient() {
    curl_global_cleanup();
}

std::string SonarrClient::normalize_prefix(std::string s) {
    // One implementation, shared with the Radarr client.
    return RadarrClient::normalize_prefix(std::move(s));
}

std::string SonarrClient::build_lookup_path_tmdb(int tmdb_id) {
    // The "tmdb:" prefix is matched literally by Sonarr's SkyHook proxy —
    // percent-encoding the colon is unnecessary and obscures the contract.
    return "/api/v3/series/lookup?term=tmdb:" + std::to_string(tmdb_id);
}

std::string SonarrClient::build_lookup_path_term(const std::string& term) {
    return "/api/v3/series/lookup?term=" + url_encode(term);
}

// --- transport -----------------------------------------------------------

namespace {
// Shared curl setup for all four verbs.
struct CurlRequest {
    CURL* curl = nullptr;
    curl_slist* headers = nullptr;
    std::string body;
    ~CurlRequest() {
        if (headers) curl_slist_free_all(headers);
        if (curl) curl_easy_cleanup(curl);
    }
};
}  // namespace

std::string SonarrClient::http_get(const std::string& path) {
    CurlRequest req;
    req.curl = curl_easy_init();
    if (!req.curl) { set_error("curl init failed"); return {}; }
    const std::string url = cfg_.base_url + path;
    req.headers = curl_slist_append(req.headers,
                                    ("X-Api-Key: " + cfg_.api_key).c_str());
    curl_easy_setopt(req.curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(req.curl, CURLOPT_HTTPHEADER, req.headers);
    curl_easy_setopt(req.curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(req.curl, CURLOPT_WRITEDATA, &req.body);
    curl_easy_setopt(req.curl, CURLOPT_TIMEOUT,
                     static_cast<long>(cfg_.timeout_secs));
    // Required off the main thread: without it libcurl's resolver uses
    // SIGALRM and crashes with a SIGSEGV inside the signal handler.
    // ProwlarrClient documents the same; RadarrClient omits it (a latent bug
    // this client deliberately does not copy).
    curl_easy_setopt(req.curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(req.curl);
    long http_code = 0;
    curl_easy_getinfo(req.curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (rc != CURLE_OK) { set_error(curl_easy_strerror(rc)); return {}; }
    if (http_code >= 400) {
        std::ostringstream os; os << "HTTP " << http_code;
        set_error(os.str());
        return {};
    }
    return req.body;
}

std::string SonarrClient::http_post(const std::string& path, const std::string& body) {
    CurlRequest req;
    req.curl = curl_easy_init();
    if (!req.curl) { set_error("curl init failed"); return {}; }
    const std::string url = cfg_.base_url + path;
    req.headers = curl_slist_append(req.headers,
                                    ("X-Api-Key: " + cfg_.api_key).c_str());
    req.headers = curl_slist_append(req.headers, "Content-Type: application/json");
    curl_easy_setopt(req.curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(req.curl, CURLOPT_HTTPHEADER, req.headers);
    curl_easy_setopt(req.curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(req.curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(req.curl, CURLOPT_WRITEDATA, &req.body);
    curl_easy_setopt(req.curl, CURLOPT_TIMEOUT,
                     static_cast<long>(cfg_.timeout_secs));
    curl_easy_setopt(req.curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(req.curl);
    long http_code = 0;
    curl_easy_getinfo(req.curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (rc != CURLE_OK) { set_error(curl_easy_strerror(rc)); return {}; }
    if (http_code >= 400) {
        // Include the body: Sonarr's validation failures (seriesExistsValidator,
        // bad qualityProfileId) explain themselves there and nowhere else.
        std::ostringstream os; os << "HTTP " << http_code << ": " << req.body;
        set_error(os.str());
        return {};
    }
    return req.body;
}

std::string SonarrClient::http_put(const std::string& path, const std::string& body) {
    CurlRequest req;
    req.curl = curl_easy_init();
    if (!req.curl) { set_error("curl init failed"); return {}; }
    const std::string url = cfg_.base_url + path;
    req.headers = curl_slist_append(req.headers,
                                    ("X-Api-Key: " + cfg_.api_key).c_str());
    req.headers = curl_slist_append(req.headers, "Content-Type: application/json");
    curl_easy_setopt(req.curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(req.curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(req.curl, CURLOPT_HTTPHEADER, req.headers);
    curl_easy_setopt(req.curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(req.curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(req.curl, CURLOPT_WRITEDATA, &req.body);
    curl_easy_setopt(req.curl, CURLOPT_TIMEOUT,
                     static_cast<long>(cfg_.timeout_secs));
    curl_easy_setopt(req.curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(req.curl);
    long http_code = 0;
    curl_easy_getinfo(req.curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (rc != CURLE_OK) { set_error(curl_easy_strerror(rc)); return {}; }
    if (http_code >= 400) {
        std::ostringstream os; os << "HTTP " << http_code << ": " << req.body;
        set_error(os.str());
        return {};
    }
    return req.body;
}

std::string SonarrClient::http_delete(const std::string& path) {
    CurlRequest req;
    req.curl = curl_easy_init();
    if (!req.curl) { set_error("curl init failed"); return {}; }
    const std::string url = cfg_.base_url + path;
    req.headers = curl_slist_append(req.headers,
                                    ("X-Api-Key: " + cfg_.api_key).c_str());
    curl_easy_setopt(req.curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(req.curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(req.curl, CURLOPT_HTTPHEADER, req.headers);
    curl_easy_setopt(req.curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(req.curl, CURLOPT_WRITEDATA, &req.body);
    curl_easy_setopt(req.curl, CURLOPT_TIMEOUT,
                     static_cast<long>(cfg_.timeout_secs));
    curl_easy_setopt(req.curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(req.curl);
    long http_code = 0;
    curl_easy_getinfo(req.curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (rc != CURLE_OK) { set_error(curl_easy_strerror(rc)); return {}; }
    if (http_code >= 400) {
        std::ostringstream os; os << "HTTP " << http_code;
        set_error(os.str());
        return {};
    }
    return req.body;
}

// --- endpoints -----------------------------------------------------------

bool SonarrClient::is_reachable() {
    return !http_get("/ping").empty();
}

std::optional<SystemStatus> SonarrClient::get_status() {
    auto resp = http_get("/api/v3/system/status");
    if (resp.empty()) return std::nullopt;
    // Same {version, buildTime} shape Radarr serves.
    return RadarrParsers::parse_system_status(resp);
}

std::vector<SeriesSearchHit>
SonarrClient::lookup_by_tmdb(int tmdb_id, const std::string& title_fallback) {
    auto resp = http_get(build_lookup_path_tmdb(tmdb_id));
    auto hits = resp.empty() ? std::vector<SeriesSearchHit>{}
                             : SonarrParsers::parse_series_lookup(resp);
    if (!hits.empty() || title_fallback.empty()) return hits;
    // No TMDB->TVDB mapping in SkyHook for this show. Retry as free text.
    spdlog::info("[sonarr] tmdb:{} had no lookup match; falling back to "
                 "title search '{}'", tmdb_id, title_fallback);
    return lookup(title_fallback);
}

std::vector<SeriesSearchHit> SonarrClient::lookup(const std::string& query) {
    auto resp = http_get(build_lookup_path_term(query));
    if (resp.empty()) return {};
    return SonarrParsers::parse_series_lookup(resp);
}

std::optional<std::vector<Series>> SonarrClient::get_library_checked() {
    auto resp = http_get("/api/v3/series");
    if (resp.empty()) return std::nullopt;
    return SonarrParsers::parse_series_list(resp);
}

std::vector<Series> SonarrClient::get_library() {
    return get_library_checked().value_or(std::vector<Series>{});
}

std::optional<Series> SonarrClient::get_series(int sonarr_id) {
    auto resp = http_get("/api/v3/series/" + std::to_string(sonarr_id));
    if (resp.empty()) return std::nullopt;
    return SonarrParsers::parse_series(resp);
}

std::optional<std::vector<Series>> SonarrClient::find_series_by_tvdb(int tvdb_id) {
    auto resp = http_get("/api/v3/series?tvdbId=" + std::to_string(tvdb_id));
    if (resp.empty()) return std::nullopt;  // transport/HTTP failure — NOT "absent"
    return SonarrParsers::parse_series_list(resp);
}

std::vector<QualityProfile> SonarrClient::get_quality_profiles() {
    auto resp = http_get("/api/v3/qualityprofile");
    if (resp.empty()) return {};
    return SonarrParsers::parse_quality_profiles(resp);
}

std::vector<RootFolder> SonarrClient::get_root_folders() {
    auto resp = http_get("/api/v3/rootfolder");
    if (resp.empty()) return {};
    return SonarrParsers::parse_root_folders(resp);
}

std::string SonarrClient::resolve_host_path(const std::string& container_path) const {
    if (container_path.empty()) return container_path;
    if (container_path.rfind(cfg_.container_library_prefix, 0) == 0) {
        return cfg_.host_library_prefix +
               container_path.substr(cfg_.container_library_prefix.size());
    }
    // No legacy alternates: unlike the movie library, the TV subtree was
    // created after the hardlink migration, so /data/library/tv is the only
    // path Sonarr has ever recorded.
    spdlog::warn("[sonarr] resolve_host_path: '{}' does not match prefix "
                 "'{}'; passing through unchanged",
                 container_path, cfg_.container_library_prefix);
    return container_path;
}

}  // namespace media_browser
