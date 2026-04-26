#include "media_browser/radarr/radarr_client.h"
#include "media_browser/radarr/radarr_parsers.h"

#include <curl/curl.h>
#include <json/json.h>
#include <spdlog/spdlog.h>

#include <algorithm>
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
}  // namespace

RadarrClient::RadarrClient(Config config) : cfg_(std::move(config)) {
    // Normalize prefixes so prefix matching can't fall for /library2/foo.
    cfg_.container_library_prefix =
        RadarrClient::normalize_prefix(cfg_.container_library_prefix);
    cfg_.host_library_prefix =
        RadarrClient::normalize_prefix(cfg_.host_library_prefix);

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
    last_error_.clear();

    // Radarr v3 requires the full movie record (title, year, slug, images,
    // minimumAvailability, etc.) when POSTing to /api/v3/movie. The minimum
    // {tmdbId, qualityProfileId} payload that older Radarr versions accepted
    // now returns 400 Validation failed. Pattern: lookup the movie via TMDB
    // first to get the full object, then mutate the bits we control.
    std::string lookup_path = "/api/v3/movie/lookup?term=tmdb:"
                            + std::to_string(tmdb_id);
    std::string lookup_resp = http_get(lookup_path);
    if (lookup_resp.empty()) {
        last_error_ = "Radarr lookup failed for tmdb:" + std::to_string(tmdb_id);
        spdlog::error("[radarr] add_movie: {}", last_error_);
        return false;
    }

    Json::CharReaderBuilder rb;
    Json::Value root;
    std::string err;
    std::istringstream is(lookup_resp);
    if (!Json::parseFromStream(rb, is, &root, &err)) {
        last_error_ = "Radarr lookup parse failed: " + err;
        spdlog::error("[radarr] add_movie: {}", last_error_);
        return false;
    }

    // Lookup endpoint returns either a single object or an array depending
    // on the Radarr version. Normalize to single object.
    Json::Value movie;
    if (root.isArray()) {
        if (root.size() == 0) {
            last_error_ = "Radarr lookup returned no results";
            spdlog::error("[radarr] add_movie: {}", last_error_);
            return false;
        }
        movie = root[0u];
    } else if (root.isObject()) {
        movie = root;
    } else {
        last_error_ = "Radarr lookup returned unexpected JSON shape";
        spdlog::error("[radarr] add_movie: {}", last_error_);
        return false;
    }

    // Mutate the bits we own. Pick the first registered root folder so
    // we don't have to hardcode a path that might not exist on every
    // install.
    //
    // minimumAvailability=announced — let Radarr search/grab the moment
    // a release surfaces, regardless of TMDB's release-status string.
    // The original "released" default left users staring at "Awaiting
    // Release" forever for movies still in their theatrical window
    // (TMDB doesn't flip status to Released until digital/physical
    // home release dates land, which can lag theatrical by months —
    // verified in production with The Super Mario Galaxy Movie 2026).
    // The kiosk's quality scoring already filters out junk releases
    // (telesync, CAM, low-seeder, oversized) so flipping to
    // "announced" doesn't degrade the grab quality — it just
    // unblocks the search gate. Users who add a movie are
    // intentionally requesting it; they'd rather have a Webrip
    // grabbed today than wait three months for a Bluray.
    auto roots = get_root_folders();
    if (roots.empty()) {
        last_error_ = "No root folder configured in Radarr";
        spdlog::error("[radarr] add_movie: {}", last_error_);
        return false;
    }
    movie["qualityProfileId"]    = quality_profile_id;
    movie["rootFolderPath"]      = roots.front().path;
    movie["monitored"]           = monitor;
    movie["minimumAvailability"] = "announced";
    Json::Value addOptions;
    addOptions["searchForMovie"] = monitor;
    addOptions["monitor"]        = "movieOnly";
    addOptions["addMethod"]      = "manual";
    movie["addOptions"]          = addOptions;

    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    std::string body = Json::writeString(wb, movie);

    auto resp = http_post("/api/v3/movie", body);
    if (resp.empty()) {
        // last_error_ already set by http_post on failure.
        spdlog::error("[radarr] add_movie POST failed: {}", last_error_);
        return false;
    }
    spdlog::info("[radarr] add_movie ok: tmdb_id={} title='{}' rootFolder='{}'",
                 tmdb_id,
                 movie.get("title", "?").asString(),
                 roots.front().path);
    return true;
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

std::vector<std::string>
RadarrClient::get_movie_download_hashes(int movie_id) {
    // Radarr's /api/v3/history endpoint takes movieId as a filter and
    // returns events newest-first. We pull a generous pageSize because
    // grabbed/imported/failed events for a single movie can pile up
    // when the user re-grabs (e.g. cancelled the wrong release once
    // and re-added). 50 entries covers any realistic re-grab chain.
    auto resp = http_get("/api/v3/history?movieId="
                         + std::to_string(movie_id)
                         + "&pageSize=50");
    if (resp.empty()) return {};

    // Parse manually rather than going through RadarrParsers — we only
    // need one specific field (downloadId) and the history shape is
    // not used elsewhere, so adding a HistoryEntry type to radarr_types
    // would be over-engineering. The body is a paged result with
    // {records: [...], page, totalRecords}.
    Json::CharReaderBuilder rb;
    Json::Value root;
    std::string err;
    std::istringstream is(resp);
    if (!Json::parseFromStream(rb, is, &root, &err)) {
        last_error_ = "history parse error";
        spdlog::warn("[radarr] history parse failed for movie {}: {}",
                     movie_id, err);
        return {};
    }
    const Json::Value* records = nullptr;
    if (root.isObject() && root.isMember("records")) {
        records = &root["records"];
    } else if (root.isArray()) {
        // Older Radarr versions return a bare array.
        records = &root;
    }
    if (!records || !records->isArray()) return {};

    // Collect distinct hashes; preserve insertion order so the most-
    // recent grab gets cleaned up first (small UX win — qBit's delete
    // is essentially synchronous from our perspective so this is
    // mostly cosmetic in the spdlog output).
    std::vector<std::string> out;
    out.reserve(4);  // realistically 1-2 per movie
    for (const auto& r : *records) {
        if (!r.isObject()) continue;
        std::string id = r.get("downloadId", "").asString();
        if (id.empty()) continue;
        // Lowercase normalize — Radarr emits uppercase hex, qBit hash
        // comparison is case-insensitive in practice but we match
        // QbittorrentClient's lowercase storage convention.
        std::transform(id.begin(), id.end(), id.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (std::find(out.begin(), out.end(), id) == out.end()) {
            out.push_back(std::move(id));
        }
    }
    return out;
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

std::string RadarrClient::normalize_prefix(std::string s) {
    if (s.empty()) return s;
    if (s.back() != '/') s.push_back('/');
    return s;
}

}  // namespace media_browser
