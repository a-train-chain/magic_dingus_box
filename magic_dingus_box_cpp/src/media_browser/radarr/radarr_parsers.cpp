#include "media_browser/radarr/radarr_parsers.h"

#include <json/json.h>
#include <spdlog/spdlog.h>

#include <sstream>

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
        if (q.size_bytes > 0) {
            q.progress = static_cast<double>(q.size_bytes - q.sizeleft_bytes)
                         / static_cast<double>(q.size_bytes);
        }
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
