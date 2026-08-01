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
    static ActiveSearches parse_active_searches(const std::string& json);

    // Rewrites the "/t/p/<size>/" segment of a TMDB image URL to w500;
    // non-TMDB URLs (TVDB, fanart.tv) pass through unchanged. Public because
    // SonarrParsers needs the identical behaviour — Sonarr serves a mix of
    // TMDB and TVDB artwork and the 256MB artwork-cache budget applies to
    // both libraries. Was a file-local helper until Phase 2b.
    static std::string normalize_tmdb_poster_url(const std::string& url);
};

}  // namespace media_browser
