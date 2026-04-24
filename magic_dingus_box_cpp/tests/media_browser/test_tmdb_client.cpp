#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "media_browser/tmdb_client.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
// Fixture loader — resolves relative to the test binary's source tree so it
// works regardless of what CWD the runner uses.
std::string load_fixture(const std::string& name) {
    // __FILE__ expands to the absolute path of this translation unit at compile
    // time; walk up to the fixtures directory.
    std::filesystem::path here = std::filesystem::path(__FILE__).parent_path();
    auto path = here / "fixtures" / "tmdb" / name;
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream os;
    os << f.rdbuf();
    return os.str();
}
}  // namespace

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
    REQUIRE(detail->poster_path == "https://image.tmdb.org/t/p/w500/poster.jpg");
    REQUIRE(detail->backdrop_path == "https://image.tmdb.org/t/p/w500/backdrop.jpg");
}

TEST_CASE("TmdbClient::parse_search_response handles empty results", "[tmdb]") {
    std::string json = R"({"page":1,"results":[],"total_pages":0,"total_results":0})";
    auto results = media_browser::TmdbClient::parse_search_response(json);
    REQUIRE(results.empty());
}

TEST_CASE("TmdbClient::parse_list_response parses popular.json fixture", "[tmdb]") {
    auto body = load_fixture("popular.json");
    REQUIRE_FALSE(body.empty());

    auto results = media_browser::TmdbClient::parse_list_response(body);
    REQUIRE(results.size() == 3);

    REQUIRE(results[0].tmdb_id == 533535);
    REQUIRE(results[0].title == "Deadpool & Wolverine");
    REQUIRE(results[0].year == 2024);
    REQUIRE(results[0].rating == Catch::Approx(7.7));
    // poster_path should be the full image URL (w500), not a bare path.
    REQUIRE(results[0].poster_path ==
            "https://image.tmdb.org/t/p/w500/8cdWjvZQUExUUTzyp4t6EDMubfO.jpg");

    REQUIRE(results[1].title == "Inside Out 2");
    REQUIRE(results[2].title == "Despicable Me 4");
}

TEST_CASE("TmdbClient::parse_list_response handles missing poster_path", "[tmdb]") {
    // TMDB occasionally ships records with "poster_path": null — the parser
    // must not crash and must leave the hit's URL empty (which the artwork
    // cache will treat as "no art" and fall back to the tint placeholder).
    std::string json = R"({
        "results": [
            {"id": 1, "title": "No Poster", "release_date": "2020-01-01"}
        ]
    })";
    auto results = media_browser::TmdbClient::parse_list_response(json);
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].poster_path.empty());
}

TEST_CASE("TmdbClient::parse_genres_response parses genres.json fixture", "[tmdb]") {
    auto body = load_fixture("genres.json");
    REQUIRE_FALSE(body.empty());

    auto genres = media_browser::TmdbClient::parse_genres_response(body);
    REQUIRE(genres.size() == 10);
    REQUIRE(genres[0].id == 28);
    REQUIRE(genres[0].name == "Action");
    REQUIRE(genres[9].id == 878);
    REQUIRE(genres[9].name == "Science Fiction");
}

TEST_CASE("TmdbClient::parse_genres_response on empty array", "[tmdb]") {
    auto genres = media_browser::TmdbClient::parse_genres_response(R"({"genres":[]})");
    REQUIRE(genres.empty());
}

TEST_CASE("TmdbClient::build_discover_url encodes filter params", "[tmdb]") {
    media_browser::DiscoverFilter f;
    f.genre_id = 28;
    f.year = 2024;
    f.sort_by = "popularity.desc";

    std::string url = media_browser::TmdbClient::build_discover_url("KEY", f, 1);
    // URL should contain every filter parameter and the api_key.
    REQUIRE(url.find("api_key=KEY") != std::string::npos);
    REQUIRE(url.find("with_genres=28") != std::string::npos);
    REQUIRE(url.find("primary_release_year=2024") != std::string::npos);
    REQUIRE(url.find("sort_by=popularity.desc") != std::string::npos);
    REQUIRE(url.find("/discover/movie") != std::string::npos);
}

TEST_CASE("TmdbClient::build_discover_url omits unset filters", "[tmdb]") {
    media_browser::DiscoverFilter f;  // all defaults — no genre, no year.
    std::string url = media_browser::TmdbClient::build_discover_url("KEY", f, 2);
    REQUIRE(url.find("with_genres") == std::string::npos);
    REQUIRE(url.find("primary_release_year") == std::string::npos);
    REQUIRE(url.find("page=2") != std::string::npos);
    REQUIRE(url.find("sort_by=popularity.desc") != std::string::npos);
}
