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

TEST_CASE("TmdbClient::parse_movie_detail extracts rich metadata "
          "(tagline / genres / cast / directors)", "[tmdb]") {
    // Realistic TMDB payload shape for the new fields.
    // append_to_response=credits embeds a credits object alongside
    // the standard /movie/{id} response.
    std::string json = R"({
        "id": 603,
        "title": "The Matrix",
        "original_title": "The Matrix",
        "tagline": "Welcome to the Real World.",
        "release_date": "1999-03-30",
        "runtime": 136,
        "vote_average": 8.2,
        "vote_count": 25000,
        "original_language": "en",
        "overview": "A hacker learns the truth.",
        "poster_path": "/poster.jpg",
        "backdrop_path": "/backdrop.jpg",
        "genres": [
            {"id": 28, "name": "Action"},
            {"id": 878, "name": "Science Fiction"}
        ],
        "credits": {
            "cast": [
                {"order": 0, "name": "Keanu Reeves",        "character": "Neo"},
                {"order": 1, "name": "Laurence Fishburne",  "character": "Morpheus"},
                {"order": 2, "name": "Carrie-Anne Moss",    "character": "Trinity"},
                {"order": 3, "name": "Hugo Weaving",        "character": "Agent Smith"},
                {"order": 4, "name": "Joe Pantoliano",      "character": "Cypher"},
                {"order": 5, "name": "Gloria Foster",       "character": "Oracle"},
                {"order": 6, "name": "Marcus Chong",        "character": "Tank"},
                {"order": 7, "name": "Paul Goddard",        "character": "Agent Brown"}
            ],
            "crew": [
                {"job": "Director",         "name": "Lana Wachowski"},
                {"job": "Director",         "name": "Lilly Wachowski"},
                {"job": "Producer",         "name": "Joel Silver"},
                {"job": "Director of Photography", "name": "Bill Pope"}
            ]
        }
    })";

    auto detail = media_browser::TmdbClient::parse_movie_detail(json);
    REQUIRE(detail.has_value());

    // Existing core fields still populated.
    REQUIRE(detail->title == "The Matrix");
    REQUIRE(detail->year == 1999);
    REQUIRE(detail->runtime_minutes == 136);

    // New metadata.
    REQUIRE(detail->tagline == "Welcome to the Real World.");
    REQUIRE(detail->vote_count == 25000);
    REQUIRE(detail->release_date == "1999-03-30");
    REQUIRE(detail->original_language == "en");

    REQUIRE(detail->genres.size() == 2);
    REQUIRE(detail->genres[0] == "Action");
    REQUIRE(detail->genres[1] == "Science Fiction");

    // Cast capped at the top 6 (TMDB's order-sorted billing).
    REQUIRE(detail->cast_top.size() == 6);
    REQUIRE(detail->cast_top[0] == "Keanu Reeves");
    REQUIRE(detail->cast_top[5] == "Gloria Foster");

    // Both directors preserved (not just one), producer + DP filtered out.
    REQUIRE(detail->directors.size() == 2);
    REQUIRE(detail->directors[0] == "Lana Wachowski");
    REQUIRE(detail->directors[1] == "Lilly Wachowski");
}

TEST_CASE("TmdbClient::parse_movie_detail handles missing optional fields",
          "[tmdb]") {
    // Minimal payload (no tagline, no credits, no genres) must still parse
    // — the rich-metadata fields default to empty containers.
    std::string json = R"({
        "id": 1,
        "title": "Minimal",
        "release_date": "2020-01-01"
    })";
    auto detail = media_browser::TmdbClient::parse_movie_detail(json);
    REQUIRE(detail.has_value());
    REQUIRE(detail->tagline.empty());
    REQUIRE(detail->genres.empty());
    REQUIRE(detail->cast_top.empty());
    REQUIRE(detail->directors.empty());
    REQUIRE(detail->vote_count == 0);
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
    f.genre_ids = {28};
    f.primary_release_year_gte = 2024;
    f.primary_release_year_lte = 2024;
    f.sort_by = "popularity.desc";

    std::string url = media_browser::TmdbClient::build_discover_url("KEY", f, 1);
    // URL should contain every filter parameter and the api_key.
    REQUIRE(url.find("api_key=KEY") != std::string::npos);
    REQUIRE(url.find("with_genres=28") != std::string::npos);
    REQUIRE(url.find("primary_release_date.gte=2024-01-01") != std::string::npos);
    REQUIRE(url.find("primary_release_date.lte=2024-12-31") != std::string::npos);
    REQUIRE(url.find("sort_by=popularity.desc") != std::string::npos);
    REQUIRE(url.find("/discover/movie") != std::string::npos);
}

TEST_CASE("TmdbClient::build_discover_url joins multi-genre with pipe (OR semantics)", "[tmdb]") {
    media_browser::DiscoverFilter f;
    f.genre_ids = {28, 12, 18};
    std::string url = media_browser::TmdbClient::build_discover_url("KEY", f, 1);
    // %7C is the URL-encoded pipe — TMDB treats it as OR for with_genres.
    REQUIRE(url.find("with_genres=28%7C12%7C18") != std::string::npos);
}

TEST_CASE("TmdbClient::build_discover_url omits unset filters", "[tmdb]") {
    media_browser::DiscoverFilter f;  // all defaults — no genre, no year, etc.
    std::string url = media_browser::TmdbClient::build_discover_url("KEY", f, 2);
    REQUIRE(url.find("with_genres") == std::string::npos);
    REQUIRE(url.find("primary_release_date.gte") == std::string::npos);
    REQUIRE(url.find("primary_release_date.lte") == std::string::npos);
    REQUIRE(url.find("page=2") != std::string::npos);
    REQUIRE(url.find("sort_by=popularity.desc") != std::string::npos);
}

// --- has_api_key() ---------------------------------------------------
// Drives the kiosk's "no key configured" empty states (BrowseScreen's
// category grid, DetailScreen's metadata panel). Without it those two
// screens render a generic "empty"/"couldn't fetch" message that is
// indistinguishable from a genuinely empty category or a network fault,
// so a box that just needs a key in the Content Manager looks broken.
TEST_CASE("TmdbClient::has_api_key reports whether a key is configured", "[tmdb]") {
    SECTION("empty key — the unconfigured box") {
        media_browser::TmdbClient c("");
        REQUIRE_FALSE(c.has_api_key());
    }
    SECTION("non-empty key") {
        media_browser::TmdbClient c("deadbeef");
        REQUIRE(c.has_api_key());
    }
}
