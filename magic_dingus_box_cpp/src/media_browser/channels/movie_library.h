#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "media_browser/radarr/radarr_types.h"

namespace app {

struct LibraryMovie {
    int tmdb_id = 0;
    std::string title;
    int year = 0;
    std::vector<std::string> genres;
    std::string certification;
    std::string path;  // absolute host path
    double length_s = 0;  // exact imported-file duration
    std::string codec;
};

// Version 1 snapshots contain every field; malformed records are skipped,
// while corrupt text or an unsupported root returns nullopt. No file I/O.
std::string serialize_movie_library(const std::vector<LibraryMovie>& movies);
std::optional<std::vector<LibraryMovie>> parse_movie_library(const std::string& json);
std::vector<LibraryMovie> to_library_movies(
    const std::vector<media_browser::Movie>& movies,
    const std::function<std::string(const std::string&)>& resolve);

}  // namespace app
