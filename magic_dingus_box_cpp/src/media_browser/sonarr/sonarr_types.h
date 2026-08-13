#pragma once

#include <cstdint>
#include <string>
#include <vector>

// QualityProfile and RootFolder are Servarr-generic: Sonarr's
// /api/v3/qualityprofile and /api/v3/rootfolder payloads are the same shape
// Radarr serves, and both live in namespace media_browser — redefining them
// here would be a redefinition error, and duplicating them under new names
// would fork two identical structs. Reuse them.
#include "media_browser/radarr/radarr_types.h"

namespace media_browser {

// One season of a series. Sonarr models monitoring per season, which is what
// makes "season at a time" configuration rather than engineering.
struct Season {
    int season_number = 0;          // 0 == Specials
    bool monitored = false;
    int episode_count = 0;          // statistics.totalEpisodeCount
    int episode_file_count = 0;     // statistics.episodeFileCount
    int64_t size_on_disk_bytes = 0; // statistics.sizeOnDisk
};

// A single episode. Phase 2b only ever populates this from a queue record's
// embedded `episode` object (requested via includeEpisode=true) — there is no
// /api/v3/episode fetch until Phase 3's episode picker needs one.
struct Episode {
    int id = 0;
    int series_id = 0;
    int season_number = 0;
    int episode_number = 0;
    std::string title;
    std::string air_date;   // ISO yyyy-mm-dd; empty for unaired/unknown
};

// One record of GET /api/v3/episode?seriesId=&includeEpisodeFile=true — the
// Phase 3 episode picker's row type. Distinct from the minimal Episode above
// (queue embeds keep using that); this one carries the file facts the picker
// and playback handoff need. Live-verified against Sonarr 4.0.19:
// hasFile=false records have NO episodeFile embed and episode_file_id == 0,
// so the three file_* fields keep their empty/0 defaults; runtime may be 0
// (specials) and is stored as-is — callers fall back to the series runtime.
// episode_logic.h's templates touch ONLY season_number / episode_number /
// has_file / title (title feeds decide_end_overlay's countdown line);
// those four names are load-bearing.
struct EpisodeInfo {
    int id = 0;
    int season_number = 0;
    int episode_number = 0;
    std::string title;
    int runtime_minutes = 0;         // json `runtime`; 0 is real (specials)
    std::string air_date;            // ISO yyyy-mm-dd; empty for unaired/unknown
    bool has_file = false;
    int episode_file_id = 0;         // 0 when there is no file
    bool monitored = false;
    // episodeFile.path, container-absolute (/data/library/tv/...). Run it
    // through SonarrClient::resolve_host_path before touching the host.
    std::string file_container_path;
    long long file_size_bytes = 0;   // episodeFile.size
    std::string file_quality;        // episodeFile.quality.quality.name
};

// Season history aggregation for per-season delete operations.
struct SeasonHistory {
    std::vector<int> grabbed_history_ids;     // eventType == "grabbed"
    std::vector<int> imported_history_ids;    // eventType == "downloadFolderImported"
    std::vector<std::string> download_hashes; // distinct, lowercased
};

// Episode file metadata for download tracking.
struct EpisodeFileInfo {
    int id = 0;
    int season_number = 0;
};

// A series as returned by /api/v3/series/lookup — not yet in the library, so
// no Sonarr id and no path.
struct SeriesSearchHit {
    int tvdb_id = 0;         // Sonarr's primary key for adds; POST validates > 0
    int tmdb_id = 0;
    std::string imdb_id;
    std::string title;
    std::string overview;
    int year = 0;
    // PER-EPISODE runtime in minutes. This is the multiplicand in the
    // whole-series disk estimate (episodes x runtime x preferred MB/min).
    int runtime_minutes = 0;
    std::string status;      // "continuing" / "ended" / "upcoming"
    std::string poster_url;  // w500-normalized when TMDB-sourced
    std::string fanart_url;
    std::vector<Season> seasons;
};

// A series in the library.
struct Series : SeriesSearchHit {
    int sonarr_id = 0;
    bool monitored = false;
    // Container-internal path, e.g. "/data/library/tv/Breaking Bad".
    // Run it through SonarrClient::resolve_host_path before handing it to
    // anything on the host (GStreamer, stat()).
    std::string path;
    std::string added_at;            // ISO 8601
    int episode_file_count = 0;      // statistics.episodeFileCount
    int64_t size_on_disk_bytes = 0;  // statistics.sizeOnDisk
};

// One /api/v3/queue record. Sonarr's queue is per EPISODE: a season pack
// yields N of these sharing ONE download_id. This type is deliberately NOT
// pre-grouped — Phase 2c groups by download_id for display, and
// DELETE /api/v3/queue/{id}?removeFromClient=true acts on the whole download
// (every sibling row 404s afterwards), so the UI needs the raw rows to know
// which ids belong together.
struct SonarrQueueItem {
    int id = 0;              // queue row id — the delete key
    int series_id = 0;
    int episode_id = 0;
    int season_number = 0;
    std::string title;       // release title (identical across a pack's rows)
    int64_t size_bytes = 0;
    int64_t sizeleft_bytes = 0;
    double progress = 0.0;   // 0.0 - 1.0, derived from size/sizeleft
    int eta_seconds = 0;     // from "timeleft"
    std::string state;                   // status: queued/downloading/completed/failed
    std::string tracked_download_state;  // importBlocked / importPending / imported / ...
    // Raw casing as Sonarr emits it (uppercase hex in practice, == the qBit
    // info_hash). Consumers lowercase at comparison time, exactly as
    // QueueScreen already does for Radarr's queue.
    std::string download_id;
    Episode episode;         // populated when the request set includeEpisode=true
};

// One row of GET /api/v3/qualitydefinition — the source of the TV disk
// estimate's MB/min multiplier. Sizes are megabytes-per-minute doubles in
// Sonarr's API (minSize/maxSize/preferredSize); preferred may be null
// upstream ("unlimited"), which parses to 0 and is skipped by consumers.
struct QualityDefinition {
    int quality_id = 0;             // quality.id
    std::string title;              // quality.name, e.g. "HDTV-1080p"
    double preferred_mb_per_min = 0.0;
    double max_mb_per_min = 0.0;
};

}  // namespace media_browser
