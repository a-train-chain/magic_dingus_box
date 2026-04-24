#include "media_browser/tmdb_client.h"

#include <curl/curl.h>
#include <json/json.h>
#include <spdlog/spdlog.h>

#include <sstream>

namespace media_browser {

namespace {
constexpr const char* kApiBase = "https://api.themoviedb.org/3";
// Poster image size. w500 is ~500px wide — plenty of quality for the
// poster grid and far lighter on Pi SD-card IO / bandwidth than "original".
constexpr const char* kImageBase = "https://image.tmdb.org/t/p/w500";

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

// Converts a "/abc123.jpg" relative poster_path into a full URL. Handles
// empty paths (returns empty — the artwork cache treats that as "no art").
static std::string resolve_poster_url(const std::string& path) {
    if (path.empty()) return {};
    // TMDB consistently returns leading-slash paths; concatenating is safe.
    return std::string(kImageBase) + path;
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
    // append_to_response=credits gets cast + crew in the same HTTP round-trip,
    // so DetailScreen can show actors and directors without a second fetch.
    std::ostringstream url;
    url << kApiBase << "/movie/" << tmdb_id << "?api_key=" << api_key_
        << "&append_to_response=credits";
    auto body = http_get(url.str());
    if (body.empty()) return std::nullopt;
    return parse_movie_detail(body);
}

std::vector<TmdbSearchHit> TmdbClient::get_popular(int page) {
    std::ostringstream url;
    url << kApiBase << "/movie/popular?api_key=" << api_key_
        << "&page=" << page;
    auto body = http_get(url.str());
    if (body.empty()) return {};
    return parse_list_response(body);
}

std::vector<TmdbSearchHit> TmdbClient::get_now_playing(int page) {
    std::ostringstream url;
    url << kApiBase << "/movie/now_playing?api_key=" << api_key_
        << "&page=" << page;
    auto body = http_get(url.str());
    if (body.empty()) return {};
    return parse_list_response(body);
}

std::vector<TmdbSearchHit> TmdbClient::get_top_rated(int page) {
    std::ostringstream url;
    url << kApiBase << "/movie/top_rated?api_key=" << api_key_
        << "&page=" << page;
    auto body = http_get(url.str());
    if (body.empty()) return {};
    return parse_list_response(body);
}

std::vector<TmdbSearchHit> TmdbClient::get_upcoming(int page) {
    std::ostringstream url;
    url << kApiBase << "/movie/upcoming?api_key=" << api_key_
        << "&page=" << page;
    auto body = http_get(url.str());
    if (body.empty()) return {};
    return parse_list_response(body);
}

std::string TmdbClient::build_discover_url(const std::string& api_key,
                                           const DiscoverFilter& filter,
                                           int page) {
    std::ostringstream url;
    url << kApiBase << "/discover/movie?api_key=" << api_key
        << "&page=" << page
        << "&include_adult=false";
    if (!filter.sort_by.empty()) {
        url << "&sort_by=" << filter.sort_by;
    }
    if (filter.genre_id.has_value()) {
        url << "&with_genres=" << *filter.genre_id;
    }
    if (filter.year.has_value()) {
        url << "&primary_release_year=" << *filter.year;
    }
    return url.str();
}

std::vector<TmdbSearchHit> TmdbClient::discover(const DiscoverFilter& filter, int page) {
    auto body = http_get(build_discover_url(api_key_, filter, page));
    if (body.empty()) return {};
    return parse_list_response(body);
}

std::vector<Genre> TmdbClient::get_genres() {
    std::ostringstream url;
    url << kApiBase << "/genre/movie/list?api_key=" << api_key_;
    auto body = http_get(url.str());
    if (body.empty()) return {};
    return parse_genres_response(body);
}

std::vector<TmdbSearchHit> TmdbClient::parse_search_response(const std::string& json) {
    // Kept distinct from parse_list_response so search_movie() can preserve
    // the pre-Phase-2 behaviour (raw relative poster_path — unused callers
    // read the bare path). Tests still exercise both code paths.
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

std::vector<TmdbSearchHit> TmdbClient::parse_list_response(const std::string& json) {
    std::vector<TmdbSearchHit> hits;
    Json::CharReaderBuilder rb;
    Json::Value root;
    std::string err;
    std::istringstream is(json);
    if (!Json::parseFromStream(rb, is, &root, &err)) {
        spdlog::error("[media_browser] TMDB list parse error: {}", err);
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
        // Full URL, prefixed with the TMDB image base — this is the
        // key difference from parse_search_response: callers can hand
        // this straight to the artwork cache.
        h.poster_path = resolve_poster_url(r.get("poster_path", "").asString());
        h.year = extract_year(r.get("release_date", "").asString());
        h.rating = r.get("vote_average", 0.0).asDouble();
        hits.push_back(std::move(h));
    }
    return hits;
}

std::vector<Genre> TmdbClient::parse_genres_response(const std::string& json) {
    std::vector<Genre> genres;
    Json::CharReaderBuilder rb;
    Json::Value root;
    std::string err;
    std::istringstream is(json);
    if (!Json::parseFromStream(rb, is, &root, &err)) {
        spdlog::error("[media_browser] TMDB genres parse error: {}", err);
        return genres;
    }
    const auto& arr = root["genres"];
    if (!arr.isArray()) return genres;
    for (const auto& g : arr) {
        Genre gg;
        gg.id = g.get("id", 0).asInt();
        gg.name = g.get("name", "").asString();
        if (gg.id != 0) genres.push_back(std::move(gg));
    }
    return genres;
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
    d.tagline = root.get("tagline", "").asString();
    // Prefix poster/backdrop with the image base URL so DetailScreen and the
    // artwork cache can consume them directly — same convention as
    // parse_list_response. Empty strings stay empty (artwork cache treats
    // those as "no art" and falls back to the tint placeholder).
    d.poster_path = resolve_poster_url(root.get("poster_path", "").asString());
    d.backdrop_path = resolve_poster_url(root.get("backdrop_path", "").asString());
    d.runtime_minutes = root.get("runtime", 0).asInt();
    d.rating = root.get("vote_average", 0.0).asDouble();
    d.vote_count = root.get("vote_count", 0).asInt();
    d.release_date = root.get("release_date", "").asString();
    d.year = extract_year(d.release_date);
    d.original_language = root.get("original_language", "").asString();

    // Genres: array of {id, name}. We only need names for display.
    const auto& genres = root["genres"];
    if (genres.isArray()) {
        for (const auto& g : genres) {
            std::string name = g.get("name", "").asString();
            if (!name.empty()) d.genres.push_back(std::move(name));
        }
    }

    // credits.cast: pre-sorted by "order" (lowest first = top-billed).
    // Take up to 6 names. Each entry has both `name` (real name) and
    // `character` (role); the spec asks for `name`.
    const auto& credits = root["credits"];
    if (credits.isObject()) {
        const auto& cast = credits["cast"];
        if (cast.isArray()) {
            constexpr int kMaxCast = 6;
            int taken = 0;
            for (const auto& c : cast) {
                if (taken >= kMaxCast) break;
                std::string name = c.get("name", "").asString();
                if (!name.empty()) {
                    d.cast_top.push_back(std::move(name));
                    ++taken;
                }
            }
        }
        // credits.crew: filter to job=="Director". Most films have one,
        // but some (e.g. Wachowskis) have multiple — preserve them all.
        const auto& crew = credits["crew"];
        if (crew.isArray()) {
            for (const auto& c : crew) {
                std::string job = c.get("job", "").asString();
                if (job == "Director") {
                    std::string name = c.get("name", "").asString();
                    if (!name.empty()) d.directors.push_back(std::move(name));
                }
            }
        }
    }

    return d;
}

}  // namespace media_browser
