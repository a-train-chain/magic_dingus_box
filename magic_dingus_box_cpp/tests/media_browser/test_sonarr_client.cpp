#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
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
