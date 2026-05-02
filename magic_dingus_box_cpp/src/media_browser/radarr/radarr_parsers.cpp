#include "media_browser/radarr/radarr_parsers.h"

#include <json/json.h>
#include <spdlog/spdlog.h>

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
            return img["remoteUrl"].asString();
        }
    }
    return "";
}

void fill_search_hit(const Json::Value& r, MovieSearchHit& h) {
    h.tmdb_id = r.get("tmdbId", 0).asInt();
    h.imdb_id = r.get("imdbId", "").asString();
    h.title = r.get("title", "").asString();
    h.original_title = r.get("originalTitle", "").asString();
    h.overview = r.get("overview", "").asString();
    h.year = r.get("year", 0).asInt();
    h.runtime_minutes = r.get("runtime", 0).asInt();
    h.rating = r["ratings"]["tmdb"].get("value", 0.0).asDouble();
    h.poster_url = pick_image(r["images"], "poster");
    h.fanart_url = pick_image(r["images"], "fanart");
}

}  // namespace

std::vector<MovieSearchHit> RadarrParsers::parse_movie_lookup(const std::string& json) {
    std::vector<MovieSearchHit> out;
    Json::Value root;
    if (!parse_json(json, root) || !root.isArray()) return out;
    for (const auto& r : root) {
        MovieSearchHit h;
        fill_search_hit(r, h);
        out.push_back(std::move(h));
    }
    return out;
}

std::vector<Movie> RadarrParsers::parse_movie_list(const std::string& json) {
    std::vector<Movie> out;
    Json::Value root;
    if (!parse_json(json, root) || !root.isArray()) return out;
    for (const auto& r : root) {
        Movie m;
        fill_search_hit(r, m);
        m.radarr_id = r.get("id", 0).asInt();
        m.monitored = r.get("monitored", false).asBool();
        m.has_file = r.get("hasFile", false).asBool();
        if (r.isMember("movieFile")) {
            const auto& f = r["movieFile"];
            m.file_path = f.get("relativePath", "").asString();
            m.file_container_path = f.get("path", "").asString();
            m.file_quality = f["quality"]["quality"].get("name", "").asString();
            m.file_size_bytes = f.get("size", 0).asInt64();
        }
        m.added_at = r.get("added", "").asString();
        out.push_back(std::move(m));
    }
    return out;
}

std::optional<Movie> RadarrParsers::parse_movie(const std::string& json) {
    Json::Value root;
    if (!parse_json(json, root) || !root.isObject()) return std::nullopt;
    Movie m;
    fill_search_hit(root, m);
    m.radarr_id = root.get("id", 0).asInt();
    m.monitored = root.get("monitored", false).asBool();
    m.has_file = root.get("hasFile", false).asBool();
    return m;
}

namespace {

// Parse Radarr `timeleft` strings into total seconds.
// Accepts "HH:MM:SS" and "D.HH:MM:SS" (days-dot-hours). Returns 0 on parse
// failure so the caller can leave QueueItem::eta_seconds at its default.
int parse_timeleft_to_seconds(const std::string& s) {
    if (s.empty()) return 0;
    int days = 0;
    size_t time_start = 0;
    // Optional "D.HH:MM:SS" prefix
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
    // sscanf returns the number of successfully parsed fields.
    if (std::sscanf(t.c_str(), "%d:%d:%d", &hh, &mm, &ss) != 3) {
        return 0;
    }
    if (hh < 0 || mm < 0 || ss < 0) return 0;
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

}  // namespace

std::vector<QueueItem> RadarrParsers::parse_queue(const std::string& json) {
    std::vector<QueueItem> out;
    Json::Value root;
    if (!parse_json(json, root)) return out;
    const auto& records = root["records"];
    if (!records.isArray()) return out;
    for (const auto& r : records) {
        QueueItem q;
        q.id = r.get("id", 0).asInt();
        q.movie_id = r.get("movieId", 0).asInt();
        q.title = r.get("title", "").asString();
        q.size_bytes = r.get("size", 0).asInt64();
        q.sizeleft_bytes = r.get("sizeleft", 0).asInt64();
        q.state = r.get("status", "").asString();
        // trackedDownloadState distinguishes "completed-and-imported"
        // from "completed-but-stuck" (importBlocked = the release passed
        // the download client but Radarr couldn't extract a video file
        // from it — e.g. a "movie" torrent that turned out to be only
        // trailers). LibraryScreen surfaces this as a 'Bad release'
        // badge instead of misleadingly saying "downloading".
        q.tracked_download_state = r.get("trackedDownloadState", "").asString();
        q.download_id = r.get("downloadId", "").asString();
        if (q.size_bytes > 0) {
            q.progress = static_cast<double>(q.size_bytes - q.sizeleft_bytes)
                         / static_cast<double>(q.size_bytes);
        }

        // Live telemetry (optional fields — absent on older Radarr versions
        // or non-torrent download clients).
        if (r.isMember("timeleft")) {
            q.eta_seconds = parse_timeleft_to_seconds(
                r["timeleft"].asString());
        }
        if (r.isMember("seeders")) {
            q.seeds = r.get("seeders", 0).asInt();
        }
        if (r.isMember("leechers")) {
            q.peers = r.get("leechers", 0).asInt();
        }
        // NOTE: download_rate_bps / upload_rate_bps stay at 0. Radarr's
        // /api/v3/queue shape does not reliably expose live transfer rates
        // across download-client types — qBittorrent-specific rate fields
        // live on the qBittorrent WebUI, not in Radarr's generic queue
        // record. Fetching them would require a second HTTP round-trip to
        // qBittorrent, which is out of scope for this parser.

        out.push_back(std::move(q));
    }
    return out;
}

std::vector<QualityProfile> RadarrParsers::parse_quality_profiles(const std::string& json) {
    std::vector<QualityProfile> out;
    Json::Value root;
    if (!parse_json(json, root) || !root.isArray()) return out;
    for (const auto& r : root) {
        QualityProfile p;
        p.id = r.get("id", 0).asInt();
        p.name = r.get("name", "").asString();
        p.cutoff_quality_id = r.get("cutoff", 0).asInt();
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<RootFolder> RadarrParsers::parse_root_folders(const std::string& json) {
    std::vector<RootFolder> out;
    Json::Value root;
    if (!parse_json(json, root) || !root.isArray()) return out;
    for (const auto& r : root) {
        RootFolder f;
        f.id = r.get("id", 0).asInt();
        f.path = r.get("path", "").asString();
        f.free_space_bytes = r.get("freeSpace", 0).asInt64();
        out.push_back(std::move(f));
    }
    return out;
}

std::optional<SystemStatus> RadarrParsers::parse_system_status(const std::string& json) {
    Json::Value root;
    if (!parse_json(json, root) || !root.isObject()) return std::nullopt;
    SystemStatus s;
    s.version = root.get("version", "").asString();
    s.build_time = root.get("buildTime", "").asString();
    s.startup_completed = true;
    return s;
}

}  // namespace media_browser
