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
#include "media_browser/ui/series_detail_logic.h"

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

TEST_CASE("resolve_host_path: a bare root-folder path needs normalize_prefix",
          "[sonarr][paths]") {
    // Sonarr's GET /rootfolder returns "/data/library/tv" WITHOUT a trailing
    // slash; the configured container prefix carries one. The raw path
    // therefore passes through UNMAPPED (pinning the contract that makes
    // normalization at the call site REQUIRED — the whole-series preflight's
    // host probe depends on it), while the normalized form maps.
    mb::SonarrClient c{mb::SonarrClient::Config{}};  // all defaults
    CHECK(c.resolve_host_path("/data/library/tv") == "/data/library/tv");
    CHECK(c.resolve_host_path(mb::SonarrClient::normalize_prefix("/data/library/tv"))
          == "/mnt/ssd/library/tv/");
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
    long http_delete(const std::string&) override { return 200; }
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
    // is_reachable()==true and (below/elsewhere) find_series_by_tvdb()
    // returning an engaged optional are KNOWN, DELIBERATE, currently
    // out-of-scope traps, not a contract to preserve. Neither has a live
    // caller today, but get_library_checked() — the method that DOES have
    // callers (BrowseScreen's TV library refresh) — was deliberately made to
    // report unreachable (nullopt) even though this mock otherwise looks
    // "up", specifically so a box with no Sonarr configured reads as
    // "no TV library" rather than fabricating one (see 63f9046 and
    // sonarr_mock.cpp's get_library_checked() doc comment for the full
    // rationale: an engaged optional here would poison the in-library hide
    // and seed For You's TV recommendations from a fake Breaking Bad entry).
    // If a future Sonarr health gate or add flow ever keys off
    // is_reachable() or find_series_by_tvdb() the way BrowseScreen keys off
    // get_library_checked(), it would silently re-open that exact hole.
    // Changing this assertion later means you are changing a known-deferred
    // trap, not breaking a contract — go read that rationale first.
    CHECK(m.is_reachable());
    // Seeded-data assertions go through the unchecked shape now — see the
    // dedicated get_library_checked test below for the reachability contract.
    auto lib = m.get_library();
    REQUIRE(lib.size() == 1);
    CHECK(lib[0].sonarr_id == 1);
    CHECK(lib[0].tmdb_id == 1396);
    // Season 1 monitored, the rest not — the same shape a real firstSeason
    // add produces, so a mock-mode screen renders the real state machine.
    REQUIRE(lib[0].seasons.size() >= 2);
    CHECK_FALSE(lib[0].seasons[0].monitored);  // Specials
    CHECK(lib[0].seasons[1].monitored);        // Season 1

    auto one = m.get_series(1);
    REQUIRE(one.has_value());
    CHECK(one->title == lib[0].title);

    auto profiles = m.get_quality_profiles();
    REQUIRE(profiles.size() == 1);
    CHECK(profiles[0].name == "Any");
    CHECK(m.get_root_folders().front().path == "/data/library/tv");
}

TEST_CASE("SonarrMockClient's get_library_checked reports unreachable, "
          "matching a real box with no Sonarr configured", "[sonarr][mock]") {
    // Review fix: the mock exists precisely BECAUSE no live Sonarr is
    // present, so get_library_checked() — whose whole contract is
    // reachability — must answer "unreachable" like the absent service it
    // stands in for, not hand back an engaged optional wrapping the seed.
    // Pre-fix this made SonarrMockClient the only mock whose checked library
    // call lied about reachability, which on a box with no Sonarr API key
    // configured (main.cpp's fallback path) silently fabricated a one-series
    // TV library, hid Breaking Bad from every TV grid as "owned," and seeded
    // For You's TV recommendations from it. See sonarr_mock.cpp for the full
    // rationale, including why RadarrMockClient does not need the same fix.
    mb::SonarrMockClient m;
    CHECK_FALSE(m.get_library_checked().has_value());
    // The unchecked shape is untouched — dev/test callers that want the
    // mock's data still get it.
    CHECK(m.get_library().size() == 1);
}

// --- add_series ----------------------------------------------------------

namespace {
// Models the live box's ACTUAL sequence, live-probed on 2026-08-01 by
// adding Breaking Bad and polling GET /series/{id} 40 times at 0.7s:
//   - `addOptions` was ABSENT from the POST response AND from all 40 GETs —
//     Sonarr's SeriesResource never serializes AddOptions on read, in any
//     build tested. A predicate keyed on "addOptions disappeared" is
//     therefore dead code against real Sonarr; do not resurrect it.
//   - Sonarr writes episodes and applies addOptions.monitor in SEPARATE
//     steps. There is a real, observable window where
//     statistics.totalEpisodeCount > 0 (RefreshSeriesService has pulled the
//     episode list from SkyHook) but every season still reads
//     monitored:true (EpisodeMonitoredService has not yet run). A predicate
//     keyed on episode presence ALONE reads that window as settled and
//     hands back "the whole series is monitored" — the exact failure this
//     task exists to prevent.
// This stub reproduces all three stages: episodes absent, the intermediate
// "episodes present, still all-monitored" stage (unsettled by definition),
// and the terminal "episodes present, only the requested season monitored"
// stage (settled).
class AddSonarr : public mb::SonarrClient {
public:
    static Config fast_settle() {
        Config c;
        // Tight budget: the settle path needs only three polls (absent →
        // mid-refresh → landed), and the never-settles path must not spin
        // for long.
        c.add_settle_timeout_ms = 50;
        c.add_settle_poll_ms = 0;   // never actually sleep in the suite
        return c;
    }
    AddSonarr() : SonarrClient(fast_settle()) {}
    std::vector<std::string> gets;
    std::string post_path, post_body;
    bool already_added = false;
    // The existing record's user has monitored EVERY season (their own
    // choice) rather than being left in the firstSeason shape. Sonarr never
    // (re)applies addOptions to an existing series, so this is a completely
    // legitimate, permanent state — not a mid-refresh snapshot.
    bool already_added_all_monitored = false;
    // Pins the sequence on the DANGEROUS middle stage forever: episodes
    // present, but every season still monitored:true. This is the state a
    // bare "episodes exist" check would misread as settled.
    bool never_settles = false;
    bool probe_fails = false;     // simulate a Gluetun blip on the tvdbId probe
    int series_gets = 0;

    std::string http_get(const std::string& path) override {
        gets.push_back(path);
        if (path.rfind("/api/v3/series/lookup?term=tmdb:1396", 0) == 0) {
            return read_fixture("series_lookup.json");
        }
        if (path.rfind("/api/v3/series?tvdbId=81189", 0) == 0) {
            if (probe_fails) return "";  // transport failure, NOT "absent"
            if (already_added_all_monitored) {
                return read_fixture("series_list_all_monitored.json");
            }
            return already_added ? read_fixture("series_list.json") : "[]";
        }
        if (path.rfind("/api/v3/rootfolder", 0) == 0) {
            return read_fixture("root_folders.json");
        }
        if (path.rfind("/api/v3/series/7", 0) == 0) {
            ++series_gets;
            if (never_settles) return read_fixture("series_added_mid_refresh.json");
            if (series_gets == 1) return read_fixture("series_added_pending.json");
            if (series_gets == 2) return read_fixture("series_added_mid_refresh.json");
            return read_fixture("series_added.json");
        }
        return "";
    }
    std::string http_post(const std::string& path, const std::string& body) override {
        post_path = path;
        post_body = body;
        // The stored resource as it exists the instant after the insert:
        // id assigned, seasons untouched. No addOptions — see the class doc
        // comment above; real Sonarr never serializes it back.
        return read_fixture("series_added_pending.json");
    }
    std::string http_put(const std::string&, const std::string&) override { return ""; }
    long http_delete(const std::string&) override { return 200; }
};
}  // namespace

TEST_CASE("add_series polls until Sonarr's async refresh settles",
          "[sonarr][add]") {
    // THE load-bearing test of this phase. The stub walks all three stages a
    // real add goes through: episodes absent, episodes present but every
    // season still monitored:true (the dangerous race window — see AddSonarr's
    // doc comment), and finally episodes present with only the requested
    // season monitored. A client that settled on either of the first two
    // would tell the UI the whole series is monitored, and "download next
    // season" would target the wrong season forever.
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

    // Proof it POLLED THROUGH BOTH false-settle traps rather than stopping
    // early: stage 1 (no episodes) and stage 2 (episodes present, all still
    // monitored) were both correctly rejected before stage 3 landed.
    CHECK(s.series_gets >= 3);
}

TEST_CASE("add_series stays UNSETTLED when episodes exist but every season is "
          "still monitored, and times out cleanly (the mid-refresh race)",
          "[sonarr][add]") {
    // Bounded, not unbounded: a wedged refresh must not hang the worker. This
    // pins the sequence on the exact race window the live probe found —
    // statistics.totalEpisodeCount > 0 with every season still
    // monitored:true — forever. A predicate keyed on episode presence alone
    // (the pre-fix implementation) would read this as settled and hand back
    // "the whole series is monitored". The caller still gets the series (the
    // add DID happen) but settled=false tells Phase 2c to re-fetch instead of
    // caching a plausible-but-wrong season list — and seasons[] is cleared
    // rather than left holding that wrong list.
    AddSonarr s;
    s.never_settles = true;
    auto added = s.add_series(1396, 1, true);
    CHECK(added.ok);                       // the add succeeded
    CHECK_FALSE(added.settled);            // ...but the outcome never matched the request
    CHECK(added.series.sonarr_id == 7);
    CHECK(added.series.seasons.empty());   // unsettled → no plausible-but-wrong list
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
    // settled is NOT hardcoded true for this path — record_refreshed() is
    // applied to the probed record (see the ALL-monitored test below for
    // why it must be record_refreshed and NOT add_settled). series_list.json
    // already has episodes, so it reads refreshed/settled here too.
    CHECK(added.settled);
    CHECK(added.series.sonarr_id == 7);
    CHECK(s.post_path.empty());  // nothing was POSTed
}

TEST_CASE("add_series treats an existing ALL-monitored record as settled "
          "via record_refreshed, not stuck unsettled forever",
          "[sonarr][add]") {
    // CRITICAL bug found in review: the idempotent path must NOT ask
    // "does this record match what I just requested" (add_settled) —
    // that question is meaningless for a record Sonarr never applied
    // addOptions to. A real existing record's monitored-season shape can be
    // anything: the user's own choice to monitor every season (this
    // fixture), an earlier set_season_monitored(id, 2, true) call mid-way
    // through a season pass (S1+S2 monitored), or coincidentally the
    // firstSeason shape (only reason the OTHER idempotent test above
    // passed). add_settled reads the first two as "unsettled" FOREVER,
    // because the record's shape never changes on its own — the caller
    // would be told settled=false, seasons CLEARED, "re-fetch me", and
    // every re-fetch would return the exact same permanent wrong answer.
    // Season-at-a-time means EVERY show passes through the S1+S2 state at
    // its second season, so this is not a rare edge case — it is the
    // primary workflow. The only question that means anything for an
    // EXISTING record is whether Sonarr has ever actually refreshed it
    // (record_refreshed(): episodes populated), independent of the
    // monitored shape.
    AddSonarr s;
    s.already_added = true;
    s.already_added_all_monitored = true;
    auto added = s.add_series(1396, 1, true);
    REQUIRE(added.ok);
    CHECK(added.settled);
    REQUIRE_FALSE(added.series.seasons.empty());  // NOT cleared — this is real data
    CHECK(added.series.seasons.size() == 6);
    CHECK(added.series.seasons[0].monitored);      // the record's REAL shape is
    CHECK(added.series.seasons[1].monitored);      // reported as-is, never
    CHECK(added.series.seasons[2].monitored);      // second-guessed against
    CHECK(added.series.seasons[3].monitored);      // what THIS call requested
    CHECK(added.series.seasons[4].monitored);
    CHECK(added.series.seasons[5].monitored);
    CHECK(s.post_path.empty());  // idempotent: no POST
}

TEST_CASE("add_settled rejects a one-regular-season-plus-specials "
          "mid-refresh state (specials must not be monitored too)",
          "[sonarr][add]") {
    // IMPORTANT bug found in review: a show with exactly ONE regular season
    // plus specials has a mid-refresh all-monitored state of {S0 monitored,
    // S1 monitored} — monitored_non_special == 1 there too, so a predicate
    // that checks only the non-special count settles one poll early and
    // reports specials monitored when firstSeason will unmonitor them
    // (live-verified in Phase 2a: firstSeason left "S2-5 AND specials"
    // unmonitored). Exercised directly against add_settled — exposed in the
    // header specifically so shapes like this can be table-tested without a
    // fixture + HTTP stub for each one.
    mb::Series mid_refresh;
    mid_refresh.sonarr_id = 42;
    mid_refresh.seasons = {
        {0, /*monitored=*/true, /*episode_count=*/3, 0, 0},   // Specials — still armed
        {1, /*monitored=*/true, /*episode_count=*/10, 0, 0},  // the only regular season
    };
    CHECK_FALSE(mb::add_settled(mid_refresh, /*monitor=*/true));

    // The actually-settled shape for the same show: specials unmonitored,
    // the one regular season monitored.
    mb::Series settled = mid_refresh;
    settled.seasons[0].monitored = false;
    CHECK(mb::add_settled(settled, /*monitor=*/true));
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
        long http_delete(const std::string&) override { return 200; }
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
    long http_delete(const std::string& path) override {
        delete_path = path;
        return 200;
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

// --- queue ---------------------------------------------------------------

namespace {
class QueueSonarr : public mb::SonarrClient {
public:
    QueueSonarr() : SonarrClient(Config{}) {}
    std::vector<std::string> gets;
    std::string delete_path;
    std::string http_get(const std::string& path) override {
        gets.push_back(path);
        if (path.rfind("/api/v3/queue", 0) == 0) return read_fixture("queue.json");
        if (path.rfind("/api/v3/history/series", 0) == 0) {
            return read_fixture("history_series.json");
        }
        return "";
    }
    std::string http_post(const std::string&, const std::string&) override { return ""; }
    std::string http_put(const std::string&, const std::string&) override { return ""; }
    long http_delete(const std::string& path) override {
        delete_path = path;
        return 200;
    }
};
}  // namespace

TEST_CASE("get_queue requests embedded episodes and returns them ungrouped",
          "[sonarr][queue]") {
    QueueSonarr s;
    auto q = s.get_queue();

    // One page: the fixture's 3 records are a short page against the default
    // pageSize of 100, so the loop stops without a second request.
    REQUIRE(s.gets.size() == 1);
    CHECK(s.gets[0].rfind("/api/v3/queue?", 0) == 0);
    CHECK(s.gets[0].find("page=1") != std::string::npos);
    CHECK(s.gets[0].find("pageSize=100") != std::string::npos);
    // includeEpisode=true gets the S02E01 label in the same round-trip;
    // Phase 2c's grouped row needs it to say "3 episodes" with names.
    CHECK(s.gets[0].find("includeEpisode=true") != std::string::npos);

    // Three episode rows for ONE season pack, deliberately NOT collapsed.
    // Grouping by download_id is Phase 2c's UI concern; the client must not
    // pre-empt it, because DELETE acts on the whole download and the UI needs
    // to know which ids are siblings.
    REQUIRE(q.size() == 3);
    CHECK(q[0].download_id == q[2].download_id);
    CHECK(q[0].season_number == 2);
    CHECK(q[0].episode.episode_number == 1);
    CHECK(q[2].episode.episode_number == 3);
}

TEST_CASE("get_queue pages until the queue is exhausted", "[sonarr][queue]") {
    // Sonarr's queue is per EPISODE, so a single 20-episode season pack is 20
    // records and ~5 concurrent packs saturate a 100-record page. Truncating
    // silently looks exactly like "that download isn't queued".
    //
    // queue_page_size is configurable precisely so this can be proven without
    // a 100-record fixture: set it to 3 and the existing fixture becomes a
    // FULL page, forcing a second request.
    class PagedSonarr : public mb::SonarrClient {
    public:
        static Config small_pages() {
            Config c;
            c.queue_page_size = 3;
            return c;
        }
        PagedSonarr() : SonarrClient(small_pages()) {}
        std::vector<std::string> gets;
        std::string http_get(const std::string& path) override {
            gets.push_back(path);
            if (path.find("page=1") != std::string::npos) {
                return read_fixture("queue.json");   // 3 records == a full page
            }
            if (path.find("page=2") != std::string::npos) {
                return R"({"page":2,"pageSize":3,"totalRecords":4,"records":[
                  {"id":104,"seriesId":7,"episodeId":5004,"seasonNumber":2,
                   "title":"Breaking.Bad.S02.1080p.WEB-DL.x264-GROUPA",
                   "size":12884901888,"sizeleft":6442450944,"status":"downloading",
                   "downloadId":"A1B2C3D4E5F60718293A4B5C6D7E8F9012345678"}
                ]})";
            }
            return "";
        }
        std::string http_post(const std::string&, const std::string&) override { return ""; }
        std::string http_put(const std::string&, const std::string&) override { return ""; }
        long http_delete(const std::string&) override { return 200; }
    };
    PagedSonarr s;
    auto q = s.get_queue();
    REQUIRE(s.gets.size() == 2);
    CHECK(s.gets[0].find("page=1") != std::string::npos);
    CHECK(s.gets[0].find("pageSize=3") != std::string::npos);
    CHECK(s.gets[1].find("page=2") != std::string::npos);
    REQUIRE(q.size() == 4);
    CHECK(q[3].id == 104);
}

TEST_CASE("get_queue returns empty on transport failure", "[sonarr][queue]") {
    class Dead : public mb::SonarrClient {
    public:
        Dead() : SonarrClient(Config{}) {}
        std::string http_get(const std::string&) override { return ""; }
        std::string http_post(const std::string&, const std::string&) override { return ""; }
        std::string http_put(const std::string&, const std::string&) override { return ""; }
        long http_delete(const std::string&) override { return 200; }
    };
    Dead d;
    CHECK(d.get_queue().empty());
}

TEST_CASE("get_queue_checked distinguishes unreachable from genuinely empty",
          "[sonarr][queue]") {
    // The whole reason this shape exists: QueueScreen's TV worker branches
    // on nullopt (Sonarr didn't answer -> keep showing the last-known
    // groups + a "Sonarr offline" warning) vs an engaged empty vector
    // (Sonarr answered "nothing queued" -> clear the groups for real). {}
    // must never stand in for "the request failed", the same contract as
    // get_library_checked / get_series_download_hashes_checked.
    SECTION("first-page transport failure -> nullopt") {
        class Dead : public mb::SonarrClient {
        public:
            Dead() : SonarrClient(Config{}) {}
            std::string http_get(const std::string&) override { return ""; }
            std::string http_post(const std::string&, const std::string&) override { return ""; }
            std::string http_put(const std::string&, const std::string&) override { return ""; }
            long http_delete(const std::string&) override { return 200; }
        };
        Dead d;
        CHECK_FALSE(d.get_queue_checked().has_value());
        // The raw wrapper still collapses nullopt to {}, same as get_queue().
        CHECK(d.get_queue().empty());
    }
    SECTION("genuinely empty queue -> engaged optional, empty vector") {
        class EmptyQueueSonarr : public mb::SonarrClient {
        public:
            EmptyQueueSonarr() : SonarrClient(Config{}) {}
            std::string http_get(const std::string& path) override {
                if (path.rfind("/api/v3/queue", 0) == 0) {
                    return R"({"page":1,"pageSize":100,"totalRecords":0,"records":[]})";
                }
                return "";
            }
            std::string http_post(const std::string&, const std::string&) override { return ""; }
            std::string http_put(const std::string&, const std::string&) override { return ""; }
            long http_delete(const std::string&) override { return 200; }
        };
        EmptyQueueSonarr s;
        auto checked = s.get_queue_checked();
        REQUIRE(checked.has_value());
        CHECK(checked->empty());
    }
    SECTION("populated queue agrees with the raw wrapper") {
        QueueSonarr s;
        auto checked = s.get_queue_checked();
        REQUIRE(checked.has_value());
        auto raw = s.get_queue();
        REQUIRE(checked->size() == raw.size());
        REQUIRE(checked->size() == 3);
        CHECK((*checked)[0].id == raw[0].id);
        CHECK((*checked)[0].download_id == raw[0].download_id);
        CHECK((*checked)[2].episode.episode_number == raw[2].episode.episode_number);
    }
}

TEST_CASE("get_queue_checked treats a mid-paging failure as nullopt, "
          "never a truncated success", "[sonarr][queue]") {
    // Contrast with the historical get_queue() behaviour (still exercised by
    // the "pages until the queue is exhausted" test above): a failure on
    // page 2 used to keep page 1's rows and warn. For the checked variant
    // that is the exact hazard queue_groups.h documents — a season pack's
    // sibling rows split across "returned" and "lost" would render as if
    // the whole pack were present. Page 1 succeeding is not enough; ANY
    // page failing must nullopt the whole call.
    class PagedThenDead : public mb::SonarrClient {
    public:
        static Config small_pages() {
            Config c;
            c.queue_page_size = 3;
            return c;
        }
        PagedThenDead() : SonarrClient(small_pages()) {}
        std::vector<std::string> gets;
        std::string http_get(const std::string& path) override {
            gets.push_back(path);
            if (path.find("page=1") != std::string::npos) {
                return read_fixture("queue.json");   // 3 records == a full page
            }
            // page=2 (and any later page) fails — Gluetun re-link mid-poll.
            return "";
        }
        std::string http_post(const std::string&, const std::string&) override { return ""; }
        std::string http_put(const std::string&, const std::string&) override { return ""; }
        long http_delete(const std::string&) override { return 200; }
    };
    PagedThenDead s;
    auto checked = s.get_queue_checked();
    REQUIRE(s.gets.size() == 2);
    CHECK_FALSE(checked.has_value());
    // The raw wrapper collapses this to {} too — never a truncated 3-row
    // result silently passed off as the whole queue.
    CHECK(s.get_queue().empty());
}

TEST_CASE("cancel_queue_item removes the whole download from the client",
          "[sonarr][queue]") {
    // Live-proven: DELETE /api/v3/queue/{id}?removeFromClient=true kills the
    // entire download; the sibling episode rows then 404. blocklist=false so a
    // user-initiated cancel does not poison the release for a later retry.
    QueueSonarr s;
    REQUIRE(s.cancel_queue_item(101));
    CHECK(s.delete_path == "/api/v3/queue/101?removeFromClient=true&blocklist=false");
}

// --- history -------------------------------------------------------------

TEST_CASE("get_series_download_hashes walks the series history",
          "[sonarr][history]") {
    // Powers the orphan-proof remove: the active queue only knows in-progress
    // downloads, so finished-and-seeding torrents would be left behind in
    // qBittorrent without this.
    QueueSonarr s;
    auto hashes = s.get_series_download_hashes(7);
    REQUIRE(s.gets.size() == 1);
    CHECK(s.gets[0] == "/api/v3/history/series?seriesId=7");
    REQUIRE(hashes.size() == 2);
    CHECK(hashes[0] == "a1b2c3d4e5f60718293a4b5c6d7e8f9012345678");
    CHECK(hashes[1] == "ffeeddccbbaa99887766554433221100aabbccdd");
}

TEST_CASE("get_series_download_hashes_checked distinguishes empty from failed",
          "[sonarr][history]") {
    // Task 8's orphan-proof remove worker reads THIS return alone to decide
    // whether to abort — never a follow-up last_error() call, because Task
    // 8's background re-poll runs on its own thread and shares this
    // client's one last_error_ member. nullopt must mean "Sonarr did not
    // answer"; an engaged vector, even empty, must mean it genuinely did
    // (same contract as get_library_checked, pinned the same way).
    SECTION("HTTP failure -> nullopt") {
        StubSonarr s;  // no replies configured -> http_get returns ""
        CHECK_FALSE(s.get_series_download_hashes_checked(7).has_value());
        // The raw wrapper collapses nullopt to {}, same as get_library().
        CHECK(s.get_series_download_hashes(7).empty());
    }
    SECTION("genuinely no history -> engaged optional, empty vector") {
        StubSonarr s;
        s.get_replies = {{"/api/v3/history/series?seriesId=7", "[]"}};
        auto hashes = s.get_series_download_hashes_checked(7);
        REQUIRE(hashes.has_value());
        CHECK(hashes->empty());
    }
    SECTION("populated history") {
        StubSonarr s;
        s.get_replies = {{"/api/v3/history/series?seriesId=7",
                          read_fixture("history_series.json")}};
        auto hashes = s.get_series_download_hashes_checked(7);
        REQUIRE(hashes.has_value());
        REQUIRE(hashes->size() == 2);
        CHECK((*hashes)[0] == "a1b2c3d4e5f60718293a4b5c6d7e8f9012345678");
        CHECK((*hashes)[1] == "ffeeddccbbaa99887766554433221100aabbccdd");
    }
}

// --- mock ----------------------------------------------------------------

TEST_CASE("SonarrMockClient seeds a coherent season pack", "[sonarr][mock]") {
    mb::SonarrMockClient m;
    auto q = m.get_queue();
    // Three episode rows sharing one downloadId — the shape 2c's grouping has
    // to handle, available on a dev machine with no services running.
    REQUIRE(q.size() == 3);
    CHECK(q[0].download_id == q[1].download_id);
    CHECK(q[1].download_id == q[2].download_id);
    CHECK(q[0].series_id == 1);

    auto hashes = m.get_series_download_hashes(1);
    REQUIRE(hashes.size() == 1);
    // Mock hashes come back lowercase, like the real client's.
    CHECK(hashes[0] == q[0].download_id);

    REQUIRE(m.cancel_queue_item(q[0].id));
    // Cancelling one row removes the whole download, matching live behaviour.
    CHECK(m.get_queue().empty());
}

TEST_CASE("SonarrMockClient's get_series_download_hashes_checked stays "
          "coherent with the raw variant", "[sonarr][mock]") {
    // Unlike get_library_checked (deliberately always nullopt — see the
    // dedicated test above for why), this mock has no field-observed
    // reachability lie to defend against here, so the checked override is
    // engaged and must agree exactly with the raw wrapper it backs.
    mb::SonarrMockClient m;
    auto checked = m.get_series_download_hashes_checked(1);
    REQUIRE(checked.has_value());
    REQUIRE(checked->size() == 1);
    CHECK((*checked)[0] == "a1b2c3d4e5f60718293a4b5c6d7e8f9012345678");
    CHECK(*checked == m.get_series_download_hashes(1));
}

TEST_CASE("SonarrMockClient serves fixture-shaped quality definitions",
          "[sonarr][mock]") {
    mb::SonarrMockClient m;
    auto defs = m.get_quality_definitions();
    REQUIRE(defs.size() == 2);
    CHECK(media_browser::ui::pick_preferred_mb_per_min(defs) == 70.0);
}

// --- episodes ------------------------------------------------------------

namespace {
// GET /api/v3/episode?seriesId=&includeEpisodeFile=true, live-verified
// 2026-08-02 against Sonarr 4.0.19:
//   - hasFile=false records carry NO `episodeFile` key and episodeFileId==0
//     (S1E1 below pins both).
//   - the episodeFile embed, when present, has `path` (container-absolute
//     /data/library/tv/...), `size`, and quality.quality.name. NOTE: this
//     embedded shape is from Sonarr's docs, not a live import (zero files
//     existed at design time) — Task 9 re-verifies it against the first real
//     import and updates this fixture if reality differs.
//   - `runtime` may be 0 on specials (S0E1 below); stored as-is, callers
//     fall back to the series runtime.
// Records are DELIBERATELY out of (season, episode) order so the sort
// contract is actually exercised, and a season-0 special is present to pin
// that the client does NOT filter it (policy lives in the UI, which excludes
// specials to match merge_season_rows).
const char* kEpisodesFixture = R"JSON([
  {
    "id": 12,
    "seriesId": 7,
    "seasonNumber": 1,
    "episodeNumber": 2,
    "title": "Cat's in the Bag...",
    "airDate": "2008-01-27",
    "runtime": 48,
    "hasFile": true,
    "monitored": true,
    "episodeFileId": 101,
    "episodeFile": {
      "id": 101,
      "seriesId": 7,
      "path": "/data/library/tv/Breaking Bad/Season 1/Breaking Bad - S01E02.mkv",
      "size": 1234567890,
      "quality": {
        "quality": { "id": 3, "name": "WEBDL-1080p" },
        "revision": { "version": 1, "real": 0, "isRepack": false }
      }
    }
  },
  {
    "id": 30,
    "seriesId": 7,
    "seasonNumber": 0,
    "episodeNumber": 1,
    "title": "Good Cop / Bad Cop",
    "airDate": "2009-02-17",
    "runtime": 0,
    "hasFile": false,
    "monitored": false,
    "episodeFileId": 0
  },
  {
    "id": 11,
    "seriesId": 7,
    "seasonNumber": 1,
    "episodeNumber": 1,
    "title": "Pilot",
    "airDate": "2008-01-20",
    "runtime": 58,
    "hasFile": false,
    "monitored": true,
    "episodeFileId": 0
  }
])JSON";
}  // namespace

TEST_CASE("parse_episode_list extracts fields, sorts by (season, episode), "
          "and defaults the no-file shape", "[sonarr][episodes]") {
    auto eps = mb::SonarrClient::parse_episode_list(kEpisodesFixture);
    REQUIRE(eps.size() == 3);

    // Sorted by (season_number, episode_number) — next_up assumes it — and
    // season 0 is NOT filtered here (the client stays policy-free).
    CHECK(eps[0].season_number == 0);
    CHECK(eps[0].episode_number == 1);
    CHECK(eps[1].season_number == 1);
    CHECK(eps[1].episode_number == 1);
    CHECK(eps[2].season_number == 1);
    CHECK(eps[2].episode_number == 2);

    // The special: runtime 0 stored AS-IS, never invented.
    CHECK(eps[0].id == 30);
    CHECK(eps[0].title == "Good Cop / Bad Cop");
    CHECK(eps[0].runtime_minutes == 0);
    CHECK_FALSE(eps[0].monitored);

    // hasFile=false: no episodeFile key, episodeFileId 0 — every file field
    // stays at its empty/0 default.
    CHECK(eps[1].id == 11);
    CHECK(eps[1].title == "Pilot");
    CHECK(eps[1].air_date == "2008-01-20");
    CHECK(eps[1].runtime_minutes == 58);
    CHECK(eps[1].monitored);
    CHECK_FALSE(eps[1].has_file);
    CHECK(eps[1].episode_file_id == 0);
    CHECK(eps[1].file_container_path.empty());
    CHECK(eps[1].file_size_bytes == 0);
    CHECK(eps[1].file_quality.empty());

    // hasFile=true: the full embed maps across.
    CHECK(eps[2].id == 12);
    CHECK(eps[2].title == "Cat's in the Bag...");
    CHECK(eps[2].air_date == "2008-01-27");
    CHECK(eps[2].runtime_minutes == 48);
    CHECK(eps[2].has_file);
    CHECK(eps[2].monitored);
    CHECK(eps[2].episode_file_id == 101);
    CHECK(eps[2].file_container_path ==
          "/data/library/tv/Breaking Bad/Season 1/Breaking Bad - S01E02.mkv");
    CHECK(eps[2].file_size_bytes == 1234567890LL);
    CHECK(eps[2].file_quality == "WEBDL-1080p");
}

TEST_CASE("get_episodes_checked distinguishes unreachable from empty",
          "[sonarr][episodes]") {
    // Same contract as get_library_checked, for the same reason: Sonarr rides
    // Gluetun's netns, so transport blips are routine, and "{} because the
    // fetch failed" must never read as "this series has no episodes".
    SECTION("transport failure → nullopt") {
        StubSonarr s;  // no replies configured → http_get returns ""
        CHECK_FALSE(s.get_episodes_checked(7).has_value());
        REQUIRE(s.gets.size() == 1);
        CHECK(s.gets[0] == "/api/v3/episode?seriesId=7&includeEpisodeFile=true");
    }
    SECTION("genuinely empty episode list → engaged optional, empty vector") {
        StubSonarr s;
        s.get_replies = {{"/api/v3/episode?seriesId=7", "[]"}};
        auto eps = s.get_episodes_checked(7);
        REQUIRE(eps.has_value());
        CHECK(eps->empty());
    }
    SECTION("populated") {
        StubSonarr s;
        s.get_replies = {{"/api/v3/episode?seriesId=7", kEpisodesFixture}};
        auto eps = s.get_episodes_checked(7);
        REQUIRE(eps.has_value());
        CHECK(eps->size() == 3);
    }
    SECTION("malformed body → engaged-EMPTY, not nullopt (accepted "
            "misclassification, exactly like get_library_checked)") {
        // A non-empty body means transport succeeded, so it goes through the
        // parser; an unparseable body then parses to an empty vector. That
        // misreads "Sonarr answered garbage" as "no episodes" — accepted,
        // because it mirrors get_library_checked verbatim and a divergent
        // error discipline between the two checked variants would be worse
        // than the shared, known quirk.
        StubSonarr s;
        s.get_replies = {{"/api/v3/episode?seriesId=7", "not json {{{"}};
        auto eps = s.get_episodes_checked(7);
        REQUIRE(eps.has_value());
        CHECK(eps->empty());
        CHECK(mb::SonarrClient::parse_episode_list("not json {{{").empty());
        // Non-array JSON (an error object) is the same engaged-empty.
        CHECK(mb::SonarrClient::parse_episode_list(R"({"error":"x"})").empty());
    }
}

TEST_CASE("SonarrMockClient's get_episodes_checked reports unreachable, "
          "matching a real box with no Sonarr configured", "[sonarr][mock]") {
    // Same honesty rule as get_library_checked's pin test above: the mock
    // stands in for an ABSENT Sonarr (main.cpp falls back to it when the API
    // key is empty), so the checked shape must answer "did not answer" —
    // fabricating an episode list would hand SeriesDetail a fake picker on a
    // box whose owner has no Sonarr at all.
    mb::SonarrMockClient m;
    CHECK_FALSE(m.get_episodes_checked(1).has_value());
}
