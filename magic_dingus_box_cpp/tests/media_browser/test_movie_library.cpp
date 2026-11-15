#include <catch2/catch_test_macros.hpp>
#include <json/json.h>
#include <sstream>

#include "media_browser/channels/movie_library.h"

TEST_CASE("movie library snapshot preserves exact file facts", "[movie_library]") {
    const app::LibraryMovie movie{42, "A \"Movie\"", 1979, {"Family", "Animation"},
                                  "PG", "/movies/a.mp4", 4863.25, "X264"};
    const auto bytes = app::serialize_movie_library({movie});
    Json::Value root;
    std::istringstream input(bytes);
    input >> root;
    REQUIRE(root["version"].asInt() == 1);
    REQUIRE(root["movies"].size() == 1);
    REQUIRE(root["movies"][0]["length_s"].asDouble() == 4863.25);
    const auto parsed = app::parse_movie_library(bytes);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 1);
    const auto& result = parsed->front();
    CHECK(result.tmdb_id == 42);
    CHECK(result.title == "A \"Movie\"");
    CHECK(result.year == 1979);
    CHECK(result.genres == std::vector<std::string>{"Family", "Animation"});
    CHECK(result.certification == "PG");
    CHECK(result.path == "/movies/a.mp4");
    CHECK(result.length_s == 4863.25);
    CHECK(result.codec == "X264");
    const auto empty = app::parse_movie_library(app::serialize_movie_library({}));
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
}

TEST_CASE("movie library rejects corrupt or unsupported snapshot roots", "[movie_library]") {
    for (const auto* text : {"", "broken", "[]", "null", "{\"version\":2,\"movies\":[]}",
                             "{\"version\":1,\"movies\":{}}", "{\"movies\":[]}",
                             "{\"version\":1,\"movies\":[]} trailing"}) {
        INFO(text);
        CHECK_FALSE(app::parse_movie_library(text).has_value());
    }
}

TEST_CASE("wrong typed movie fields discard only their record", "[movie_library]") {
    const std::string valid = R"({"tmdb_id":7,"title":"Keep","year":2000,"genres":["Drama"],"certification":"R","path":"/keep.mp4","length_s":90.5,"codec":"h264"})";
    Json::Value movie;
    std::istringstream(valid) >> movie;
    for (const auto* field : {"tmdb_id", "title", "year", "genres", "certification", "path", "length_s", "codec"}) {
        INFO(field);
        Json::Value bad = movie;
        bad[field] = Json::Value(Json::objectValue);
        Json::Value root;
        root["version"] = 1;
        root["movies"] = Json::Value(Json::arrayValue);
        root["movies"].append(bad);
        root["movies"].append(movie);
        const auto parsed = app::parse_movie_library(Json::writeString(Json::StreamWriterBuilder{}, root));
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->size() == 1);
        CHECK(parsed->front().title == "Keep");
    }
    Json::Value bad_genres = movie;
    bad_genres["genres"].append(5);
    const auto parsed = app::parse_movie_library("{\"version\":1,\"movies\":[null,3," +
        Json::writeString(Json::StreamWriterBuilder{}, bad_genres) + "," + valid + "]}");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 1);
}

TEST_CASE("library conversion uses imported files and resolved host paths", "[movie_library]") {
    media_browser::Movie downloaded;
    downloaded.has_file = true;
    downloaded.tmdb_id = 12;
    downloaded.title = "Imported";
    downloaded.year = 2001;
    downloaded.genres = {"Drama", "War"};
    downloaded.certification = "R";
    downloaded.file_container_path = "/library/imported.mkv";
    downloaded.file_path = "wrong-basename.mkv";
    downloaded.file_runtime_s = 123.75;
    downloaded.runtime_minutes = 999;
    downloaded.file_video_codec = "HEVC";
    media_browser::Movie pending = downloaded;
    pending.has_file = false;
    pending.tmdb_id = 99;
    const auto converted = app::to_library_movies({pending, downloaded}, [](const std::string& path) {
        return "/host" + path;
    });
    REQUIRE(converted.size() == 1);
    CHECK(converted[0].tmdb_id == 12);
    CHECK(converted[0].title == "Imported");
    CHECK(converted[0].year == 2001);
    CHECK(converted[0].genres == std::vector<std::string>{"Drama", "War"});
    CHECK(converted[0].certification == "R");
    CHECK(converted[0].path == "/host/library/imported.mkv");
    CHECK(converted[0].length_s == 123.75);
    CHECK(converted[0].codec == "HEVC");
}
