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

// --- TV list URL builder -------------------------------------------------

TEST_CASE("build_tv_list_url builds a paged TV list URL", "[tmdb][tv][url]") {
    const std::string url = mb::TmdbClient::build_tv_list_url("KEY", "/tv/popular", 3);
    CHECK(url.find("https://api.themoviedb.org/3/tv/popular") == 0);
    CHECK(url.find("api_key=KEY") != std::string::npos);
    CHECK(url.find("language=en-US") != std::string::npos);
    CHECK(url.find("page=3") != std::string::npos);
    // include_adult does NOT exist on /tv/popular, /tv/top_rated,
    // /tv/{id}/similar or /tv/{id}/recommendations. Sending it would be a
    // lie about where the family-safe gate lives (it lives in parse_tv_list).
    CHECK(url.find("include_adult") == std::string::npos);
}

TEST_CASE("build_tv_list_url works for the per-series endpoints",
          "[tmdb][tv][url]") {
    const std::string rec =
        mb::TmdbClient::build_tv_list_url("KEY", "/tv/1396/recommendations", 1);
    CHECK(rec.find("/tv/1396/recommendations") != std::string::npos);
    const std::string sim =
        mb::TmdbClient::build_tv_list_url("KEY", "/tv/1396/similar", 2);
    CHECK(sim.find("/tv/1396/similar") != std::string::npos);
    CHECK(sim.find("page=2") != std::string::npos);
}

// --- TV discover URL builder ---------------------------------------------

TEST_CASE("build_tv_discover_url uses first_air_date, never air_date",
          "[tmdb][tv][url]") {
    mb::TvDiscoverFilter f;
    f.first_air_date_year_gte = 2015;
    f.first_air_date_year_lte = 2020;

    const std::string url = mb::TmdbClient::build_tv_discover_url("KEY", f, 1);
    CHECK(url.find("/discover/tv") != std::string::npos);
    CHECK(url.find("first_air_date.gte=2015-01-01") != std::string::npos);
    CHECK(url.find("first_air_date.lte=2020-12-31") != std::string::npos);
    // air_date.* matches ANY episode's air date, which is not what the
    // filter means. The '&' prefix is load-bearing: without it the needle
    // would match inside "first_air_date.gte".
    CHECK(url.find("&air_date.gte=") == std::string::npos);
    CHECK(url.find("&air_date.lte=") == std::string::npos);
}

TEST_CASE("build_tv_discover_url joins multi-genre with pipe (OR semantics)",
          "[tmdb][tv][url]") {
    mb::TvDiscoverFilter f;
    // TV genre id space: 10759 Action & Adventure, 10765 Sci-Fi & Fantasy,
    // 18 Drama. Movie ids (28, 878) are INVALID here.
    f.genre_ids = {10759, 10765, 18};
    const std::string url = mb::TmdbClient::build_tv_discover_url("KEY", f, 1);
    CHECK(url.find("with_genres=10759%7C10765%7C18") != std::string::npos);
}

TEST_CASE("build_tv_discover_url emits include_adult=false and the rest of the "
          "filter", "[tmdb][tv][url]") {
    mb::TvDiscoverFilter f;
    f.vote_average_gte = 7.5f;
    f.vote_count_gte = 200;
    f.with_runtime_gte = 20;
    f.with_runtime_lte = 70;
    f.with_original_language = "en";
    f.sort_by = "vote_average.desc";
    const std::string url = mb::TmdbClient::build_tv_discover_url("KEY", f, 4);
    // include_adult DOES exist on /discover/tv (unlike the list endpoints).
    CHECK(url.find("include_adult=false") != std::string::npos);
    CHECK(url.find("vote_average.gte=7.5") != std::string::npos);
    CHECK(url.find("vote_count.gte=200") != std::string::npos);
    CHECK(url.find("with_runtime.gte=20") != std::string::npos);
    CHECK(url.find("with_runtime.lte=70") != std::string::npos);
    CHECK(url.find("with_original_language=en") != std::string::npos);
    CHECK(url.find("sort_by=vote_average.desc") != std::string::npos);
    CHECK(url.find("page=4") != std::string::npos);
    // /discover/tv has NO certification params (movie-only) — never emit them.
    CHECK(url.find("certification") == std::string::npos);
}

TEST_CASE("build_tv_discover_url omits unset filters", "[tmdb][tv][url]") {
    mb::TvDiscoverFilter f;  // all defaults
    const std::string url = mb::TmdbClient::build_tv_discover_url("KEY", f, 1);
    CHECK(url.find("with_genres") == std::string::npos);
    CHECK(url.find("first_air_date.gte") == std::string::npos);
    CHECK(url.find("first_air_date.lte") == std::string::npos);
    CHECK(url.find("vote_average.gte") == std::string::npos);
    CHECK(url.find("with_runtime") == std::string::npos);
    CHECK(url.find("with_original_language") == std::string::npos);
    CHECK(url.find("sort_by=popularity.desc") != std::string::npos);
}

// --- TV detail -----------------------------------------------------------

TEST_CASE("parse_tv_detail extracts the series record", "[tmdb][tv][detail]") {
    const std::string json = load_fixture("tv_detail.json");
    REQUIRE_FALSE(json.empty());
    auto d = mb::TmdbClient::parse_tv_detail(json);
    REQUIRE(d.has_value());

    CHECK(d->tmdb_id == 1396);
    CHECK(d->title == "Breaking Bad");            // from "name"
    CHECK(d->original_title == "Breaking Bad");   // from "original_name"
    CHECK(d->tagline == "Change the equation.");
    CHECK(d->first_air_date == "2008-01-20");
    CHECK(d->last_air_date == "2013-09-29");
    CHECK(d->year == 2008);
    CHECK(d->rating == Catch::Approx(8.906));
    CHECK(d->vote_count == 13500);
    CHECK(d->original_language == "en");
    CHECK(d->status == "Ended");
    CHECK_FALSE(d->in_production);
    CHECK(d->number_of_seasons == 5);
    CHECK(d->number_of_episodes == 62);
    CHECK(d->poster_path ==
          "https://image.tmdb.org/t/p/w500/ggFHVNu6YYI5L9pCfOacjizRGt.jpg");
    CHECK(d->backdrop_path ==
          "https://image.tmdb.org/t/p/w500/tsRy63Mu5cu8etL1X7ZLyf7UP1M.jpg");

    REQUIRE(d->genres.size() == 2);
    CHECK(d->genres[0] == "Drama");
    CHECK(d->genres[1] == "Crime");

    // Cast capped at 6 like the movie detail parser.
    REQUIRE(d->cast_top.size() == 6);
    CHECK(d->cast_top[0] == "Bryan Cranston");
    CHECK(d->cast_top[5] == "RJ Mitte");

    // TV's analog of "directors" is created_by.
    REQUIRE(d->creators.size() == 1);
    CHECK(d->creators[0] == "Vince Gilligan");
}

TEST_CASE("parse_tv_detail keeps Specials (season 0) in seasons[]",
          "[tmdb][tv][detail]") {
    // Sonarr's addOptions.monitor=firstSeason leaves specials UNmonitored, so
    // the UI needs to know season 0 exists to render it correctly. The parser
    // preserves TMDB's order and does not filter it out.
    auto d = mb::TmdbClient::parse_tv_detail(load_fixture("tv_detail.json"));
    REQUIRE(d.has_value());
    REQUIRE(d->seasons.size() == 3);
    CHECK(d->seasons[0].season_number == 0);
    CHECK(d->seasons[0].name == "Specials");
    CHECK(d->seasons[0].episode_count == 5);
    CHECK(d->seasons[1].season_number == 1);
    CHECK(d->seasons[1].episode_count == 7);
    CHECK(d->seasons[1].air_date == "2008-01-20");
    CHECK(d->seasons[1].poster_path ==
          "https://image.tmdb.org/t/p/w500/1BP4xYv9ZG4ZVHkL7ocOziBbSYH.jpg");
    CHECK(d->seasons[2].season_number == 2);
    CHECK(d->seasons[2].poster_path.empty());  // null poster_path
}

TEST_CASE("parse_tv_detail drops adult entries", "[tmdb][tv][detail]") {
    // /tv/{id} DOES document `adult`. Same defence-in-depth as
    // parse_movie_detail: a caller with a raw id must not land on a porn entry.
    const std::string json =
        R"({"id": 5, "name": "XXX", "first_air_date": "2020-01-01", "adult": true})";
    CHECK_FALSE(mb::TmdbClient::parse_tv_detail(json).has_value());
}

TEST_CASE("parse_tv_detail handles a minimal payload and bad JSON",
          "[tmdb][tv][detail]") {
    auto d = mb::TmdbClient::parse_tv_detail(
        R"({"id": 1, "name": "Minimal", "first_air_date": "2020-01-01"})");
    REQUIRE(d.has_value());
    CHECK(d->tagline.empty());
    CHECK(d->genres.empty());
    CHECK(d->cast_top.empty());
    CHECK(d->creators.empty());
    CHECK(d->seasons.empty());
    CHECK(d->number_of_seasons == 0);

    CHECK_FALSE(mb::TmdbClient::parse_tv_detail("not json {{{").has_value());
    // No "id" member → not a series payload.
    CHECK_FALSE(mb::TmdbClient::parse_tv_detail(R"({"success": false})").has_value());
}

// --- TV detail / genre URL builders --------------------------------------

TEST_CASE("build_tv_genres_url hits /genre/tv/list, never /genre/movie/list",
          "[tmdb][tv][url]") {
    // The entire reason get_tv_genres() exists as a separate call. A
    // one-character typo here ships a movie genre table into TV mode, where
    // ids 28/878 do not exist and 10759/10765 are missing — silently, because
    // both URLs return a well-formed {genres:[...]} the parser accepts.
    const std::string url = mb::TmdbClient::build_tv_genres_url("KEY");
    CHECK(url.find("/genre/tv/list") != std::string::npos);
    CHECK(url.find("/genre/movie/list") == std::string::npos);
    CHECK(url.find("api_key=KEY") != std::string::npos);
}

TEST_CASE("build_tv_detail_url appends credits and not content_ratings",
          "[tmdb][tv][url]") {
    const std::string url = mb::TmdbClient::build_tv_detail_url("KEY", 1396);
    CHECK(url.find("/tv/1396") != std::string::npos);
    CHECK(url.find("append_to_response=credits") != std::string::npos);
    CHECK(url.find("api_key=KEY") != std::string::npos);
    CHECK(url.find("language=en-US") != std::string::npos);
    // No certification gate is applied for TV (spec decision), so paying for
    // content_ratings would buy nothing.
    CHECK(url.find("content_ratings") == std::string::npos);
}

// --- TV genres -----------------------------------------------------------

TEST_CASE("TV genre list is a different id space from movies",
          "[tmdb][tv][genres]") {
    const std::string json = load_fixture("tv_genres.json");
    REQUIRE_FALSE(json.empty());
    // The genre payload shape is identical for movies and TV, so the existing
    // parser is reused; what must NOT be shared is the resulting table.
    auto genres = mb::TmdbClient::parse_genres_response(json);
    REQUIRE(genres.size() == 16);

    auto has_id = [&genres](int id) {
        for (const auto& g : genres) if (g.id == id) return true;
        return false;
    };
    auto name_of = [&genres](int id) -> std::string {
        for (const auto& g : genres) if (g.id == id) return g.name;
        return {};
    };

    // TV-only ids.
    CHECK(has_id(10759));
    CHECK(name_of(10759) == "Action & Adventure");
    CHECK(has_id(10765));
    CHECK(name_of(10765) == "Sci-Fi & Fantasy");
    CHECK(has_id(10768));  // War & Politics

    // Movie-only ids must be ABSENT — a shared table keyed by movie ids would
    // silently mislabel or drop TV rows.
    CHECK_FALSE(has_id(28));    // Action (movie)
    CHECK_FALSE(has_id(878));   // Science Fiction (movie)
    CHECK_FALSE(has_id(10749)); // Romance (movie)
    CHECK_FALSE(has_id(10770)); // TV Movie (movie)

    // Overlapping ids keep their names.
    CHECK(name_of(18) == "Drama");
    CHECK(name_of(16) == "Animation");
}
