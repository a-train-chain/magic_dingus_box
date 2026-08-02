#include "media_browser/sonarr/sonarr_client.h"
#include "media_browser/sonarr/sonarr_parsers.h"
#include "media_browser/radarr/radarr_client.h"   // normalize_prefix (one implementation)
#include "media_browser/radarr/radarr_parsers.h"  // parse_system_status

#include <curl/curl.h>
#include <json/json.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <sstream>
#include <thread>

namespace media_browser {

namespace {

size_t curl_write_cb(void* ptr, size_t size, size_t nmemb, void* ud) {
    auto* s = static_cast<std::string*>(ud);
    s->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

// RFC 3986 percent-encoding: unreserved characters pass through, everything
// else becomes %HH. Copied in spirit from tmdb_client.cpp rather than from
// RadarrClient::lookup's minimal encoder — that one allocates a CURL handle
// per call in ProwlarrClient's variant and percent-encodes '.'/'-' needlessly.
std::string url_encode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3 / 2);
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned char c : s) {
        const bool unreserved = (c >= 'A' && c <= 'Z')
                              || (c >= 'a' && c <= 'z')
                              || (c >= '0' && c <= '9')
                              || c == '-' || c == '_'
                              || c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

}  // namespace

SonarrClient::SonarrClient(Config config) : cfg_(std::move(config)) {
    cfg_.container_library_prefix = normalize_prefix(cfg_.container_library_prefix);
    cfg_.host_library_prefix      = normalize_prefix(cfg_.host_library_prefix);
    // libcurl reference-counts init/cleanup pairs, and RadarrClient/TmdbClient
    // already run their own — matching them here keeps the client usable on
    // its own (the test_media_browser CLI constructs one with nothing else
    // alive).
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

SonarrClient::~SonarrClient() {
    curl_global_cleanup();
}

std::string SonarrClient::normalize_prefix(std::string s) {
    // One implementation, shared with the Radarr client.
    return RadarrClient::normalize_prefix(std::move(s));
}

std::string SonarrClient::build_lookup_path_tmdb(int tmdb_id) {
    // The "tmdb:" prefix is matched literally by Sonarr's SkyHook proxy —
    // percent-encoding the colon is unnecessary and obscures the contract.
    return "/api/v3/series/lookup?term=tmdb:" + std::to_string(tmdb_id);
}

std::string SonarrClient::build_lookup_path_term(const std::string& term) {
    return "/api/v3/series/lookup?term=" + url_encode(term);
}

// --- transport -----------------------------------------------------------

namespace {
// Shared curl setup for all four verbs.
struct CurlRequest {
    CURL* curl = nullptr;
    curl_slist* headers = nullptr;
    std::string body;
    ~CurlRequest() {
        if (headers) curl_slist_free_all(headers);
        if (curl) curl_easy_cleanup(curl);
    }
};
}  // namespace

std::string SonarrClient::http_get(const std::string& path) {
    CurlRequest req;
    req.curl = curl_easy_init();
    if (!req.curl) { set_error("curl init failed"); return {}; }
    const std::string url = cfg_.base_url + path;
    req.headers = curl_slist_append(req.headers,
                                    ("X-Api-Key: " + cfg_.api_key).c_str());
    curl_easy_setopt(req.curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(req.curl, CURLOPT_HTTPHEADER, req.headers);
    curl_easy_setopt(req.curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(req.curl, CURLOPT_WRITEDATA, &req.body);
    curl_easy_setopt(req.curl, CURLOPT_TIMEOUT,
                     static_cast<long>(cfg_.timeout_secs));
    // Required off the main thread: without it libcurl's resolver uses
    // SIGALRM and crashes with a SIGSEGV inside the signal handler.
    // ProwlarrClient documents the same; RadarrClient omits it (a latent bug
    // this client deliberately does not copy).
    curl_easy_setopt(req.curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(req.curl);
    long http_code = 0;
    curl_easy_getinfo(req.curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (rc != CURLE_OK) { set_error(curl_easy_strerror(rc)); return {}; }
    if (http_code >= 400) {
        std::ostringstream os; os << "HTTP " << http_code;
        set_error(os.str());
        return {};
    }
    return req.body;
}

std::string SonarrClient::http_post(const std::string& path, const std::string& body) {
    CurlRequest req;
    req.curl = curl_easy_init();
    if (!req.curl) { set_error("curl init failed"); return {}; }
    const std::string url = cfg_.base_url + path;
    req.headers = curl_slist_append(req.headers,
                                    ("X-Api-Key: " + cfg_.api_key).c_str());
    req.headers = curl_slist_append(req.headers, "Content-Type: application/json");
    curl_easy_setopt(req.curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(req.curl, CURLOPT_HTTPHEADER, req.headers);
    curl_easy_setopt(req.curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(req.curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(req.curl, CURLOPT_WRITEDATA, &req.body);
    curl_easy_setopt(req.curl, CURLOPT_TIMEOUT,
                     static_cast<long>(cfg_.timeout_secs));
    curl_easy_setopt(req.curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(req.curl);
    long http_code = 0;
    curl_easy_getinfo(req.curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (rc != CURLE_OK) { set_error(curl_easy_strerror(rc)); return {}; }
    if (http_code >= 400) {
        // Include the body: Sonarr's validation failures (seriesExistsValidator,
        // bad qualityProfileId) explain themselves there and nowhere else.
        std::ostringstream os; os << "HTTP " << http_code << ": " << req.body;
        set_error(os.str());
        return {};
    }
    return req.body;
}

std::string SonarrClient::http_put(const std::string& path, const std::string& body) {
    CurlRequest req;
    req.curl = curl_easy_init();
    if (!req.curl) { set_error("curl init failed"); return {}; }
    const std::string url = cfg_.base_url + path;
    req.headers = curl_slist_append(req.headers,
                                    ("X-Api-Key: " + cfg_.api_key).c_str());
    req.headers = curl_slist_append(req.headers, "Content-Type: application/json");
    curl_easy_setopt(req.curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(req.curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(req.curl, CURLOPT_HTTPHEADER, req.headers);
    curl_easy_setopt(req.curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(req.curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(req.curl, CURLOPT_WRITEDATA, &req.body);
    curl_easy_setopt(req.curl, CURLOPT_TIMEOUT,
                     static_cast<long>(cfg_.timeout_secs));
    curl_easy_setopt(req.curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(req.curl);
    long http_code = 0;
    curl_easy_getinfo(req.curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (rc != CURLE_OK) { set_error(curl_easy_strerror(rc)); return {}; }
    if (http_code >= 400) {
        std::ostringstream os; os << "HTTP " << http_code << ": " << req.body;
        set_error(os.str());
        return {};
    }
    return req.body;
}

std::string SonarrClient::http_delete(const std::string& path) {
    CurlRequest req;
    req.curl = curl_easy_init();
    if (!req.curl) { set_error("curl init failed"); return {}; }
    const std::string url = cfg_.base_url + path;
    req.headers = curl_slist_append(req.headers,
                                    ("X-Api-Key: " + cfg_.api_key).c_str());
    curl_easy_setopt(req.curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(req.curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(req.curl, CURLOPT_HTTPHEADER, req.headers);
    curl_easy_setopt(req.curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(req.curl, CURLOPT_WRITEDATA, &req.body);
    curl_easy_setopt(req.curl, CURLOPT_TIMEOUT,
                     static_cast<long>(cfg_.timeout_secs));
    curl_easy_setopt(req.curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(req.curl);
    long http_code = 0;
    curl_easy_getinfo(req.curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (rc != CURLE_OK) { set_error(curl_easy_strerror(rc)); return {}; }
    if (http_code >= 400) {
        std::ostringstream os; os << "HTTP " << http_code;
        set_error(os.str());
        return {};
    }
    return req.body;
}

// --- endpoints -----------------------------------------------------------

bool SonarrClient::is_reachable() {
    return !http_get("/ping").empty();
}

std::optional<SystemStatus> SonarrClient::get_status() {
    auto resp = http_get("/api/v3/system/status");
    if (resp.empty()) return std::nullopt;
    // Same {version, buildTime} shape Radarr serves.
    return RadarrParsers::parse_system_status(resp);
}

std::vector<SeriesSearchHit>
SonarrClient::lookup_by_tmdb(int tmdb_id, const std::string& title_fallback) {
    auto resp = http_get(build_lookup_path_tmdb(tmdb_id));
    auto hits = resp.empty() ? std::vector<SeriesSearchHit>{}
                             : SonarrParsers::parse_series_lookup(resp);
    if (!hits.empty() || title_fallback.empty()) return hits;
    // No TMDB->TVDB mapping in SkyHook for this show. Retry as free text.
    spdlog::info("[sonarr] tmdb:{} had no lookup match; falling back to "
                 "title search '{}'", tmdb_id, title_fallback);
    return lookup(title_fallback);
}

std::vector<SeriesSearchHit> SonarrClient::lookup(const std::string& query) {
    auto resp = http_get(build_lookup_path_term(query));
    if (resp.empty()) return {};
    return SonarrParsers::parse_series_lookup(resp);
}

std::optional<std::vector<Series>> SonarrClient::get_library_checked() {
    auto resp = http_get("/api/v3/series");
    if (resp.empty()) return std::nullopt;
    return SonarrParsers::parse_series_list(resp);
}

std::vector<Series> SonarrClient::get_library() {
    return get_library_checked().value_or(std::vector<Series>{});
}

std::optional<Series> SonarrClient::get_series(int sonarr_id) {
    auto resp = http_get("/api/v3/series/" + std::to_string(sonarr_id));
    if (resp.empty()) return std::nullopt;
    return SonarrParsers::parse_series(resp);
}

std::optional<std::vector<Series>> SonarrClient::find_series_by_tvdb(int tvdb_id) {
    auto resp = http_get("/api/v3/series?tvdbId=" + std::to_string(tvdb_id));
    if (resp.empty()) return std::nullopt;  // transport/HTTP failure — NOT "absent"
    return SonarrParsers::parse_series_list(resp);
}

// True once the async refresh has visibly applied the REQUESTED monitoring
// outcome — episodes are known to exist AND the monitored set among seasons
// matches what was asked for. This is deliberately NOT "addOptions is gone"
// (a live probe on 2026-08-01 found Sonarr's SeriesResource never
// serializes addOptions back on GET, in POST responses or any of 40 polled
// reads — that signal is unobservable against real Sonarr) and NOT "episodes
// exist" alone (the same probe showed statistics can populate before the
// monitor enum is applied — Sonarr writes episodes and applies monitoring in
// two separate steps, so a bare episode-presence check settles mid-refresh
// and reports the wrong season list). Checking the outcome we actually asked
// for is the only definition that cannot false-positive during that race.
//
// ONLY meaningful for a record add_series itself just POSTed — see the
// header doc comment for why an EXISTING library record must be checked
// with record_refreshed() instead.
bool add_settled(const Series& s, bool monitor) {
    bool has_episodes = false;
    bool special_monitored = false;
    int monitored_non_special = 0;
    for (const auto& season : s.seasons) {
        if (season.episode_count > 0) has_episodes = true;
        if (season.monitored) {
            if (season.season_number == 0) special_monitored = true;
            else ++monitored_non_special;
        }
    }
    if (!has_episodes) return false;
    if (monitor) {
        // "firstSeason" settles once EXACTLY ONE non-special season is
        // monitored AND specials are NOT. The count alone is not enough: a
        // show with exactly one regular season plus specials has a
        // mid-refresh all-monitored state of {specials monitored, S1
        // monitored} where monitored_non_special is already 1, so without
        // the specials check this settles one poll early and reports
        // specials monitored when firstSeason will unmonitor them
        // (live-verified in Phase 2a: firstSeason left "S2-5 AND specials"
        // unmonitored). The non-special count is never compared against a
        // hardcoded season NUMBER because a show's first aired season is
        // not always numbered 1.
        return monitored_non_special == 1 && !special_monitored;
    }
    // "none" settles once nothing at all is monitored, specials included.
    return !special_monitored && monitored_non_special == 0;
}

// True once Sonarr has ever actually refreshed an EXISTING library record —
// i.e. episodes are known (any season shows episode_count > 0) — regardless
// of what is or is not monitored. See the header doc comment for why this,
// and NOT add_settled(), is the only question that means anything for a
// record add_series did not just create.
bool record_refreshed(const Series& s) {
    for (const auto& season : s.seasons) {
        if (season.episode_count > 0) return true;
    }
    return false;
}

AddSeriesResult SonarrClient::add_series(int tmdb_id,
                                         int quality_profile_id,
                                         bool monitor,
                                         const std::string& title_fallback) {
    AddSeriesResult result;
    set_error({});

    // Sonarr requires the full series resource on POST (title, tvdbId, images,
    // seasons, …), so the flow is lookup-then-mutate — same pattern as
    // RadarrClient::add_movie. Parse the RAW lookup body rather than going
    // through SonarrParsers: the POST needs every field, not just the ones
    // SeriesSearchHit models.
    std::string lookup_resp = http_get(build_lookup_path_tmdb(tmdb_id));
    Json::Value root;
    auto parse_into_root = [&root](const std::string& text) {
        Json::CharReaderBuilder rb;
        Json::Value parsed;
        std::string err;
        std::istringstream is(text);
        if (!Json::parseFromStream(rb, is, &parsed, &err)) return false;
        root = std::move(parsed);
        return true;
    };
    bool have = !lookup_resp.empty() && parse_into_root(lookup_resp)
                && ((root.isArray() && root.size() > 0) || root.isObject());
    if (!have && !title_fallback.empty()) {
        // No TMDB->TVDB mapping in SkyHook; retry as free text.
        spdlog::info("[sonarr] add_series: tmdb:{} unmapped, retrying title '{}'",
                     tmdb_id, title_fallback);
        lookup_resp = http_get(build_lookup_path_term(title_fallback));
        have = !lookup_resp.empty() && parse_into_root(lookup_resp)
               && ((root.isArray() && root.size() > 0) || root.isObject());
    }
    if (!have) {
        set_error("Sonarr lookup returned no results for tmdb:"
                  + std::to_string(tmdb_id));
        spdlog::error("[sonarr] add_series: {}", last_error());
        return result;  // ok=false
    }

    Json::Value series = root.isArray() ? root[0u] : root;

    // Already in the library? POSTing would 400 on seriesExistsValidator.
    //
    // The CHECKED probe matters here: nullopt means the request itself failed
    // (Sonarr rides Gluetun's netns, so this is a routine transport blip), and
    // treating that as "not present" would POST a duplicate and replace the
    // real network error with Sonarr's validation text. Abort instead.
    const int tvdb_id = series.get("tvdbId", 0).asInt();
    if (tvdb_id > 0) {
        auto probe = find_series_by_tvdb(tvdb_id);
        if (!probe) {
            set_error("Could not reach Sonarr to check whether tvdb:"
                      + std::to_string(tvdb_id) + " is already in the library");
            spdlog::error("[sonarr] add_series: {}", last_error());
            return result;  // ok=false — deliberately NO POST
        }
        if (!probe->empty()) {
            result.ok = true;
            result.series = probe->front();
            // record_refreshed(), NOT add_settled(): "does this match what I
            // just requested" is meaningless for a record Sonarr never
            // applied addOptions to. A real existing record's shape can be
            // the user's own permanent choice (every season monitored) or a
            // season-at-a-time state left by an earlier
            // set_season_monitored() call — add_settled would read either as
            // "unsettled" FOREVER, since the record's shape never changes on
            // its own, which is a guaranteed permanent wrong answer on the
            // app's primary workflow (every show passes through a partial-
            // monitored state at its second season). The only question that
            // means anything here is whether Sonarr has ever refreshed this
            // record at all.
            result.settled = record_refreshed(result.series);
            if (!result.settled) result.series.seasons.clear();
            spdlog::info("[sonarr] add_series: tvdb:{} already in library "
                         "(id={}, settled={}); returning existing record",
                         tvdb_id, result.series.sonarr_id, result.settled);
            return result;
        }
    }

    auto roots = get_root_folders();
    if (roots.empty()) {
        set_error("No root folder configured in Sonarr");
        spdlog::error("[sonarr] add_series: {}", last_error());
        return result;  // ok=false
    }

    series["qualityProfileId"] = quality_profile_id;
    series["rootFolderPath"]   = roots.front().path;
    series["monitored"]        = monitor;
    series["seasonFolder"]     = true;
    // NO minimumAvailability — that field does not exist on Sonarr's
    // SeriesResource (it is a Radarr concept).
    Json::Value addOptions;
    // Derive the enum from the caller's intent — never hardcode "firstSeason".
    // "firstSeason" monitors the first regular season and unmonitors everything
    // else INCLUDING specials (the spec's season-at-a-time default). "none"
    // unmonitors everything unconditionally, which is the only correct value
    // for an unmonitored add: Sonarr applies addOptions.monitor independently
    // of series.monitored, so sending "firstSeason" here would leave season 1
    // armed and it would start grabbing the moment anything re-monitors the
    // series. Enum values serialize camelCase.
    addOptions["monitor"] = monitor ? "firstSeason" : "none";
    addOptions["searchForMissingEpisodes"] = monitor;
    addOptions["searchForCutoffUnmetEpisodes"] = false;
    series["addOptions"] = addOptions;

    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    const std::string body = Json::writeString(wb, series);

    const std::string resp = http_post("/api/v3/series", body);
    if (resp.empty()) {
        spdlog::error("[sonarr] add_series POST failed: {}", last_error());
        return result;  // ok=false
    }

    // The POST response IS the stored resource (RestController.Created
    // serializes GetResourceById) — but the row was inserted microseconds ago
    // and the monitor enum has not been applied yet, so its seasons[] still
    // reads exactly what we submitted: all monitored. Take the id and
    // nothing else.
    int new_id = 0;
    {
        Json::CharReaderBuilder rb;
        Json::Value posted;
        std::string err;
        std::istringstream is(resp);
        if (Json::parseFromStream(rb, is, &posted, &err) && posted.isObject()) {
            new_id = posted.get("id", 0).asInt();
        }
    }
    if (new_id <= 0 && tvdb_id > 0) {
        // POST response carried no usable id — find the row by the key Sonarr
        // indexes on.
        if (auto probe = find_series_by_tvdb(tvdb_id); probe && !probe->empty()) {
            new_id = probe->front().sonarr_id;
        }
    }
    if (new_id <= 0) {
        set_error("Sonarr accepted the add but no series id could be resolved");
        spdlog::error("[sonarr] add_series: {}", last_error());
        return result;  // ok=false
    }

    // *** Bounded settle poll ***
    // AddSeriesService published SeriesAddedEvent, which queued a
    // RefreshSeriesCommand. Poll until add_settled() can confirm the outcome
    // we asked for actually landed — see add_settled()'s doc comment for why
    // that predicate, and not addOptions or bare episode presence, is the
    // only one that cannot false-positive mid-refresh.
    //
    // A series SkyHook has no episodes for yet (announced/upcoming) can
    // never satisfy add_settled() and will burn the FULL timeout budget on
    // every add. That is accepted and safe: the result is a correctly
    // labeled settled=false (caller re-fetches later), never a wrong answer.
    //
    // THIS SLEEPS — worker thread only.
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(cfg_.add_settle_timeout_ms);
    const std::string series_path = "/api/v3/series/" + std::to_string(new_id);
    for (;;) {
        const std::string cur = http_get(series_path);
        if (!cur.empty()) {
            if (auto parsed = SonarrParsers::parse_series(cur)) {
                result.ok = true;
                result.series = *parsed;   // keep the freshest read we have
                if (add_settled(result.series, monitor)) {
                    result.settled = true;
                    break;
                }
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(cfg_.add_settle_poll_ms));
    }

    if (!result.ok) {
        set_error("Sonarr accepted the add but its state could not be read back");
        spdlog::error("[sonarr] add_series: {}", last_error());
        return result;
    }
    if (!result.settled) {
        // Not an error — the add succeeded. But the season flags we last
        // read are a pending or mid-refresh snapshot, not the applied
        // outcome — clear them rather than leave something plausible-but-
        // wrong for a naive caller to render.
        result.series.seasons.clear();
        spdlog::warn("[sonarr] add_series: tmdb={} id={} did not settle within "
                     "{}ms; returning provisional state (caller must re-fetch)",
                     tmdb_id, new_id, cfg_.add_settle_timeout_ms);
    } else {
        // A settled result is unambiguously ok=true, settled=true — clear
        // any error an earlier poll iteration's failed GET left behind
        // (http_get sets it on every transport/HTTP failure), so a caller
        // checking last_error() after success doesn't see stale text from
        // a since-recovered blip.
        set_error({});
        spdlog::info("[sonarr] add_series ok: tmdb={} tvdb={} id={} '{}'",
                     tmdb_id, tvdb_id, new_id,
                     series.get("title", "?").asString());
    }
    return result;
}

bool SonarrClient::set_season_monitored(int sonarr_id, int season_number,
                                        bool monitored) {
    set_error({});
    const std::string path = "/api/v3/series/" + std::to_string(sonarr_id);
    const std::string current = http_get(path);
    if (current.empty()) {
        set_error("Sonarr series " + std::to_string(sonarr_id) + " not readable");
        return false;
    }
    Json::Value series;
    {
        Json::CharReaderBuilder rb;
        std::string err;
        std::istringstream is(current);
        if (!Json::parseFromStream(rb, is, &series, &err) || !series.isObject()) {
            set_error("Sonarr series parse failed: " + err);
            return false;
        }
    }
    Json::Value& seasons = series["seasons"];
    if (!seasons.isArray()) {
        set_error("Sonarr series " + std::to_string(sonarr_id) + " has no seasons[]");
        return false;
    }
    bool found = false;
    for (auto& s : seasons) {
        if (s.get("seasonNumber", -1).asInt() == season_number) {
            s["monitored"] = monitored;
            found = true;
            break;
        }
    }
    if (!found) {
        set_error("season " + std::to_string(season_number) + " not found on series "
                  + std::to_string(sonarr_id));
        return false;
    }
    // PUT replaces the whole resource — send the object back intact apart from
    // the one flag we changed.
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    return !http_put(path, Json::writeString(wb, series)).empty();
}

bool SonarrClient::trigger_season_search(int sonarr_id, int season_number) {
    // Without clearing first, a success here would leave a PRIOR call's
    // error text sitting in last_error() for the UI to surface as if it
    // were current.
    set_error({});
    std::ostringstream body;
    body << R"({"name":"SeasonSearch","seriesId":)" << sonarr_id
         << R"(,"seasonNumber":)" << season_number << R"(})";
    return !http_post("/api/v3/command", body.str()).empty();
}

bool SonarrClient::trigger_series_search(int sonarr_id) {
    set_error({});
    std::ostringstream body;
    body << R"({"name":"SeriesSearch","seriesId":)" << sonarr_id << R"(})";
    return !http_post("/api/v3/command", body.str()).empty();
}

bool SonarrClient::remove_series(int sonarr_id, bool delete_files) {
    set_error({});
    const std::string path = "/api/v3/series/" + std::to_string(sonarr_id)
                           + "?deleteFiles=" + (delete_files ? "true" : "false")
                           + "&addImportListExclusion=false";
    http_delete(path);
    return last_error().empty();
}

std::vector<SonarrQueueItem> SonarrClient::get_queue() {
    // Sonarr's queue is per EPISODE, so a season pack contributes one record
    // per episode and the queue genuinely outgrows a single page — page
    // through it rather than silently truncating (a missing row is
    // indistinguishable from "that download isn't queued").
    //
    // includeEpisode=true embeds each record's episode object so the UI can
    // label "S02E01 — Seven Thirty-Seven" without a second round-trip.
    // includeSeries stays false: the series is already in the library cache
    // and the extra payload is pure weight on the 2 GB board.
    //
    // Continuation is driven ONLY by whether the page came back full
    // (records.size() >= pageSize), never by comparing accumulated records
    // against totalRecords. totalRecords reflects Sonarr's count as of THAT
    // page's query, and with concurrent grabs/completions it can drift; a
    // page that happens to land exactly on the running total would trip an
    // early exit one page before the real end. total is still tracked, but
    // only to report how badly a capped fetch got truncated.
    constexpr int kMaxPages = 20;  // 2000 records at the default page size
    const int page_size = cfg_.queue_page_size > 0 ? cfg_.queue_page_size : 100;
    set_error({});  // clear any stale error so a prior call's failure can't be misattributed
    std::vector<SonarrQueueItem> out;
    int total = 0;
    int page = 1;
    for (; page <= kMaxPages; ++page) {
        const std::string resp = http_get(
            "/api/v3/queue?page=" + std::to_string(page)
            + "&pageSize=" + std::to_string(page_size)
            + "&includeEpisode=true&includeSeries=false");
        if (resp.empty()) {
            // Transport failure mid-paging — keep what we have rather than
            // discard prior pages, but the caller MUST know this result is
            // truncated: a season pack's sibling episodes can now be split
            // across "returned" and "lost to this failure", and a naive
            // caller grouping by download_id would render a partial season
            // as if it were the whole thing.
            spdlog::warn("[sonarr] get_queue: transport failure fetching page {} "
                         "(have {} records so far); queue result may be truncated",
                         page, out.size());
            break;
        }
        auto batch = SonarrParsers::parse_queue(resp);
        const int batch_total = SonarrParsers::parse_queue_total(resp);
        if (batch_total > 0) total = batch_total;
        const bool full_page = static_cast<int>(batch.size()) >= page_size;
        out.insert(out.end(), std::make_move_iterator(batch.begin()),
                   std::make_move_iterator(batch.end()));
        if (!full_page) break;  // short page: this was genuinely the last one
    }
    if (page > kMaxPages) {
        spdlog::warn("[sonarr] get_queue hit the {}-page cap with {} records "
                     "(totalRecords={}); the queue view is truncated",
                     kMaxPages, out.size(), total);
    }
    return out;
}

bool SonarrClient::cancel_queue_item(int queue_id) {
    set_error({});
    http_delete("/api/v3/queue/" + std::to_string(queue_id)
                + "?removeFromClient=true&blocklist=false");
    return last_error().empty();
}

std::optional<std::vector<std::string>>
SonarrClient::get_series_download_hashes_checked(int sonarr_id) {
    // Entry clear, same shape as cancel_queue_item / get_quality_profiles.
    // Load-bearing here, not merely tidy: http_get returns "" both on a
    // transport failure (curl error, HTTP >= 400 — set_error was called)
    // and would return "" on nothing else, since /api/v3/history/series
    // answers a real 200 with at least "[]". Without the clear, a PRIOR
    // call's stale error state would linger uninspected; callers of THIS
    // method never read last_error() at all — the nullopt/engaged split
    // below is the whole answer, by design (see the header's doc comment
    // for why: Task 8's background re-poll shares this client's one
    // last_error_ member with the orphan-proof remove worker).
    set_error({});
    // /api/v3/history/series is UNPAGINATED (a bare array) — no pageSize
    // parameter, unlike Radarr's /api/v3/history.
    auto resp = http_get("/api/v3/history/series?seriesId="
                         + std::to_string(sonarr_id));
    if (resp.empty()) return std::nullopt;  // transport/HTTP failure — NOT "no history"
    return SonarrParsers::parse_history_download_ids(resp);
}

std::vector<std::string> SonarrClient::get_series_download_hashes(int sonarr_id) {
    return get_series_download_hashes_checked(sonarr_id)
        .value_or(std::vector<std::string>{});
}

std::vector<QualityProfile> SonarrClient::get_quality_profiles() {
    // Without clearing first, an empty result here is ambiguous to callers
    // that read last_error() to tell "Sonarr answered, no profiles" from "we
    // never reached Sonarr" — a PRIOR call's error would be surfaced as if
    // it were this one's. Same entry clear as add_series /
    // set_season_monitored / trigger_season_search.
    set_error({});
    auto resp = http_get("/api/v3/qualityprofile");
    if (resp.empty()) return {};
    return SonarrParsers::parse_quality_profiles(resp);
}

std::vector<RootFolder> SonarrClient::get_root_folders() {
    auto resp = http_get("/api/v3/rootfolder");
    if (resp.empty()) return {};
    return SonarrParsers::parse_root_folders(resp);
}

std::vector<QualityDefinition> SonarrClient::get_quality_definitions() {
    auto resp = http_get("/api/v3/qualitydefinition");
    if (resp.empty()) return {};
    return SonarrParsers::parse_quality_definitions(resp);
}

std::string SonarrClient::resolve_host_path(const std::string& container_path) const {
    if (container_path.empty()) return container_path;
    if (container_path.rfind(cfg_.container_library_prefix, 0) == 0) {
        return cfg_.host_library_prefix +
               container_path.substr(cfg_.container_library_prefix.size());
    }
    // No legacy alternates: unlike the movie library, the TV subtree was
    // created after the hardlink migration, so /data/library/tv is the only
    // path Sonarr has ever recorded.
    spdlog::warn("[sonarr] resolve_host_path: '{}' does not match prefix "
                 "'{}'; passing through unchanged",
                 container_path, cfg_.container_library_prefix);
    return container_path;
}

}  // namespace media_browser
