#pragma once

#include <string>
#include <vector>
#include <optional>
#include "media_browser/radarr/radarr_types.h"

namespace media_browser {

class RadarrParsers {
public:
    static std::vector<MovieSearchHit> parse_movie_lookup(const std::string& json);
    static std::vector<Movie> parse_movie_list(const std::string& json);
    static std::optional<Movie> parse_movie(const std::string& json);
    static std::vector<QueueItem> parse_queue(const std::string& json);
    static std::vector<QualityProfile> parse_quality_profiles(const std::string& json);
    static std::vector<RootFolder> parse_root_folders(const std::string& json);
    static std::optional<SystemStatus> parse_system_status(const std::string& json);
};

}  // namespace media_browser
