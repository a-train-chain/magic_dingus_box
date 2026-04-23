#include "media_browser/tmdb_client.h"

#include <curl/curl.h>
#include <json/json.h>
#include <spdlog/spdlog.h>

#include <sstream>

namespace media_browser {

namespace {
constexpr const char* kApiBase = "https://api.themoviedb.org/3";

static size_t curl_write_cb(void* ptr, size_t size, size_t nmemb, void* ud) {
    auto* s = static_cast<std::string*>(ud);
    s->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

static std::string url_encode(const std::string& s) {
    CURL* c = curl_easy_init();
    char* out = curl_easy_escape(c, s.c_str(), static_cast<int>(s.length()));
    std::string result(out);
    curl_free(out);
    curl_easy_cleanup(c);
    return result;
}
}  // namespace

TmdbClient::TmdbClient(std::string api_key)
    : api_key_(std::move(api_key)) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

TmdbClient::~TmdbClient() {
    curl_global_cleanup();
}

int TmdbClient::extract_year(const std::string& date) {
    if (date.size() < 4) return 0;
    try { return std::stoi(date.substr(0, 4)); }
    catch (...) { return 0; }
}

std::string TmdbClient::http_get(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        last_error_ = "curl init failed";
        return {};
    }
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "MagicDingusBox/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        last_error_ = curl_easy_strerror(rc);
        spdlog::error("[media_browser] TMDB HTTP failed: {}", last_error_);
        return {};
    }
    if (http_code >= 400) {
        std::ostringstream os;
        os << "TMDB HTTP " << http_code;
        last_error_ = os.str();
        spdlog::error("[media_browser] {} body={}", last_error_, body);
        return {};
    }
    return body;
}

std::vector<TmdbSearchHit> TmdbClient::search_movie(const std::string& query) {
    std::ostringstream url;
    url << kApiBase << "/search/movie?api_key=" << api_key_
        << "&query=" << url_encode(query);
    auto body = http_get(url.str());
    if (body.empty()) return {};
    return parse_search_response(body);
}

std::optional<TmdbMovieDetail> TmdbClient::get_movie(int tmdb_id) {
    std::ostringstream url;
    url << kApiBase << "/movie/" << tmdb_id << "?api_key=" << api_key_;
    auto body = http_get(url.str());
    if (body.empty()) return std::nullopt;
    return parse_movie_detail(body);
}

std::vector<TmdbSearchHit> TmdbClient::parse_search_response(const std::string& json) {
    std::vector<TmdbSearchHit> hits;
    Json::CharReaderBuilder rb;
    Json::Value root;
    std::string err;
    std::istringstream is(json);
    if (!Json::parseFromStream(rb, is, &root, &err)) {
        spdlog::error("[media_browser] TMDB parse error: {}", err);
        return hits;
    }
    const auto& results = root["results"];
    if (!results.isArray()) return hits;
    for (const auto& r : results) {
        TmdbSearchHit h;
        h.tmdb_id = r.get("id", 0).asInt();
        h.title = r.get("title", "").asString();
        h.original_title = r.get("original_title", "").asString();
        h.overview = r.get("overview", "").asString();
        h.poster_path = r.get("poster_path", "").asString();
        h.year = extract_year(r.get("release_date", "").asString());
        h.rating = r.get("vote_average", 0.0).asDouble();
        hits.push_back(std::move(h));
    }
    return hits;
}

std::optional<TmdbMovieDetail> TmdbClient::parse_movie_detail(const std::string& json) {
    Json::CharReaderBuilder rb;
    Json::Value root;
    std::string err;
    std::istringstream is(json);
    if (!Json::parseFromStream(rb, is, &root, &err)) {
        spdlog::error("[media_browser] TMDB detail parse error: {}", err);
        return std::nullopt;
    }
    if (!root.isObject() || !root.isMember("id")) return std::nullopt;

    TmdbMovieDetail d;
    d.tmdb_id = root.get("id", 0).asInt();
    d.title = root.get("title", "").asString();
    d.original_title = root.get("original_title", "").asString();
    d.overview = root.get("overview", "").asString();
    d.poster_path = root.get("poster_path", "").asString();
    d.backdrop_path = root.get("backdrop_path", "").asString();
    d.runtime_minutes = root.get("runtime", 0).asInt();
    d.rating = root.get("vote_average", 0.0).asDouble();
    d.year = extract_year(root.get("release_date", "").asString());
    return d;
}

}  // namespace media_browser
