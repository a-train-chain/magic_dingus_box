#pragma once

#include <string>
#include <vector>
#include <optional>

namespace media_browser {

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
    std::vector<TmdbSearchHit> get_popular(int page = 1);
    std::vector<TmdbSearchHit> get_now_playing(int page = 1);
    std::vector<TmdbSearchHit> get_top_rated(int page = 1);
    std::vector<TmdbSearchHit> get_upcoming(int page = 1);

    // Discover (free-form filter).
    std::vector<TmdbSearchHit> discover(const DiscoverFilter& filter, int page = 1);

    // Similar movies (by id) — used by Marquee playback overlay.
    std::vector<TmdbSearchHit> get_similar(int tmdb_id, int page = 1);

    // Recommendations for a movie (by id) — algorithmic mix of similar + trending.
    // Generally gives better suggestions than get_similar; callers should fall
    // back to get_similar when this returns empty.
    std::vector<TmdbSearchHit> get_recommendations(int tmdb_id, int page = 1);

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

    // URL builders — exposed so unit tests can verify query-string construction
    // without a network round-trip.
    static std::string build_discover_url(const std::string& api_key,
                                          const DiscoverFilter& filter,
                                          int page);

    // Testing / diagnostics.
    const std::string& last_error() const { return last_error_; }

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
    static int extract_year(const std::string& date_yyyy_mm_dd);

    std::string api_key_;
    std::string last_error_;
};

}  // namespace media_browser
