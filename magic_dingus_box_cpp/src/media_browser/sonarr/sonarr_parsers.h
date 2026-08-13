#pragma once

#include <optional>
#include <string>
#include <vector>
#include "media_browser/sonarr/sonarr_types.h"

namespace media_browser {

// Stateless parsers over Sonarr v4's /api/v3 payloads. Same arrangement as
// RadarrParsers: pure statics so every shape is unit-testable from a fixture
// with no network and no client instance.
class SonarrParsers {
public:
    // GET /api/v3/series/lookup?term=... — array of not-yet-added series.
    static std::vector<SeriesSearchHit> parse_series_lookup(const std::string& json);
    // GET /api/v3/series (optionally ?tvdbId=) — array of library series.
    static std::vector<Series> parse_series_list(const std::string& json);
    // GET /api/v3/series/{id} — a single library series object.
    static std::optional<Series> parse_series(const std::string& json);
    // GET /api/v3/queue — paged {records:[...]}, one record per EPISODE.
    static std::vector<SonarrQueueItem> parse_queue(const std::string& json);
    // The paged envelope's totalRecords. 0 when absent or unparseable.
    // SonarrClient::get_queue uses it to know when it has read every page.
    static int parse_queue_total(const std::string& json);
    // GET /api/v3/history/series?seriesId= — bare array (the paged
    // {records:[...]} form is also accepted). Returns distinct downloadIds,
    // lowercased for qBittorrent comparison, in first-seen order.
    static std::vector<std::string> parse_history_download_ids(const std::string& json);
    // GET /api/v3/history/series?seriesId=X&seasonNumber=N (season-scoped).
    // Input is already filtered by season on the server side.
    static SeasonHistory parse_season_history(const std::string& json);
    // GET /api/v3/episodefile?seriesId= — array of episode files with id and
    // seasonNumber for download tracking.
    static std::vector<EpisodeFileInfo> parse_episode_files(const std::string& json);
    // GET /api/v3/config/downloadclient — a single config object.
    //
    // Deliberately NOT tolerant, unlike every other parser in this file: a
    // missing/!int `id` or a missing/!bool `autoRedownloadFailed` yields
    // nullopt rather than a defaulted struct. The one consumer is
    // AutoRedownloadGuard, and both fields are load-bearing in the
    // dangerous direction — a defaulted id would PUT to the wrong path,
    // and a defaulted `false` would read as "the owner already has
    // auto-redownload off", so the guard would suppress NOTHING and
    // silently hand back the exact defect it exists to fix. nullopt makes
    // the worker abort loudly with the season still intact.
    static std::optional<DownloadClientConfig> parse_download_client_config(
        const std::string& json);
    // Servarr-identical shapes — these delegate to RadarrParsers so there is
    // exactly one implementation of each.
    static std::vector<QualityProfile> parse_quality_profiles(const std::string& json);
    static std::vector<RootFolder> parse_root_folders(const std::string& json);
    // GET /api/v3/qualitydefinition rows. Tolerant: non-array bodies and rows
    // missing the quality object yield/skip empty — the consumer treats an
    // empty vector as "use the fallback rate", never as an error state.
    static std::vector<QualityDefinition> parse_quality_definitions(
        const std::string& json);
};

}  // namespace media_browser
