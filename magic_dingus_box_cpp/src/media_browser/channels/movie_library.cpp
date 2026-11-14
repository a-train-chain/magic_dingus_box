#include "movie_library.h"

#include <cmath>
#include <memory>
#include <json/json.h>

namespace app {

std::string serialize_movie_library(const std::vector<LibraryMovie>& movies) {
    Json::Value root(Json::objectValue);
    root["version"] = 1;
    root["movies"] = Json::Value(Json::arrayValue);
    for (const auto& movie : movies) {
        Json::Value record(Json::objectValue);
        record["tmdb_id"] = movie.tmdb_id;
        record["title"] = movie.title;
        record["year"] = movie.year;
        record["genres"] = Json::Value(Json::arrayValue);
        for (const auto& genre : movie.genres) record["genres"].append(genre);
        record["certification"] = movie.certification;
        record["path"] = movie.path;
        record["length_s"] = movie.length_s;
        record["codec"] = movie.codec;
        root["movies"].append(record);
    }
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, root);
}

std::optional<std::vector<LibraryMovie>> parse_movie_library(const std::string& json) {
    Json::CharReaderBuilder builder;
    builder["failIfExtra"] = true;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    std::string error;
    if (!reader->parse(json.data(), json.data() + json.size(), &root, &error) ||
        !root.isObject() || !root["version"].isInt() || root["version"].asInt() != 1 ||
        !root["movies"].isArray()) return std::nullopt;

    std::vector<LibraryMovie> movies;
    for (const auto& record : root["movies"]) {
        if (!record.isObject() || !record["tmdb_id"].isInt() ||
            !record["title"].isString() || !record["year"].isInt() ||
            !record["genres"].isArray() || !record["certification"].isString() ||
            !record["path"].isString() || !record["length_s"].isNumeric() ||
            !record["codec"].isString()) continue;
        LibraryMovie movie;
        bool valid_genres = true;
        for (const auto& genre : record["genres"]) {
            if (!genre.isString()) {
                valid_genres = false;
                break;
            }
            movie.genres.push_back(genre.asString());
        }
        if (!valid_genres) continue;
        movie.tmdb_id = record["tmdb_id"].asInt();
        movie.title = record["title"].asString();
        movie.year = record["year"].asInt();
        movie.certification = record["certification"].asString();
        movie.path = record["path"].asString();
        movie.length_s = record["length_s"].asDouble();
        if (!std::isfinite(movie.length_s)) continue;
        movie.codec = record["codec"].asString();
        movies.push_back(std::move(movie));
    }
    return movies;
}

std::vector<LibraryMovie> to_library_movies(
    const std::vector<media_browser::Movie>& movies,
    const std::function<std::string(const std::string&)>& resolve) {
    std::vector<LibraryMovie> result;
    for (const auto& movie : movies) {
        if (!movie.has_file) continue;
        result.push_back({movie.tmdb_id, movie.title, movie.year, movie.genres,
                          movie.certification, resolve(movie.file_container_path),
                          movie.file_runtime_s, movie.file_video_codec});
    }
    return result;
}

}  // namespace app
