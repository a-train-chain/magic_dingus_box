#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "media_browser/tmdb_client.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
// Same convention as test_tmdb_client.cpp: __FILE__ resolves the fixture dir
// regardless of the runner's CWD.
std::string load_fixture(const std::string& name) {
    std::filesystem::path here = std::filesystem::path(__FILE__).parent_path();
    std::ifstream f(here / "fixtures" / "tmdb" / name);
    if (!f) return {};
    std::ostringstream os;
    os << f.rdbuf();
    return os.str();
}
}  // namespace

namespace mb = media_browser;

TEST_CASE("parse_tv_list maps name/first_air_date and tags kind=Tv", "[tmdb][tv]") {
    const std::string json = load_fixture("tv_popular.json");
    REQUIRE_FALSE(json.empty());
    auto list = mb::TmdbClient::parse_tv_list(json);

    REQUIRE(list.ok);
    CHECK(list.total_pages == 500);
    // 3 rows in the fixture, 1 dropped for adult:true.
    REQUIRE(list.hits.size() == 2);

    CHECK(list.hits[0].tmdb_id == 1396);
    CHECK(list.hits[0].title == "Breaking Bad");           // from "name"
    CHECK(list.hits[0].original_title == "Breaking Bad");  // from "original_name"
    CHECK(list.hits[0].year == 2008);                      // from "first_air_date"
    CHECK(list.hits[0].rating == Catch::Approx(8.9));
    CHECK(list.hits[0].poster_path ==
          "https://image.tmdb.org/t/p/w500/ggFHVNu6YYI5L9pCfOacjizRGt.jpg");
    CHECK(list.hits[0].kind == mb::MediaKind::Tv);
    CHECK(list.hits[1].kind == mb::MediaKind::Tv);
}

TEST_CASE("parse_tv_list treats a missing adult key as false", "[tmdb][tv]") {
    // /tv/popular and /tv/top_rated rows carry NO adult field at all. If the
    // parser required the key it would drop every popular show on the box.
    const std::string json = R"({
        "page": 1, "total_pages": 1,
        "results": [
            {"id": 1396, "name": "Breaking Bad", "original_name": "Breaking Bad",
             "first_air_date": "2008-01-20", "vote_average": 8.9,
             "poster_path": "/bb.jpg", "overview": "x"}
        ]
    })";
    auto list = mb::TmdbClient::parse_tv_list(json);
    REQUIRE(list.ok);
    REQUIRE(list.hits.size() == 1);
    CHECK(list.hits[0].tmdb_id == 1396);
}

TEST_CASE("parse_tv_list drops adult rows when the flag IS present", "[tmdb][tv]") {
    // /search/tv, /tv/{id}/similar and /tv/{id}/recommendations DO carry adult.
    const std::string json = R"({
        "page": 1, "total_pages": 1,
        "results": [
            {"id": 1, "name": "Keep", "first_air_date": "2020-01-01", "adult": false},
            {"id": 2, "name": "Drop", "first_air_date": "2020-01-01", "adult": true}
        ]
    })";
    auto list = mb::TmdbClient::parse_tv_list(json);
    REQUIRE(list.ok);
    REQUIRE(list.hits.size() == 1);
    CHECK(list.hits[0].title == "Keep");
}

TEST_CASE("parse_tv_list tolerates null poster_path and float vote_average",
          "[tmdb][tv]") {
    const std::string json = load_fixture("tv_popular.json");
    auto list = mb::TmdbClient::parse_tv_list(json);
    REQUIRE(list.hits.size() == 2);
    CHECK(list.hits[1].tmdb_id == 1399);
    CHECK(list.hits[1].poster_path.empty());          // null → "no art"
    CHECK(list.hits[1].rating == Catch::Approx(8.456));  // NOT truncated to 8
}

TEST_CASE("parse_tv_list flags malformed and error payloads", "[tmdb][tv]") {
    CHECK_FALSE(mb::TmdbClient::parse_tv_list("not json {{{").ok);
    CHECK_FALSE(mb::TmdbClient::parse_tv_list(
        R"({"status_code": 34, "status_message": "not found"})").ok);
    auto empty = mb::TmdbClient::parse_tv_list(
        R"({"page": 1, "total_pages": 1, "results": []})");
    CHECK(empty.ok);
    CHECK(empty.hits.empty());
}

TEST_CASE("movie parse_list still defaults kind to Movie", "[tmdb][tv]") {
    // Regression guard for every existing movie caller: nothing in the movie
    // path sets kind, so the default must be Movie.
    const std::string json = R"({
        "page": 1, "total_pages": 3,
        "results": [{"id": 603, "title": "The Matrix", "release_date": "1999-03-30",
                     "vote_average": 8.2, "poster_path": "/m.jpg"}]
    })";
    auto list = mb::TmdbClient::parse_list(json);
    REQUIRE(list.hits.size() == 1);
    CHECK(list.hits[0].kind == mb::MediaKind::Movie);
    CHECK(list.hits[0].title == "The Matrix");
    CHECK(list.hits[0].year == 1999);
}
