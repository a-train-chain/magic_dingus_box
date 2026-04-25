#include "media_browser/radarr/radarr_client.h"
#include "media_browser/radarr/radarr_parsers.h"

#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include <cctype>
#include <cstdio>
#include <sstream>

namespace media_browser {

namespace {
static size_t curl_write_cb(void* ptr, size_t size, size_t nmemb, void* ud) {
    auto* s = static_cast<std::string*>(ud);
    s->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

std::string ensure_trailing_slash(const std::string& s) {
    if (s.empty()) return s;
    return s.back() == '/' ? s : s + "/";
}
}  // namespace

RadarrClient::RadarrClient(Config config) : cfg_(std::move(config)) {
    // Normalize prefixes so prefix matching can't fall for /library2/foo.
    cfg_.container_library_prefix =
        ensure_trailing_slash(cfg_.container_library_prefix);
    cfg_.host_library_prefix =
        ensure_trailing_slash(cfg_.host_library_prefix);

    curl_global_init(CURL_GLOBAL_DEFAULT);
}

RadarrClient::~RadarrClient() {
    curl_global_cleanup();
}

std::string RadarrClient::http_get(const std::string& path) {
    CURL* curl = curl_easy_init();
    if (!curl) { last_error_ = "curl init failed"; return {}; }
    std::string url = cfg_.base_url + path;
    std::string body;
    struct curl_slist* headers = nullptr;
    std::string auth = "X-Api-Key: " + cfg_.api_key;
    headers = curl_slist_append(headers, auth.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(cfg_.timeout_secs));
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) { last_error_ = curl_easy_strerror(rc); return {}; }
    if (http_code >= 400) {
        std::ostringstream os; os << "HTTP " << http_code;
        last_error_ = os.str();
        return {};
    }
    return body;
}

std::string RadarrClient::http_post(const std::string& path, const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) { last_error_ = "curl init failed"; return {}; }
    std::string url = cfg_.base_url + path;
    std::string resp;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("X-Api-Key: " + cfg_.api_key).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(cfg_.timeout_secs));
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) { last_error_ = curl_easy_strerror(rc); return {}; }
    if (http_code >= 400) {
        std::ostringstream os; os << "HTTP " << http_code << ": " << resp;
        last_error_ = os.str();
        return {};
    }
    return resp;
}

std::string RadarrClient::http_delete(const std::string& path) {
    CURL* curl = curl_easy_init();
    if (!curl) { last_error_ = "curl init failed"; return {}; }
    std::string url = cfg_.base_url + path;
    std::string resp;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("X-Api-Key: " + cfg_.api_key).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(cfg_.timeout_secs));
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) { last_error_ = curl_easy_strerror(rc); return {}; }
    if (http_code >= 400) {
        std::ostringstream os; os << "HTTP " << http_code;
        last_error_ = os.str();
        return {};
    }
    return resp;
}

bool RadarrClient::is_reachable() {
    return !http_get("/ping").empty();
}

std::optional<SystemStatus> RadarrClient::get_status() {
    auto resp = http_get("/api/v3/system/status");
    if (resp.empty()) return std::nullopt;
    return RadarrParsers::parse_system_status(resp);
}

std::vector<MovieSearchHit> RadarrClient::lookup(const std::string& query) {
    // URL-encode query (minimal)
    std::string encoded;
    for (char c : query) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            encoded += c;
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            encoded += buf;
        }
    }
    auto resp = http_get("/api/v3/movie/lookup?term=" + encoded);
    if (resp.empty()) return {};
    return RadarrParsers::parse_movie_lookup(resp);
}

std::vector<Movie> RadarrClient::get_library() {
    auto resp = http_get("/api/v3/movie");
    if (resp.empty()) return {};
    return RadarrParsers::parse_movie_list(resp);
}

std::optional<Movie> RadarrClient::get_movie(int radarr_id) {
    auto resp = http_get("/api/v3/movie/" + std::to_string(radarr_id));
    if (resp.empty()) return std::nullopt;
    return RadarrParsers::parse_movie(resp);
}

bool RadarrClient::add_movie(int tmdb_id, int quality_profile_id, bool monitor) {
    std::ostringstream body;
    body << R"({"tmdbId":)" << tmdb_id
         << R"(,"qualityProfileId":)" << quality_profile_id
         << R"(,"monitored":)" << (monitor ? "true" : "false")
         << R"(,"rootFolderPath":"/library/Movies")"
         << R"(,"addOptions":{"searchForMovie":)" << (monitor ? "true" : "false") << R"(}})";
    auto resp = http_post("/api/v3/movie", body.str());
    return !resp.empty();
}

bool RadarrClient::remove_movie(int radarr_id, bool delete_files) {
    std::string path = "/api/v3/movie/" + std::to_string(radarr_id)
                     + "?deleteFiles=" + (delete_files ? "true" : "false");
    last_error_.clear();
    http_delete(path);
    return last_error_.empty();
}

bool RadarrClient::trigger_search(int radarr_id) {
    std::ostringstream body;
    body << R"({"name":"MoviesSearch","movieIds":[)" << radarr_id << R"(]})";
    auto resp = http_post("/api/v3/command", body.str());
    return !resp.empty();
}

std::vector<QueueItem> RadarrClient::get_queue() {
    auto resp = http_get("/api/v3/queue?pageSize=100");
    if (resp.empty()) return {};
    return RadarrParsers::parse_queue(resp);
}

bool RadarrClient::cancel_queue_item(int queue_id) {
    last_error_.clear();
    http_delete("/api/v3/queue/" + std::to_string(queue_id)
                + "?removeFromClient=true&blocklist=false");
    return last_error_.empty();
}

std::vector<QualityProfile> RadarrClient::get_quality_profiles() {
    auto resp = http_get("/api/v3/qualityprofile");
    if (resp.empty()) return {};
    return RadarrParsers::parse_quality_profiles(resp);
}

std::vector<RootFolder> RadarrClient::get_root_folders() {
    auto resp = http_get("/api/v3/rootfolder");
    if (resp.empty()) return {};
    return RadarrParsers::parse_root_folders(resp);
}

std::string RadarrClient::resolve_host_path(const std::string& container_path) const {
    if (container_path.empty()) return container_path;
    if (container_path.rfind(cfg_.container_library_prefix, 0) == 0) {
        return cfg_.host_library_prefix +
               container_path.substr(cfg_.container_library_prefix.size());
    }
    spdlog::warn("[radarr] resolve_host_path: '{}' does not match prefix "
                 "'{}'; passing through unchanged",
                 container_path, cfg_.container_library_prefix);
    return container_path;
}

}  // namespace media_browser
