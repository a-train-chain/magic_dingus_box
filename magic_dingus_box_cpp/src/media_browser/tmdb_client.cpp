#include "media_browser/tmdb_client.h"

#include <curl/curl.h>
#include <json/json.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <sstream>
#include <thread>

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

// Percent-encode an arbitrary UTF-8 string for safe embedding in a URL
// query value. RFC 3986 unreserved characters pass through unchanged;
// everything else is %HH-encoded.
//
// Why hand-rolled instead of curl_easy_escape: the prior implementation
// allocated and destroyed a CURL handle on every call, which (a) is
// surprisingly expensive for what's logically a string transformation,
// and (b) silently fell through to passing the unencoded string when
// curl_easy_init() returned nullptr — producing a malformed URL that
// TMDB would reject with a 400, indistinguishable from a network error
// for the caller. This loop has no failure mode beyond OOM and is
// branch-predictable enough that the Pi's branch predictor handles it
// without measurable overhead in the search path.
static std::string url_encode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3 / 2);  // Generous upper bound: ~50% growth.
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
    // Up to 3 attempts with backoff. Observed in production:
    //   1. TMDB calls through ProtonVPN's NL exit occasionally fail TLS
    //      handshake with CURLE_SSL_CONNECT_ERROR (35) — typically a
    //      transient VPN-egress issue or Cloudflare rate-limit on the
    //      exit IP.
    //   2. Under concurrent artwork-cache pressure (a Browse page can
    //      enqueue 18+ poster downloads while DetailScreen also fires a
    //      JSON fetch), the JSON call can stall behind in-flight image
    //      transfers and exceed the prior 10s timeout — which surfaced
    //      as "couldn't fetch movie info" toasts after exiting playback.
    // A 250ms first retry handles the SSL flake; a 1s second retry
    // gives the connection time to drain after artwork bursts.
    constexpr int kMaxAttempts = 3;
    std::string body;
    long http_code = 0;
    CURLcode rc = CURLE_OK;

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            last_error_ = "curl init failed";
            return {};
        }
        body.clear();
        http_code = 0;

        curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &body);
        curl_easy_setopt(curl, CURLOPT_USERAGENT,      "MagicDingusBox/1.0");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        // Force HTTP/1.1 — we observed TMDB's CDN occasionally negotiating
        // HTTP/2 and then failing the stream over the VPN tunnel (SSL
        // connect errors mid-handshake). HTTP/1.1 is rock-solid through
        // Gluetun's WireGuard interface and the keepalive savings of
        // HTTP/2 are irrelevant here (one HTTP request per movie open).
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                         CURL_HTTP_VERSION_1_1);
        // Required when called from non-main threads (libcurl uses
        // SIGALRM in its DNS resolver otherwise — would crash the
        // future async path with a SIGSEGV in the signal handler).
        // Harmless on the main thread; future-proofs the call site.
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);
        // Separate connect-timeout from total-timeout so a slow first
        // packet (TLS handshake) is bounded independently of the
        // overall budget. 8s connect handles a slow VPN-tunnel TLS
        // handshake; 25s total gives the request room to complete even
        // when concurrent artwork transfers are saturating the link
        // (the prior 10s budget was tripping under that load).
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,        25L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE,  1L);

        rc = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);

        if (rc == CURLE_OK && http_code < 500) break;  // success or 4xx (don't retry)

        if (attempt + 1 < kMaxAttempts) {
            spdlog::warn("[media_browser] TMDB HTTP attempt {} failed ({}), retrying",
                         attempt + 1,
                         rc != CURLE_OK ? curl_easy_strerror(rc)
                                        : ("HTTP " + std::to_string(http_code)).c_str());
            // Backoff: 250ms after first failure (handles SSL/TLS flake);
            // 1000ms after second failure (lets the link drain if it was
            // congested by concurrent transfers).
            const int backoff_ms = (attempt == 0) ? 250 : 1000;
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
        }
    }

    if (rc != CURLE_OK) {
        last_error_ = curl_easy_strerror(rc);
        spdlog::error("[media_browser] TMDB HTTP failed after retries: {}", last_error_);
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
    // include_adult=false: belt for the kiosk's family-safe stance. TMDB's
    // 'adult' field is conservative (porn only, not edgy R-rated content),
    // and parse_search_response strips any item with adult==true as a
    // suspenders — so even if a future endpoint forgets the param, the
    // parser drops anything that slips through.
    url << kApiBase << "/search/movie?api_key=" << api_key_
        << "&include_adult=false"
        << "&query=" << url_encode(query);
    auto body = http_get(url.str());
    if (body.empty()) return {};
    return parse_search_response(body);
}

std::optional<TmdbMovieDetail> TmdbClient::get_movie(int tmdb_id) {
    // append_to_response=credits gets cast + crew in the same HTTP round-trip,
    // so DetailScreen can show actors and directors without a second fetch.
    // /movie/{id} doesn't accept include_adult; instead parse_movie_detail
    // returns nullopt when the movie's adult flag is true (defense in depth
    // — a user shouldn't be able to land on Detail for an XXX entry by
    // typing a TMDB id directly into anything we add later).
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
        << "&include_adult=false"
        << "&page=" << page;
    auto body = http_get(url.str());
    if (body.empty()) return {};
    return parse_list_response(body);
}

std::vector<TmdbSearchHit> TmdbClient::get_now_playing(int page) {
    std::ostringstream url;
    url << kApiBase << "/movie/now_playing?api_key=" << api_key_
        << "&include_adult=false"
        << "&page=" << page;
    auto body = http_get(url.str());
    if (body.empty()) return {};
    return parse_list_response(body);
}

std::vector<TmdbSearchHit> TmdbClient::get_top_rated(int page) {
    std::ostringstream url;
    url << kApiBase << "/movie/top_rated?api_key=" << api_key_
        << "&include_adult=false"
        << "&page=" << page;
    auto body = http_get(url.str());
    if (body.empty()) return {};
    return parse_list_response(body);
}

std::vector<TmdbSearchHit> TmdbClient::get_upcoming(int page) {
    std::ostringstream url;
    url << kApiBase << "/movie/upcoming?api_key=" << api_key_
        << "&include_adult=false"
        << "&page=" << page;
    auto body = http_get(url.str());
    if (body.empty()) return {};
    return parse_list_response(body);
}

std::vector<TmdbSearchHit> TmdbClient::get_similar(int tmdb_id, int page) {
    std::ostringstream url;
    url << kApiBase << "/movie/" << tmdb_id << "/similar"
        << "?api_key=" << url_encode(api_key_)
        << "&language=en-US"
        << "&include_adult=false"
        << "&page=" << page;
    auto body = http_get(url.str());
    if (body.empty()) return {};
    return parse_list_response(body);
}

std::string TmdbClient::build_discover_url(const std::string& api_key,
                                           const DiscoverFilter& filter,
                                           int page) {
    std::ostringstream url;
    url << "https://api.themoviedb.org/3/discover/movie"
        << "?api_key=" << url_encode(api_key)
        << "&include_adult=false"
        << "&language=en-US"
        << "&page=" << page;
    if (!filter.sort_by.empty()) {
        url << "&sort_by=" << url_encode(filter.sort_by);
    }

    if (!filter.genre_ids.empty()) {
        url << "&with_genres=";
        for (size_t i = 0; i < filter.genre_ids.size(); ++i) {
            if (i > 0) url << "%7C";  // URL-encoded pipe → OR semantics (TMDB comma = AND)
            url << filter.genre_ids[i];
        }
    }
    if (filter.primary_release_year_gte) {
        url << "&primary_release_date.gte=" << *filter.primary_release_year_gte << "-01-01";
    }
    if (filter.primary_release_year_lte) {
        url << "&primary_release_date.lte=" << *filter.primary_release_year_lte << "-12-31";
    }
    if (filter.vote_average_gte) {
        url << "&vote_average.gte=" << *filter.vote_average_gte;
    }
    if (filter.vote_count_gte) {
        url << "&vote_count.gte=" << *filter.vote_count_gte;
    }
    if (filter.with_runtime_gte) {
        url << "&with_runtime.gte=" << *filter.with_runtime_gte;
    }
    if (filter.with_runtime_lte) {
        url << "&with_runtime.lte=" << *filter.with_runtime_lte;
    }
    if (filter.with_original_language) {
        url << "&with_original_language=" << url_encode(*filter.with_original_language);
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
        // Defense-in-depth: drop any item TMDB flags as adult, regardless
        // of the URL-side include_adult filter. Two reasons: (1) keeps the
        // parser usable on cached/saved responses where the request param
        // can't be re-asserted, (2) covers the rare case where TMDB
        // mis-tags an item that we'd want to drop anyway.
        if (r.get("adult", false).asBool()) continue;
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
        // Same family-safe drop as parse_search_response — see comment there.
        if (r.get("adult", false).asBool()) continue;
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

    // Family-safe gate: drop XXX entries even when the caller hits
    // /movie/{id} directly with a known TMDB id. The Detail screen treats
    // this as a "couldn't fetch movie" condition and shows the error
    // state, which is the right UX (the alternative — silently rendering
    // a blank Detail — would be confusing). Tied to the same `adult`
    // flag the search/list parsers check for consistency.
    if (root.get("adult", false).asBool()) {
        spdlog::warn("[media_browser] TMDB detail: dropping adult entry id={}",
                     root.get("id", 0).asInt());
        return std::nullopt;
    }

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
