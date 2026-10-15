// Odd-shaped JSON must never throw out of a parser or client method.
// Json::LogicError escaping into a worker thread is std::terminate — the
// whole kiosk dies. Shapes here are ones a half-up service, a proxy error
// page, or an API change could plausibly return.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/radarr/radarr_client.h"
#include "media_browser/radarr/radarr_parsers.h"
#include "media_browser/sonarr/sonarr_client.h"
#include "media_browser/sonarr/sonarr_parsers.h"
#include "media_browser/tmdb_client.h"
#include "media_browser/ui/indexer_toggle_logic.h"

namespace mb = media_browser;

namespace {
const std::vector<std::string> kOddBodies = {
    "[]", "[1,2,3]", "\"records\"", "42", "null", "true",
    R"({"records":"nope"})", R"({"records":[1,"x",null,[]]})",
    R"({"records":{"a":1}})", R"({"results":[1,"x"]})",
    R"([{"images":[1,"x"],"ratings":[1],"movieFile":"x"}])",
    R"({"id":1,"movieFile":{"quality":"x","mediaInfo":[1]}})",
    R"([{"status":"started","name":"MoviesSearch","body":[1]}])",
    R"([{"status":"started","name":"MoviesSearch","body":{"movieIds":["x",{}]}}])",
    R"([{"seasons":[1,"x"],"statistics":[]}])",
    R"({"results":[{"adult":"yes"}],"genres":[1,{"name":{}}]})",
};

// Structurally fine but wrong-TYPED fields (asInt() on a string/object
// throws in jsoncpp). The Radarr/Sonarr parsers let these propagate — only
// their clients call them — and every client method turns it into its
// normal failure value (so a CHECKED read reports failure, never an
// authoritative "empty"). TMDB's display-only parsers catch themselves.
const std::vector<std::string> kTypedBodies = {
    R"([{"id":"abc","title":{"x":1}}])",
    R"({"id":"abc","movieFile":{"quality":"x","mediaInfo":[1]}})",
    R"({"records":[{"id":{"x":1},"sizeleft":"x"}]})",
};

std::vector<std::string> all_bodies() {
    auto v = kOddBodies;
    v.insert(v.end(), kTypedBodies.begin(), kTypedBodies.end());
    return v;
}
}  // namespace

TEST_CASE("Radarr parsers survive odd JSON shapes", "[json][radarr]") {
    for (const auto& b : kOddBodies) {
        INFO(b);
        CHECK_NOTHROW(mb::RadarrParsers::parse_queue(b));
        CHECK_NOTHROW(mb::RadarrParsers::parse_active_searches(b));
        CHECK_NOTHROW(mb::RadarrParsers::parse_quality_profiles(b));
        CHECK_NOTHROW(mb::RadarrParsers::parse_root_folders(b));
        CHECK_NOTHROW(mb::RadarrParsers::parse_system_status(b));
    }
    // The reported crash: an ARRAY root indexed with ["records"].
    CHECK(mb::RadarrParsers::parse_queue("[1,2]").empty());
    // Non-object records are skipped, good ones kept.
    auto q = mb::RadarrParsers::parse_queue(
        R"({"records":[1,"x",{"id":5,"movieId":9,"title":"T"}]})");
    REQUIRE(q.size() == 1);
    CHECK(q[0].id == 5);
    // Structural oddities inside a movie record are tolerated.
    auto m = mb::RadarrParsers::parse_movie_list(
        R"([{"id":3,"title":"M","images":[1,"x"],"ratings":[1],"movieFile":"x"}])");
    REQUIRE(m.size() == 1);
    CHECK(m[0].title == "M");
    auto a = mb::RadarrParsers::parse_active_searches(
        R"([{"status":"started","name":"MoviesSearch","body":{"movieIds":["x",7]}}])");
    CHECK(a.movie_ids.count(7) == 1);
}

TEST_CASE("Sonarr parsers survive odd JSON shapes", "[json][sonarr]") {
    for (const auto& b : kOddBodies) {
        INFO(b);
        CHECK_NOTHROW(mb::SonarrParsers::parse_queue(b));
        CHECK_NOTHROW(mb::SonarrParsers::parse_queue_total(b));
        CHECK_NOTHROW(mb::SonarrParsers::parse_series_lookup(b));
        CHECK_NOTHROW(mb::SonarrParsers::parse_series_list(b));
        CHECK_NOTHROW(mb::SonarrParsers::parse_series(b));
        CHECK_NOTHROW(mb::SonarrParsers::parse_quality_definitions(b));
        CHECK_NOTHROW(mb::SonarrParsers::parse_download_client_config(b));
    }
    CHECK(mb::SonarrParsers::parse_queue("[1,2]").empty());
    auto s = mb::SonarrParsers::parse_series_list(
        R"([1,{"id":4,"title":"S","seasons":[1,{"seasonNumber":2}],"images":["x"]}])");
    REQUIRE(s.size() == 1);
    CHECK(s[0].seasons.size() == 1);
}

TEST_CASE("TMDB parsers survive odd JSON shapes", "[json][tmdb]") {
    for (const auto& b : all_bodies()) {
        INFO(b);
        CHECK_NOTHROW(mb::TmdbClient::parse_search_response(b));
        CHECK_NOTHROW(mb::TmdbClient::parse_list_response(b));
        CHECK_NOTHROW(mb::TmdbClient::parse_list(b));
        CHECK_NOTHROW(mb::TmdbClient::parse_tv_list(b));
        CHECK_NOTHROW(mb::TmdbClient::parse_genres_response(b));
        CHECK_NOTHROW(mb::TmdbClient::parse_movie_detail(b));
        CHECK_NOTHROW(mb::TmdbClient::parse_tv_detail(b));
    }
    // Wrong-typed field (asInt on a string) degrades to "no data".
    CHECK_FALSE(mb::TmdbClient::parse_list(R"({"results":[{"id":{}}]})").ok);
    CHECK_FALSE(mb::TmdbClient::parse_movie_detail(R"({"id":{"x":1}})").has_value());
    auto l = mb::TmdbClient::parse_list(
        R"({"results":[1,"x",{"id":5,"title":"A"}],"total_pages":1})");
    REQUIRE(l.ok);
    REQUIRE(l.hits.size() == 1);
    CHECK(l.hits[0].tmdb_id == 5);
}

TEST_CASE("TmdbClient cancel token short-circuits every request",
          "[tmdb][cancel]") {
    mb::TmdbClient c("dummy-key");
    std::atomic<bool> cancel{true};
    mb::TmdbClient::ScopedCancel guard(&cancel);
    const auto t0 = std::chrono::steady_clock::now();
    auto r = c.get_similar(603);
    auto r2 = c.get_recommendations(603);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
    CHECK(r.hits.empty());
    CHECK(r2.hits.empty());
    CHECK(c.last_error() == "cancelled");
}

namespace {
class ShapeRadarr : public mb::RadarrClient {
public:
    explicit ShapeRadarr(std::string body) : RadarrClient(Config{}), body_(std::move(body)) {}
    std::string http_get(const std::string&) override { return body_; }
    std::string http_get_long(const std::string&, int) override { return body_; }
    std::string http_post(const std::string&, const std::string&) override { return ""; }
    long http_delete(const std::string&) override { return 200; }
private:
    std::string body_;
};

class ShapeSonarr : public mb::SonarrClient {
public:
    explicit ShapeSonarr(std::string body) : SonarrClient(Config{}), body_(std::move(body)) {}
    std::string http_get(const std::string&) override { return body_; }
    std::string http_post(const std::string&, const std::string&) override { return ""; }
    std::string http_put(const std::string&, const std::string&) override { return ""; }
    long http_delete(const std::string&) override { return 200; }
private:
    std::string body_;
};
}  // namespace

TEST_CASE("Radarr client methods survive odd shapes", "[json][radarr]") {
    for (const auto& b : all_bodies()) {
        INFO(b);
        ShapeRadarr r(b);
        CHECK_NOTHROW(r.get_history(1));   // root["records"] on an array root
        CHECK_NOTHROW(r.get_queue_checked());
        CHECK_NOTHROW(r.get_releases_for_movie(1));
        CHECK_NOTHROW(r.get_movie_download_hashes(1));
        CHECK_NOTHROW(r.get_library_checked());
        CHECK_NOTHROW(r.get_movie(1));
        CHECK_NOTHROW(r.get_active_searches());
    }
    ShapeRadarr arr("[1,2,3]");
    CHECK(arr.get_history(1).empty());
    // Non-object releases are dropped (callers .get() on each).
    ShapeRadarr rel(R"([1,"x",{"title":"R1"}])");
    auto rs = rel.get_releases_for_movie(1);
    REQUIRE(rs.size() == 1);
    CHECK(rs[0]["title"].asString() == "R1");
    // A wrong-typed field fails the CHECKED read (nullopt), never "empty".
    ShapeRadarr typed(R"({"records":[{"id":"abc"}]})");
    CHECK_FALSE(typed.get_queue_checked().has_value());
}

TEST_CASE("Sonarr checked reads map a wrong-typed body to failure, not "
          "'authoritatively empty'", "[json][sonarr]") {
    for (const auto& b : all_bodies()) {
        INFO(b);
        ShapeSonarr s(b);
        CHECK_NOTHROW(s.get_season_history_checked(1, 1));
        CHECK_NOTHROW(s.get_series_download_hashes_checked(1));
        CHECK_NOTHROW(s.get_episode_files_checked(1));
        CHECK_NOTHROW(s.get_episodes_checked(1));
        CHECK_NOTHROW(s.get_library_checked());
    }
    // season history drives the destructive delete-season flow: a record
    // whose id is the wrong type must ABORT (nullopt), not read as "no
    // history" (which would delete files with nothing blocklisted).
    ShapeSonarr typed(R"([{"id":{"x":1},"eventType":"grabbed","downloadId":"AB"}])");
    CHECK_FALSE(typed.get_season_history_checked(1, 1).has_value());
}

namespace {
struct Row { int id; bool enabled; bool has_stats; int result_count; };
}

TEST_CASE("apply_indexer_toggle re-sorts by id and follows the row",
          "[mb_settings][indexers]") {
    std::vector<Row> rows = {
        {1, true, true, 10}, {2, true, false, 0}, {3, false, false, 0}};
    // Disable #1 -> it drops to the disabled section.
    int c = mb::ui::apply_indexer_toggle(rows, 1, false, 5);
    REQUIRE(rows[0].id == 2);
    CHECK(rows.back().enabled == false);
    CHECK(rows[c].id == 1);
    CHECK_FALSE(rows[c].enabled);
    // Re-enable #3: follows into the enabled section.
    c = mb::ui::apply_indexer_toggle(rows, 3, true, 5);
    CHECK(rows[c].id == 3);
    CHECK(rows[c].enabled);
    // Unknown id (list reloaded meanwhile): no change, -1.
    auto before = rows.size();
    CHECK(mb::ui::apply_indexer_toggle(rows, 99, true, 5) == -1);
    CHECK(rows.size() == before);
    // Cursor clamps to the visible window.
    std::vector<Row> many;
    for (int i = 1; i <= 8; ++i) many.push_back({i, true, true, 100 - i});
    c = mb::ui::apply_indexer_toggle(many, 2, false, 5);
    CHECK(c == 4);
}
