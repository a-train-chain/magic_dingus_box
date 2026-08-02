#include "media_browser/sonarr/sonarr_parsers.h"
#include "media_browser/radarr/radarr_parsers.h"

#include <json/json.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <string>

namespace media_browser {

namespace {

bool parse_json(const std::string& text, Json::Value& out) {
    Json::CharReaderBuilder rb;
    std::string err;
    std::istringstream is(text);
    return Json::parseFromStream(rb, is, &out, &err);
}

std::string pick_image(const Json::Value& images, const std::string& coverType) {
    if (!images.isArray()) return "";
    for (const auto& img : images) {
        if (img["coverType"].asString() == coverType) {
            // Sonarr mixes TMDB and TVDB/fanart.tv artwork. The TMDB ones get
            // downsized to w500 (shared artwork-cache key + the 256MB budget);
            // the rest pass through untouched.
            return RadarrParsers::normalize_tmdb_poster_url(
                img["remoteUrl"].asString());
        }
    }
    return "";
}

// "HH:MM:SS" and "D.HH:MM:SS" → total seconds; 0 on failure. Same format
// Sonarr uses for queue timeleft as Radarr does.
int parse_timeleft_to_seconds(const std::string& s) {
    if (s.empty()) return 0;
    int days = 0;
    size_t time_start = 0;
    size_t dot = s.find('.');
    size_t first_colon = s.find(':');
    if (dot != std::string::npos && first_colon != std::string::npos &&
        dot < first_colon) {
        try {
            days = std::stoi(s.substr(0, dot));
        } catch (...) {
            return 0;
        }
        time_start = dot + 1;
    }
    const std::string t = s.substr(time_start);
    int hh = 0, mm = 0, ss = 0;
    if (std::sscanf(t.c_str(), "%d:%d:%d", &hh, &mm, &ss) != 3) return 0;
    if (hh < 0 || mm < 0 || ss < 0) return 0;
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

void fill_seasons(const Json::Value& r, std::vector<Season>& out) {
    const auto& seasons = r["seasons"];
    if (!seasons.isArray()) return;
    for (const auto& s : seasons) {
        Season season;
        season.season_number = s.get("seasonNumber", 0).asInt();
        season.monitored     = s.get("monitored", false).asBool();
        const auto& st = s["statistics"];
        if (st.isObject()) {
            season.episode_count      = st.get("totalEpisodeCount", 0).asInt();
            season.episode_file_count = st.get("episodeFileCount", 0).asInt();
            season.size_on_disk_bytes = st.get("sizeOnDisk", 0).asInt64();
        }
        out.push_back(std::move(season));
    }
}

void fill_search_hit(const Json::Value& r, SeriesSearchHit& h) {
    h.tvdb_id         = r.get("tvdbId", 0).asInt();
    h.tmdb_id         = r.get("tmdbId", 0).asInt();
    h.imdb_id         = r.get("imdbId", "").asString();
    h.title           = r.get("title", "").asString();
    h.overview        = r.get("overview", "").asString();
    h.year            = r.get("year", 0).asInt();
    h.runtime_minutes = r.get("runtime", 0).asInt();
    h.status          = r.get("status", "").asString();
    h.poster_url      = pick_image(r["images"], "poster");
    h.fanart_url      = pick_image(r["images"], "fanart");
    fill_seasons(r, h.seasons);
}

void fill_library_fields(const Json::Value& r, Series& s) {
    s.sonarr_id = r.get("id", 0).asInt();
    s.monitored = r.get("monitored", false).asBool();
    s.path      = r.get("path", "").asString();
    s.added_at  = r.get("added", "").asString();
    const auto& st = r["statistics"];
    if (st.isObject()) {
        s.episode_file_count = st.get("episodeFileCount", 0).asInt();
        s.size_on_disk_bytes = st.get("sizeOnDisk", 0).asInt64();
    }
}

// Both the bare-array (/history/series) and paged (/history) shapes.
const Json::Value* records_of(const Json::Value& root) {
    if (root.isArray()) return &root;
    if (root.isObject() && root.isMember("records") && root["records"].isArray()) {
        return &root["records"];
    }
    return nullptr;
}

}  // namespace

std::vector<SeriesSearchHit> SonarrParsers::parse_series_lookup(const std::string& json) {
    std::vector<SeriesSearchHit> out;
    Json::Value root;
    if (!parse_json(json, root) || !root.isArray()) return out;
    for (const auto& r : root) {
        SeriesSearchHit h;
        fill_search_hit(r, h);
        out.push_back(std::move(h));
    }
    return out;
}

std::vector<Series> SonarrParsers::parse_series_list(const std::string& json) {
    std::vector<Series> out;
    Json::Value root;
    if (!parse_json(json, root) || !root.isArray()) return out;
    for (const auto& r : root) {
        Series s;
        fill_search_hit(r, s);
        fill_library_fields(r, s);
        out.push_back(std::move(s));
    }
    return out;
}

std::optional<Series> SonarrParsers::parse_series(const std::string& json) {
    Json::Value root;
    if (!parse_json(json, root) || !root.isObject()) return std::nullopt;
    Series s;
    fill_search_hit(root, s);
    fill_library_fields(root, s);
    return s;
}

std::vector<SonarrQueueItem> SonarrParsers::parse_queue(const std::string& json) {
    std::vector<SonarrQueueItem> out;
    Json::Value root;
    if (!parse_json(json, root)) return out;
    const auto& records = root["records"];
    if (!records.isArray()) return out;
    for (const auto& r : records) {
        SonarrQueueItem q;
        q.id            = r.get("id", 0).asInt();
        q.series_id     = r.get("seriesId", 0).asInt();
        q.episode_id    = r.get("episodeId", 0).asInt();
        q.season_number = r.get("seasonNumber", 0).asInt();
        q.title         = r.get("title", "").asString();
        q.size_bytes    = r.get("size", 0).asInt64();
        // 'sizeleft' is what serializes today; 'sizeLeft' is staged upstream
        // (the replacement property is committed but commented out). Accept
        // both so a Sonarr bump cannot silently zero every progress bar.
        q.sizeleft_bytes = r.isMember("sizeleft")
                             ? r.get("sizeleft", 0).asInt64()
                             : r.get("sizeLeft", 0).asInt64();
        q.state          = r.get("status", "").asString();
        q.tracked_download_state = r.get("trackedDownloadState", "").asString();
        q.download_id    = r.get("downloadId", "").asString();
        if (q.size_bytes > 0) {
            q.progress = static_cast<double>(q.size_bytes - q.sizeleft_bytes)
                         / static_cast<double>(q.size_bytes);
        }
        if (r.isMember("timeleft")) {
            q.eta_seconds = parse_timeleft_to_seconds(r["timeleft"].asString());
        }
        const auto& ep = r["episode"];
        if (ep.isObject()) {
            q.episode.id             = ep.get("id", 0).asInt();
            q.episode.series_id      = ep.get("seriesId", 0).asInt();
            q.episode.season_number  = ep.get("seasonNumber", 0).asInt();
            q.episode.episode_number = ep.get("episodeNumber", 0).asInt();
            q.episode.title          = ep.get("title", "").asString();
            q.episode.air_date       = ep.get("airDate", "").asString();
        }
        out.push_back(std::move(q));
    }
    return out;
}

int SonarrParsers::parse_queue_total(const std::string& json) {
    Json::Value root;
    if (!parse_json(json, root) || !root.isObject()) return 0;
    return root.get("totalRecords", 0).asInt();
}

std::vector<std::string>
SonarrParsers::parse_history_download_ids(const std::string& json) {
    std::vector<std::string> out;
    Json::Value root;
    if (!parse_json(json, root)) return out;
    const Json::Value* records = records_of(root);
    if (!records) return out;
    out.reserve(4);
    for (const auto& r : *records) {
        if (!r.isObject()) continue;
        std::string id = to_lower(r.get("downloadId", "").asString());
        if (id.empty()) continue;
        if (std::find(out.begin(), out.end(), id) == out.end()) {
            out.push_back(std::move(id));
        }
    }
    return out;
}

std::vector<QualityProfile> SonarrParsers::parse_quality_profiles(const std::string& json) {
    return RadarrParsers::parse_quality_profiles(json);
}

std::vector<RootFolder> SonarrParsers::parse_root_folders(const std::string& json) {
    return RadarrParsers::parse_root_folders(json);
}

}  // namespace media_browser
