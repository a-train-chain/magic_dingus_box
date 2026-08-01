#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <json/json.h>
#include "media_browser/sonarr/sonarr_client.h"
#include "media_browser/sonarr/sonarr_mock.h"

namespace fs = std::filesystem;
namespace mb = media_browser;

static std::string read_fixture(const std::string& name) {
    fs::path p = fs::path(__FILE__).parent_path() / "fixtures" / "sonarr" / name;
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// --- URL builders --------------------------------------------------------

TEST_CASE("build_lookup_path_tmdb keeps the tmdb: prefix literal",
          "[sonarr][url]") {
    // Live-proven on the box: GET /api/v3/series/lookup?term=tmdb:1396 →
    // Breaking Bad with tvdbId 81189. Sonarr's SkyHook proxy special-cases the
    // literal "tmdb:" prefix, so the colon must NOT be percent-encoded.
    CHECK(mb::SonarrClient::build_lookup_path_tmdb(1396) ==
          "/api/v3/series/lookup?term=tmdb:1396");
}

TEST_CASE("build_lookup_path_term percent-encodes the title", "[sonarr][url]") {
    CHECK(mb::SonarrClient::build_lookup_path_term("Breaking Bad") ==
          "/api/v3/series/lookup?term=Breaking%20Bad");
    // RFC 3986 unreserved characters pass through; everything else is %HH.
    CHECK(mb::SonarrClient::build_lookup_path_term("Marvel's Agents of S.H.I.E.L.D.") ==
          "/api/v3/series/lookup?term=Marvel%27s%20Agents%20of%20S.H.I.E.L.D.");
}

// --- Path translation ----------------------------------------------------

TEST_CASE("resolve_host_path: default config maps Sonarr's /data/library/tv",
          "[sonarr][paths]") {
    mb::SonarrClient c{mb::SonarrClient::Config{}};  // all defaults
    CHECK(c.resolve_host_path("/data/library/tv/Breaking Bad/S01E01.mkv") ==
          "/mnt/ssd/library/tv/Breaking Bad/S01E01.mkv");
}

TEST_CASE("resolve_host_path: rejects a /tv2 false prefix", "[sonarr][paths]") {
    mb::SonarrClient c{mb::SonarrClient::Config{}};
    CHECK(c.resolve_host_path("/data/library/tv2/foo.mkv") ==
          "/data/library/tv2/foo.mkv");
}

TEST_CASE("resolve_host_path: normalizes prefixes without a trailing slash",
          "[sonarr][paths]") {
    mb::SonarrClient::Config cfg;
    cfg.container_library_prefix = "/data/library/tv";      // no trailing slash
    cfg.host_library_prefix      = "/mnt/ssd/library/tv";   // no trailing slash
    mb::SonarrClient c(cfg);
    CHECK(c.resolve_host_path("/data/library/tv/Show/ep.mkv") ==
          "/mnt/ssd/library/tv/Show/ep.mkv");
    CHECK(c.resolve_host_path("/data/library/tv2/foo.mkv") ==
          "/data/library/tv2/foo.mkv");
}

TEST_CASE("resolve_host_path: passes through unrecognized and empty paths",
          "[sonarr][paths]") {
    mb::SonarrClient c{mb::SonarrClient::Config{}};
    CHECK(c.resolve_host_path("/data/library/Movies/x.mkv") ==
          "/data/library/Movies/x.mkv");
    CHECK(c.resolve_host_path("").empty());
}

// --- Stub harness --------------------------------------------------------

namespace {
// Records every path the client hits and replies from a canned table. Reused
// by later tasks' tests.
class StubSonarr : public mb::SonarrClient {
public:
    StubSonarr() : SonarrClient(Config{}) {}
    std::vector<std::string> gets;
    // path prefix -> response body. First matching prefix wins.
    std::vector<std::pair<std::string, std::string>> get_replies;

    std::string http_get(const std::string& path) override {
        gets.push_back(path);
        for (const auto& kv : get_replies) {
            if (path.rfind(kv.first, 0) == 0) return kv.second;
        }
        return "";
    }
    std::string http_post(const std::string&, const std::string&) override { return ""; }
    std::string http_put(const std::string&, const std::string&) override { return ""; }
    std::string http_delete(const std::string&) override { return ""; }
};
}  // namespace

TEST_CASE("lookup_by_tmdb hits the tmdb: path and parses the result",
          "[sonarr][lookup]") {
    StubSonarr s;
    s.get_replies = {{"/api/v3/series/lookup?term=tmdb:1396",
                      read_fixture("series_lookup.json")}};
    auto hits = s.lookup_by_tmdb(1396);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].tvdb_id == 81189);
    REQUIRE(s.gets.size() == 1);
    CHECK(s.gets[0] == "/api/v3/series/lookup?term=tmdb:1396");
}

TEST_CASE("lookup_by_tmdb falls back to a title search on an empty result",
          "[sonarr][lookup]") {
    // Some shows have no TMDB->TVDB mapping in SkyHook; the tmdb: lookup then
    // returns []. Without the fallback the kiosk could never add them.
    StubSonarr s;
    s.get_replies = {
        {"/api/v3/series/lookup?term=tmdb:", "[]"},
        {"/api/v3/series/lookup?term=Breaking%20Bad",
         read_fixture("series_lookup.json")},
    };
    auto hits = s.lookup_by_tmdb(1396, "Breaking Bad");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].title == "Breaking Bad");
    REQUIRE(s.gets.size() == 2);
    CHECK(s.gets[0] == "/api/v3/series/lookup?term=tmdb:1396");
    CHECK(s.gets[1] == "/api/v3/series/lookup?term=Breaking%20Bad");
}

TEST_CASE("lookup_by_tmdb does not fall back without a title",
          "[sonarr][lookup]") {
    StubSonarr s;
    s.get_replies = {{"/api/v3/series/lookup?term=tmdb:", "[]"}};
    CHECK(s.lookup_by_tmdb(1396).empty());
    CHECK(s.gets.size() == 1);
}

TEST_CASE("get_library_checked distinguishes empty from failed",
          "[sonarr][library]") {
    // The Radarr equivalent shipped as a bug fix: a bare vector made "empty
    // library" and "GET failed" indistinguishable, which broke For You.
    // Start with the checked shape rather than retrofit it.
    SECTION("HTTP failure → nullopt") {
        StubSonarr s;  // no replies configured → http_get returns ""
        CHECK_FALSE(s.get_library_checked().has_value());
        CHECK(s.get_library().empty());
    }
    SECTION("genuinely empty library → engaged optional, empty vector") {
        StubSonarr s;
        s.get_replies = {{"/api/v3/series", "[]"}};
        auto lib = s.get_library_checked();
        REQUIRE(lib.has_value());
        CHECK(lib->empty());
    }
    SECTION("populated library") {
        StubSonarr s;
        s.get_replies = {{"/api/v3/series", read_fixture("series_list.json")}};
        auto lib = s.get_library_checked();
        REQUIRE(lib.has_value());
        REQUIRE(lib->size() == 1);
        CHECK((*lib)[0].sonarr_id == 7);
    }
}

TEST_CASE("get_series and find_series_by_tvdb use the right paths",
          "[sonarr][library]") {
    StubSonarr s;
    s.get_replies = {
        {"/api/v3/series?tvdbId=81189", read_fixture("series_list.json")},
        {"/api/v3/series/7", read_fixture("series_added.json")},
    };
    auto by_tvdb = s.find_series_by_tvdb(81189);
    REQUIRE(by_tvdb.has_value());
    REQUIRE(by_tvdb->size() == 1);
    CHECK((*by_tvdb)[0].sonarr_id == 7);

    auto by_id = s.get_series(7);
    REQUIRE(by_id.has_value());
    CHECK(by_id->seasons.size() == 6);

    REQUIRE(s.gets.size() == 2);
    CHECK(s.gets[0] == "/api/v3/series?tvdbId=81189");
    CHECK(s.gets[1] == "/api/v3/series/7");
}

TEST_CASE("find_series_by_tvdb separates 'not in library' from 'request failed'",
          "[sonarr][library]") {
    // This probe gates a MUTATION (add_series decides whether to POST), so
    // collapsing the two outcomes is the same class of bug get_library_checked
    // exists to fix — and Sonarr rides Gluetun's netns, so transport blips are
    // routine rather than theoretical.
    SECTION("server answered: not in the library") {
        StubSonarr s;
        s.get_replies = {{"/api/v3/series?tvdbId=", "[]"}};
        auto r = s.find_series_by_tvdb(81189);
        REQUIRE(r.has_value());   // the request worked...
        CHECK(r->empty());        // ...and the answer is "no"
    }
    SECTION("transport failure") {
        StubSonarr s;  // no replies configured → http_get returns ""
        CHECK_FALSE(s.find_series_by_tvdb(81189).has_value());
    }
    SECTION("already in the library") {
        StubSonarr s;
        s.get_replies = {{"/api/v3/series?tvdbId=", read_fixture("series_list.json")}};
        auto r = s.find_series_by_tvdb(81189);
        REQUIRE(r.has_value());
        REQUIRE(r->size() == 1);
        CHECK((*r)[0].sonarr_id == 7);
    }
}

TEST_CASE("profiles and root folders use the Servarr paths", "[sonarr][config]") {
    StubSonarr s;
    s.get_replies = {
        {"/api/v3/qualityprofile", read_fixture("quality_profiles.json")},
        {"/api/v3/rootfolder", read_fixture("root_folders.json")},
    };
    auto profiles = s.get_quality_profiles();
    REQUIRE(profiles.size() == 1);
    CHECK(profiles[0].name == "Any");
    auto roots = s.get_root_folders();
    REQUIRE(roots.size() == 1);
    CHECK(roots[0].path == "/data/library/tv");
    CHECK(s.gets[0] == "/api/v3/qualityprofile");
    CHECK(s.gets[1] == "/api/v3/rootfolder");
}

// --- Mock ----------------------------------------------------------------

TEST_CASE("SonarrMockClient serves a coherent seeded library", "[sonarr][mock]") {
    mb::SonarrMockClient m;
    CHECK(m.is_reachable());
    auto lib = m.get_library_checked();
    REQUIRE(lib.has_value());
    REQUIRE(lib->size() == 1);
    CHECK((*lib)[0].sonarr_id == 1);
    CHECK((*lib)[0].tmdb_id == 1396);
    // Season 1 monitored, the rest not — the same shape a real firstSeason
    // add produces, so a mock-mode screen renders the real state machine.
    REQUIRE((*lib)[0].seasons.size() >= 2);
    CHECK_FALSE((*lib)[0].seasons[0].monitored);  // Specials
    CHECK((*lib)[0].seasons[1].monitored);        // Season 1

    auto one = m.get_series(1);
    REQUIRE(one.has_value());
    CHECK(one->title == (*lib)[0].title);

    auto profiles = m.get_quality_profiles();
    REQUIRE(profiles.size() == 1);
    CHECK(profiles[0].name == "Any");
    CHECK(m.get_root_folders().front().path == "/data/library/tv");
}

// --- add_series ----------------------------------------------------------

namespace {
// Reproduces the live box's ACTUAL behaviour. Sonarr's POST returns the
// STORED resource (RestController.Created serializes GetResourceById), but
// addOptions is applied ASYNCHRONOUSLY — AddSeriesService persists, publishes
// SeriesAddedEvent, which queues a RefreshSeriesCommand; only once
// RefreshSeriesService has pulled episodes from SkyHook does
// EpisodeMonitoredService apply the monitor enum and null addOptions out.
//
// So both the POST response AND any immediate GET show every season
// monitored:true. This stub models exactly that: the POST and the first GET
// return the pending fixture, later GETs return the settled one.
class AddSonarr : public mb::SonarrClient {
public:
    static Config fast_settle() {
        Config c;
        // Tight budget: the settle path needs only two polls, and the
        // never-settles path must not spin for long.
        c.add_settle_timeout_ms = 50;
        c.add_settle_poll_ms = 0;   // never actually sleep in the suite
        return c;
    }
    AddSonarr() : SonarrClient(fast_settle()) {}
    std::vector<std::string> gets;
    std::string post_path, post_body;
    bool already_added = false;
    bool never_settles = false;   // simulate a refresh that never completes
    bool probe_fails = false;     // simulate a Gluetun blip on the tvdbId probe
    int series_gets = 0;

    std::string http_get(const std::string& path) override {
        gets.push_back(path);
        if (path.rfind("/api/v3/series/lookup?term=tmdb:1396", 0) == 0) {
            return read_fixture("series_lookup.json");
        }
        if (path.rfind("/api/v3/series?tvdbId=81189", 0) == 0) {
            if (probe_fails) return "";  // transport failure, NOT "absent"
            return already_added ? read_fixture("series_list.json") : "[]";
        }
        if (path.rfind("/api/v3/rootfolder", 0) == 0) {
            return read_fixture("root_folders.json");
        }
        if (path.rfind("/api/v3/series/7", 0) == 0) {
            // First read races the refresh; later reads see it landed.
            ++series_gets;
            if (never_settles || series_gets == 1) {
                return read_fixture("series_added_pending.json");
            }
            return read_fixture("series_added.json");
        }
        return "";
    }
    std::string http_post(const std::string& path, const std::string& body) override {
        post_path = path;
        post_body = body;
        // The stored resource as it exists the instant after the insert:
        // id assigned, addOptions still populated, seasons untouched.
        return read_fixture("series_added_pending.json");
    }
    std::string http_put(const std::string&, const std::string&) override { return ""; }
    std::string http_delete(const std::string&) override { return ""; }
};
}  // namespace

TEST_CASE("add_series polls until Sonarr's async refresh settles",
          "[sonarr][add]") {
    // THE load-bearing test of this phase. A single read — of the POST body OR
    // of an immediate GET — returns every season monitored:true, because the
    // monitor enum has not been applied yet. A client that trusted it would
    // tell the UI the whole series is monitored, and "download next season"
    // would target the wrong season forever.
    AddSonarr s;
    auto added = s.add_series(1396, /*quality_profile_id=*/1, /*monitor=*/true);
    REQUIRE(added.ok);
    REQUIRE(added.settled);           // the poll saw the refresh land
    CHECK(added.series.sonarr_id == 7);
    REQUIRE(added.series.seasons.size() == 6);
    CHECK_FALSE(added.series.seasons[0].monitored);  // Specials
    CHECK(added.series.seasons[1].monitored);        // Season 1 — the only one
    CHECK_FALSE(added.series.seasons[2].monitored);
    CHECK_FALSE(added.series.seasons[3].monitored);
    CHECK_FALSE(added.series.seasons[4].monitored);
    CHECK_FALSE(added.series.seasons[5].monitored);

    // Proof it POLLED rather than reading once: the first GET returned the
    // pending state and was correctly rejected.
    CHECK(s.series_gets >= 2);
}

TEST_CASE("add_series returns a PROVISIONAL result when the refresh never lands",
          "[sonarr][add]") {
    // Bounded, not unbounded: a wedged SkyHook fetch must not hang the worker.
    // The caller still gets the series (the add DID happen) but settled=false
    // tells Phase 2c to re-fetch instead of caching an all-monitored season list.
    AddSonarr s;
    s.never_settles = true;
    auto added = s.add_series(1396, 1, true);
    CHECK(added.ok);                 // the add succeeded
    CHECK_FALSE(added.settled);      // ...but the season flags are not trustworthy
    CHECK(added.series.sonarr_id == 7);
}

TEST_CASE("add_series POSTs a valid Sonarr payload", "[sonarr][add]") {
    AddSonarr s;
    REQUIRE(s.add_series(1396, 1, true).ok);
    CHECK(s.post_path == "/api/v3/series");
    CHECK(s.post_body.find(R"("qualityProfileId":1)") != std::string::npos);
    CHECK(s.post_body.find(R"("rootFolderPath":"/data/library/tv")") != std::string::npos);
    CHECK(s.post_body.find(R"("monitor":"firstSeason")") != std::string::npos);
    CHECK(s.post_body.find(R"("searchForMissingEpisodes":true)") != std::string::npos);
    CHECK(s.post_body.find(R"("seasonFolder":true)") != std::string::npos);
    // minimumAvailability is a Radarr-only concept; sending it to Sonarr is
    // meaningless noise at best.
    CHECK(s.post_body.find("minimumAvailability") == std::string::npos);
}

TEST_CASE("add_series with monitor=false sends monitor:none, not firstSeason",
          "[sonarr][add]") {
    // Sonarr honours addOptions.monitor INDEPENDENTLY of series.monitored. An
    // unmonitored add that still said "firstSeason" would leave a fully
    // monitored season 1 underneath, and the moment anything flips
    // series.monitored true — a user toggle, a 2c "resume", a seasonpass bulk
    // edit — Sonarr grabs the whole season with nobody having asked for it.
    AddSonarr s;
    REQUIRE(s.add_series(1396, 1, /*monitor=*/false).ok);
    CHECK(s.post_body.find(R"("monitor":"none")") != std::string::npos);
    CHECK(s.post_body.find("firstSeason") == std::string::npos);
    CHECK(s.post_body.find(R"("searchForMissingEpisodes":false)") != std::string::npos);
}

TEST_CASE("add_series is idempotent when the series is already in the library",
          "[sonarr][add]") {
    // POSTing an already-added tvdbId 400s on seriesExistsValidator. Detect it
    // first and return the existing record instead of surfacing an error.
    AddSonarr s;
    s.already_added = true;
    auto added = s.add_series(1396, 1, true);
    REQUIRE(added.ok);
    CHECK(added.settled);  // an existing library record is settled by definition
    CHECK(added.series.sonarr_id == 7);
    CHECK(s.post_path.empty());  // nothing was POSTed
}

TEST_CASE("add_series ABORTS when the existence probe cannot reach Sonarr",
          "[sonarr][add]") {
    // Sonarr shares Gluetun's netns, so the probe failing mid-add is routine.
    // Treating that as "not in the library" would POST a duplicate and hand the
    // user Sonarr's 400 validation text instead of the real network fault.
    AddSonarr s;
    s.probe_fails = true;
    auto added = s.add_series(1396, 1, true);
    CHECK_FALSE(added.ok);
    CHECK(s.post_path.empty());  // crucially: no POST was issued
    CHECK_FALSE(s.last_error().empty());
}

TEST_CASE("add_series fails cleanly when the lookup finds nothing",
          "[sonarr][add]") {
    class EmptyLookup : public mb::SonarrClient {
    public:
        EmptyLookup() : SonarrClient(Config{}) {}
        std::string http_get(const std::string& path) override {
            if (path.rfind("/api/v3/series/lookup", 0) == 0) return "[]";
            return "";
        }
        std::string http_post(const std::string&, const std::string&) override {
            FAIL("add_series must not POST without a lookup result");
            return "";
        }
        std::string http_put(const std::string&, const std::string&) override { return ""; }
        std::string http_delete(const std::string&) override { return ""; }
    };
    EmptyLookup s;
    CHECK_FALSE(s.add_series(1396, 1, true).ok);
    CHECK_FALSE(s.last_error().empty());
}

// --- season monitoring ---------------------------------------------------

namespace {
class PutSonarr : public mb::SonarrClient {
public:
    PutSonarr() : SonarrClient(Config{}) {}
    std::string put_path, put_body, post_path, post_body, delete_path;
    std::string http_get(const std::string& path) override {
        if (path.rfind("/api/v3/series/7", 0) == 0) {
            return read_fixture("series_added.json");
        }
        return "";
    }
    std::string http_put(const std::string& path, const std::string& body) override {
        put_path = path;
        put_body = body;
        return body;  // Sonarr returns the updated resource
    }
    std::string http_post(const std::string& path, const std::string& body) override {
        post_path = path;
        post_body = body;
        return R"({"id":1,"name":"SeasonSearch","status":"queued"})";
    }
    std::string http_delete(const std::string& path) override {
        delete_path = path;
        return "{}";
    }
};
}  // namespace

TEST_CASE("set_season_monitored flips exactly one season and PUTs the whole "
          "resource", "[sonarr][seasons]") {
    // Sonarr's PUT /api/v3/series/{id} replaces the resource — sending a
    // partial object silently wipes the fields left out.
    PutSonarr s;
    REQUIRE(s.set_season_monitored(7, 2, true));
    CHECK(s.put_path == "/api/v3/series/7");

    Json::Value sent;
    {
        Json::CharReaderBuilder rb;
        std::string err;
        std::istringstream is(s.put_body);
        REQUIRE(Json::parseFromStream(rb, is, &sent, &err));
    }
    // Non-season fields survive the round-trip.
    CHECK(sent["id"].asInt() == 7);
    CHECK(sent["path"].asString() == "/data/library/tv/Breaking Bad");
    CHECK(sent["qualityProfileId"].asInt() == 1);
    REQUIRE(sent["seasons"].isArray());
    REQUIRE(sent["seasons"].size() == 6);
    for (const auto& season : sent["seasons"]) {
        const int n = season["seasonNumber"].asInt();
        const bool mon = season["monitored"].asBool();
        if (n == 1 || n == 2) CHECK(mon);   // 1 was already on, 2 just flipped
        else                  CHECK_FALSE(mon);
    }
}

TEST_CASE("set_season_monitored reports failure for an unknown season",
          "[sonarr][seasons]") {
    PutSonarr s;
    CHECK_FALSE(s.set_season_monitored(7, 99, true));
    CHECK(s.put_path.empty());  // nothing sent
    CHECK_FALSE(s.last_error().empty());
}

// --- commands + delete ---------------------------------------------------

TEST_CASE("trigger_season_search posts the SeasonSearch command",
          "[sonarr][commands]") {
    PutSonarr s;
    REQUIRE(s.trigger_season_search(7, 2));
    CHECK(s.post_path == "/api/v3/command");
    CHECK(s.post_body == R"({"name":"SeasonSearch","seriesId":7,"seasonNumber":2})");
}

TEST_CASE("trigger_series_search posts the SeriesSearch command",
          "[sonarr][commands]") {
    PutSonarr s;
    REQUIRE(s.trigger_series_search(7));
    CHECK(s.post_path == "/api/v3/command");
    CHECK(s.post_body == R"({"name":"SeriesSearch","seriesId":7})");
}

TEST_CASE("remove_series deletes with files and no import-list exclusion",
          "[sonarr][remove]") {
    PutSonarr s;
    REQUIRE(s.remove_series(7, /*delete_files=*/true));
    // addImportListExclusion=false: excluding it would make Sonarr refuse to
    // ever re-add the show, which is not what "remove from my box" means.
    CHECK(s.delete_path ==
          "/api/v3/series/7?deleteFiles=true&addImportListExclusion=false");
}
