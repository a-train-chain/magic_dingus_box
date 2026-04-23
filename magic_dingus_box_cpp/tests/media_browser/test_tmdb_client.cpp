#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "media_browser/tmdb_client.h"

TEST_CASE("TmdbClient::parse_search_response extracts movie hits", "[tmdb]") {
    std::string json = R"({
        "page": 1,
        "results": [
            {
                "id": 603,
                "title": "The Matrix",
                "original_title": "The Matrix",
                "release_date": "1999-03-30",
                "vote_average": 8.2,
                "overview": "A hacker learns the truth.",
                "poster_path": "/poster1.jpg"
            },
            {
                "id": 604,
                "title": "The Matrix Reloaded",
                "original_title": "The Matrix Reloaded",
                "release_date": "2003-05-15",
                "vote_average": 7.2,
                "overview": "Neo fights the machines.",
                "poster_path": "/poster2.jpg"
            }
        ],
        "total_pages": 1,
        "total_results": 2
    })";

    auto results = media_browser::TmdbClient::parse_search_response(json);
    REQUIRE(results.size() == 2);
    REQUIRE(results[0].tmdb_id == 603);
    REQUIRE(results[0].title == "The Matrix");
    REQUIRE(results[0].year == 1999);
    REQUIRE(results[0].rating == Catch::Approx(8.2));
    REQUIRE(results[1].tmdb_id == 604);
    REQUIRE(results[1].year == 2003);
}

TEST_CASE("TmdbClient::parse_movie_detail extracts full record", "[tmdb]") {
    std::string json = R"({
        "id": 603,
        "title": "The Matrix",
        "original_title": "The Matrix",
        "release_date": "1999-03-30",
        "runtime": 136,
        "vote_average": 8.2,
        "overview": "A hacker learns the truth.",
        "poster_path": "/poster.jpg",
        "backdrop_path": "/backdrop.jpg"
    })";

    auto detail = media_browser::TmdbClient::parse_movie_detail(json);
    REQUIRE(detail.has_value());
    REQUIRE(detail->tmdb_id == 603);
    REQUIRE(detail->title == "The Matrix");
    REQUIRE(detail->year == 1999);
    REQUIRE(detail->runtime_minutes == 136);
    REQUIRE(detail->poster_path == "/poster.jpg");
    REQUIRE(detail->backdrop_path == "/backdrop.jpg");
}

TEST_CASE("TmdbClient::parse_search_response handles empty results", "[tmdb]") {
    std::string json = R"({"page":1,"results":[],"total_pages":0,"total_results":0})";
    auto results = media_browser::TmdbClient::parse_search_response(json);
    REQUIRE(results.empty());
}
