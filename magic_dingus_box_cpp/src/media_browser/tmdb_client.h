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
    std::string poster_path;     // relative path, prepend image base URL
    int year = 0;                // extracted from release_date/first_air_date
    double rating = 0.0;         // vote_average
};

struct TmdbMovieDetail {
    int tmdb_id = 0;
    std::string title;
    std::string original_title;
    std::string overview;
    std::string poster_path;
    std::string backdrop_path;
    int year = 0;
    int runtime_minutes = 0;
    double rating = 0.0;
};

// Low-dependency HTTP client for The Movie Database (TMDB) v3 API.
// Uses libcurl for HTTP and jsoncpp for parsing. Auth key is required.
class TmdbClient {
public:
    explicit TmdbClient(std::string api_key);
    ~TmdbClient();

    // HTTP methods (network required).
    std::vector<TmdbSearchHit> search_movie(const std::string& query);
    std::optional<TmdbMovieDetail> get_movie(int tmdb_id);

    // Pure parsers — exposed static for unit testability without network.
    static std::vector<TmdbSearchHit> parse_search_response(const std::string& json);
    static std::optional<TmdbMovieDetail> parse_movie_detail(const std::string& json);

    // Testing / diagnostics.
    const std::string& last_error() const { return last_error_; }

private:
    std::string http_get(const std::string& url);
    static int extract_year(const std::string& date_yyyy_mm_dd);

    std::string api_key_;
    std::string last_error_;
};

}  // namespace media_browser
