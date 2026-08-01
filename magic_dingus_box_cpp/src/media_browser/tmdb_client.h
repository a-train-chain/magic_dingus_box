#pragma once

#include <mutex>
#include <string>
#include <vector>
#include <optional>

namespace media_browser {

// Which TMDB namespace a row came from. Defaults to Movie on every hit so
// the entire pre-TV movie path (Browse, Search, For You, playback overlay)
// is untouched by the TV work — a TV row is only ever produced by the
// parse_tv_* family, which sets this explicitly.
enum class MediaKind { Movie, Tv };

struct TmdbSearchHit {
    int tmdb_id = 0;
    std::string title;
    std::string original_title;
    std::string overview;
    // Full image URL (prefix already applied — e.g. "https://image.tmdb.org/t/p/w500/abc.jpg").
    // This matches MovieSearchHit::poster_url semantics so callers can pass it directly
    // to the artwork cache without re-prefixing.
    std::string poster_path;
    int year = 0;                // extracted from release_date/first_air_date
    double rating = 0.0;         // vote_average
    MediaKind kind = MediaKind::Movie;  // see MediaKind — movie rows never set this
};

// Result of any TMDB "results[]" list endpoint. `ok` distinguishes a fetch/
// parse failure from a genuinely empty page — the bare-vector endpoints could
// not, which made stale-while-revalidate inexpressible (spec 1a′). `ok` is
// true iff the JSON parsed AND carried a results array; TMDB error payloads
// (valid JSON, no results) are NOT ok.
struct TmdbList {
    bool ok = false;
    int total_pages = 0;   // TMDB total_pages; 0 when absent.
    std::vector<TmdbSearchHit> hits;
};

struct TmdbMovieDetail {
    int tmdb_id = 0;
    std::string title;
    std::string original_title;
    std::string overview;
    std::string tagline;            // Marketing tagline (e.g. "Free your mind.")
    // Full image URL (w500 prefix already applied — same convention as
    // TmdbSearchHit::poster_path). Pass directly to the artwork cache.
    std::string poster_path;
    std::string backdrop_path;
    int year = 0;
    int runtime_minutes = 0;
    double rating = 0.0;
    int vote_count = 0;             // Number of TMDB user ratings.
    std::string release_date;       // ISO yyyy-mm-dd (full date, not just year).
    std::string original_language;  // ISO 639-1 2-letter code (en, fr, ja, ...).
    std::vector<std::string> genres;     // Display names, in TMDB order.
    std::vector<std::string> cast_top;   // Up to 6 actor names from credits.cast.
    std::vector<std::string> directors;  // Names from credits.crew where job=="Director".
};

// Inline filter used for /discover/movie queries.
struct DiscoverFilter {
    std::vector<int> genre_ids;                    // multi-select with OR semantics (URL-emitted as with_genres=28|12 → films matching any genre)
    std::optional<int> primary_release_year_gte;   // formatted "YYYY-01-01" in URL
    std::optional<int> primary_release_year_lte;   // formatted "YYYY-12-31" in URL
    std::optional<float> vote_average_gte;
    std::optional<int> vote_count_gte;
    std::optional<int> with_runtime_gte;
    std::optional<int> with_runtime_lte;
    std::optional<std::string> with_original_language;  // ISO 639-1
    std::string sort_by = "popularity.desc";
};

// Inline filter used for /discover/tv queries. Deliberately a separate type
// from DiscoverFilter, not a shared one: the date params differ
// (first_air_date.* vs primary_release_date.*) and — the sharp edge — the
// genre id spaces are DIFFERENT. TV has 16 genres; 10759 "Action & Adventure"
// and 10765 "Sci-Fi & Fantasy" replace the movie ids 28/12 and 878/14, and
// 11 movie ids (28, 12, 14, 27, 36, 53, 878, 10402, 10749, 10752, 10770) are
// invalid for TV. A shared struct would invite a caller to carry movie ids
// into a TV query and silently get an empty grid.
struct TvDiscoverFilter {
    std::vector<int> genre_ids;                     // TV ids only (see /genre/tv/list); URL-emitted as with_genres=18%7C80 → OR
    std::optional<int> first_air_date_year_gte;     // formatted "YYYY-01-01" in URL
    std::optional<int> first_air_date_year_lte;     // formatted "YYYY-12-31" in URL
    std::optional<float> vote_average_gte;
    std::optional<int> vote_count_gte;
    std::optional<int> with_runtime_gte;            // per-episode minutes
    std::optional<int> with_runtime_lte;
    std::optional<std::string> with_original_language;  // ISO 639-1
    std::string sort_by = "popularity.desc";        // popularity|vote_average|vote_count|first_air_date|name .asc/.desc
};

// A TMDB movie genre — id + display name.
struct Genre {
    int id = 0;
    std::string name;
};

// Low-dependency HTTP client for The Movie Database (TMDB) v3 API.
// Uses libcurl for HTTP and jsoncpp for parsing. Auth key is required.
class TmdbClient {
public:
    explicit TmdbClient(std::string api_key);
    ~TmdbClient();

    // Search (query-based).
    std::vector<TmdbSearchHit> search_movie(const std::string& query);

    // Movie detail (by id).
    std::optional<TmdbMovieDetail> get_movie(int tmdb_id);

    // Category endpoints — TMDB's canonical discovery surfaces.
    TmdbList get_popular(int page = 1);
    TmdbList get_now_playing(int page = 1);
    TmdbList get_top_rated(int page = 1);
    TmdbList get_upcoming(int page = 1);

    // Discover (free-form filter).
    TmdbList discover(const DiscoverFilter& filter, int page = 1);

    // Similar movies (by id) — used by Marquee playback overlay.
    TmdbList get_similar(int tmdb_id, int page = 1);

    // Recommendations for a movie (by id) — algorithmic mix of similar + trending.
    // Generally gives better suggestions than get_similar; callers should fall
    // back to get_similar when this returns empty hits.
    TmdbList get_recommendations(int tmdb_id, int page = 1);

    // --- TV -------------------------------------------------------------
    // Same TmdbList shape as the movie endpoints; hits come back tagged
    // kind == MediaKind::Tv. None of these four accepts include_adult (it
    // exists only on /search/tv and /discover/tv), so parse_tv_list is the
    // family-safe gate — see its comment.
    TmdbList get_tv_popular(int page = 1);
    TmdbList get_tv_top_rated(int page = 1);
    TmdbList get_tv_recommendations(int tmdb_id, int page = 1);
    TmdbList get_tv_similar(int tmdb_id, int page = 1);

    // /discover/tv. Note there are NO certification params for TV (movie-only),
    // so a rating-based pre-filter is impossible server-side — the spec's
    // decision is no TV certification gate at all.
    TmdbList discover_tv(const TvDiscoverFilter& filter, int page = 1);

    // Genre list (for the Filter UI — cache client-side; changes rarely).
    std::vector<Genre> get_genres();

    // Pure parsers — exposed static for unit testability without network.
    static std::vector<TmdbSearchHit> parse_search_response(const std::string& json);
    static std::optional<TmdbMovieDetail> parse_movie_detail(const std::string& json);
    // Shared parser for any TMDB "results[]" list-response shape
    // (popular, now_playing, top_rated, upcoming, discover/movie).
    // Populates `poster_path` with the full image URL (w500) so callers
    // can pass it straight to the artwork cache.
    static std::vector<TmdbSearchHit> parse_list_response(const std::string& json);
    static std::vector<Genre> parse_genres_response(const std::string& json);

    // TmdbList-shaped variant of parse_list_response — same row handling
    // (family-safe drop, w500 poster prefix) plus ok/total_pages. The old
    // vector-shaped parser stays for its existing tests and callers.
    static TmdbList parse_list(const std::string& json);

    // TV-shaped variant of parse_list. TMDB's TV rows name their fields
    // differently (name / original_name / first_air_date instead of
    // title / original_title / release_date) and OMIT `adult` entirely on
    // /tv/popular and /tv/top_rated — so `adult` is read as
    // optional-default-false and only true rows are dropped. Every hit
    // comes back tagged kind == MediaKind::Tv.
    static TmdbList parse_tv_list(const std::string& json);

    // URL builders — exposed so unit tests can verify query-string construction
    // without a network round-trip.
    static std::string build_discover_url(const std::string& api_key,
                                          const DiscoverFilter& filter,
                                          int page);

    // Shared builder for the four paged TV list endpoints. `endpoint_path`
    // is the API-relative path with a leading slash, e.g. "/tv/popular" or
    // "/tv/1396/recommendations".
    static std::string build_tv_list_url(const std::string& api_key,
                                         const std::string& endpoint_path,
                                         int page);

    static std::string build_tv_discover_url(const std::string& api_key,
                                             const TvDiscoverFilter& filter,
                                             int page);

    // Testing / diagnostics. Copy under the error mutex — screens read
    // this on the render thread while their workers run client calls
    // that write it (same race the Radarr client had).
    std::string last_error() const {
        std::lock_guard<std::mutex> lk(err_mtx_);
        return last_error_;
    }

    // True when a key was supplied at construction. Every endpoint here
    // 401s without one, so a keyless box gets zero results from all of
    // them — which is indistinguishable, at the UI layer, from a
    // genuinely empty category or a network fault. Screens branch on
    // this to say "no key configured" instead of "nothing here", and
    // point the operator at the Content Manager's Media Browser tab.
    // main.cpp logs the same condition at startup.
    bool has_api_key() const { return !api_key_.empty(); }

private:
    std::string http_get(const std::string& url);

    void set_error(std::string msg) {
        std::lock_guard<std::mutex> lk(err_mtx_);
        last_error_ = std::move(msg);
    }

    std::string api_key_;
    mutable std::mutex err_mtx_;
    std::string last_error_;  // guarded by err_mtx_ — set_error()/last_error()
};

}  // namespace media_browser
