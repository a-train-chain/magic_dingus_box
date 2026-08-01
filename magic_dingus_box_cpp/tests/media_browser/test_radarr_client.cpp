#include <catch2/catch_test_macros.hpp>
#include <json/json.h>
#include "media_browser/radarr/radarr_client.h"
#include "media_browser/radarr/radarr_mock.h"

namespace mb = media_browser;

TEST_CASE("resolve_host_path: translates exact container prefix to host",
          "[radarr][paths]") {
    mb::RadarrClient::Config cfg;
    cfg.container_library_prefix = "/library/";
    cfg.host_library_prefix = "/mnt/ssd/library/";
    mb::RadarrClient c(cfg);
    REQUIRE(c.resolve_host_path("/library/Sintel (2010)/Sintel.mp4") ==
            "/mnt/ssd/library/Sintel (2010)/Sintel.mp4");
}

TEST_CASE("resolve_host_path: rejects /library2 false-prefix",
          "[radarr][paths]") {
    mb::RadarrClient::Config cfg;
    cfg.container_library_prefix = "/library/";
    cfg.host_library_prefix = "/mnt/ssd/library/";
    mb::RadarrClient c(cfg);
    REQUIRE(c.resolve_host_path("/library2/foo.mp4") == "/library2/foo.mp4");
}

TEST_CASE("resolve_host_path: passes through unrecognized paths",
          "[radarr][paths]") {
    mb::RadarrClient::Config cfg;
    cfg.container_library_prefix = "/library/";
    cfg.host_library_prefix = "/mnt/ssd/library/";
    mb::RadarrClient c(cfg);
    REQUIRE(c.resolve_host_path("/data/foo.mp4") == "/data/foo.mp4");
}

TEST_CASE("resolve_host_path: empty path returns empty",
          "[radarr][paths]") {
    mb::RadarrClient::Config cfg;
    mb::RadarrClient c(cfg);
    REQUIRE(c.resolve_host_path("").empty());
}

TEST_CASE("resolve_host_path: normalizes prefix without trailing slash",
          "[radarr][paths]") {
    mb::RadarrClient::Config cfg;
    cfg.container_library_prefix = "/library";       // no trailing slash
    cfg.host_library_prefix      = "/mnt/ssd/library"; // no trailing slash
    mb::RadarrClient c(cfg);
    REQUIRE(c.resolve_host_path("/library/Sintel.mp4") ==
            "/mnt/ssd/library/Sintel.mp4");
    // Critical: /library2 must still NOT match.
    REQUIRE(c.resolve_host_path("/library2/foo.mp4") == "/library2/foo.mp4");
}

TEST_CASE("resolve_host_path: default config maps Radarr's /data/library",
          "[radarr][paths]") {
    // Real deployment: the docker-compose repoints Radarr's root folder to
    // /data/library, so movieFile.path is "/data/library/...". The default
    // container prefix must match that, or every in-library movie fails
    // play_ready() and the Detail page shows only "Pick a source".
    mb::RadarrClient c{mb::RadarrClient::Config{}};  // all defaults
    REQUIRE(c.resolve_host_path(
                "/data/library/Pulp Fiction (1994)/Pulp Fiction.mp4") ==
            "/mnt/ssd/library/Pulp Fiction (1994)/Pulp Fiction.mp4");
}

TEST_CASE("resolve_host_path: still handles legacy /library paths",
          "[radarr][paths]") {
    // The compose mounts BOTH /library and /data/library into the
    // container at the same host dir; an install migrated at a different
    // time may store either. The resolver falls back to the alternate
    // prefix so Play works regardless of which one radarr.db recorded.
    mb::RadarrClient c{mb::RadarrClient::Config{}};  // default = /data/library/
    REQUIRE(c.resolve_host_path("/library/Sintel (2010)/Sintel.mp4") ==
            "/mnt/ssd/library/Sintel (2010)/Sintel.mp4");
    // The false-prefix guard must survive the fallback path too.
    REQUIRE(c.resolve_host_path("/library2/foo.mp4") == "/library2/foo.mp4");
}

namespace {
class RecordingRadarr : public mb::RadarrClient {
public:
    RecordingRadarr() : RadarrClient(Config{}) {}
    std::string last_method, last_path, last_body;
    std::string http_post(const std::string& path,
                          const std::string& body) override {
        last_method = "POST";
        last_path = path;
        last_body = body;
        return R"({"id":42})";
    }
    // Stubs to satisfy other virtuals if needed:
    std::string http_get(const std::string&) override { return ""; }
    std::string http_delete(const std::string&) override { return ""; }
};
}

TEST_CASE("RadarrClient::grab_release POSTs the release object verbatim",
          "[radarr][grab]") {
    RecordingRadarr r;
    Json::Value release;
    release["guid"]      = "magnet:?xt=urn:btih:abc";
    release["indexerId"] = 7;
    release["title"]     = "Inception 2010 1080p WEB-DL x264-GROUPA";
    bool ok = r.grab_release(release);
    REQUIRE(ok);
    REQUIRE(r.last_method == "POST");
    REQUIRE(r.last_path == "/api/v3/release");
    // Body should contain the indexerId we passed.
    REQUIRE(r.last_body.find("\"indexerId\"") != std::string::npos);
}

TEST_CASE("RadarrClient::get_releases_for_movie parses release array",
          "[radarr][grab]") {
    class StubRadarr : public mb::RadarrClient {
    public:
        StubRadarr() : RadarrClient(Config{}) {}
        // get_releases_for_movie uses http_get_long (45s timeout) because
        // Radarr's interactive search is genuinely slow. The test verifies
        // the long-timeout variant is invoked AND that the picker would
        // accept enough time for a real Radarr response.
        std::string http_get_long(const std::string& path, int timeout_secs) override {
            REQUIRE(path == "/api/v3/release?movieId=99");
            REQUIRE(timeout_secs >= 30);  // must outlast Radarr's interactive search
            return R"([
              {"guid":"magnet:?xt=urn:btih:abc","indexerId":7,"title":"R1","seeders":200,"size":2147483648},
              {"guid":"magnet:?xt=urn:btih:def","indexerId":7,"title":"R2","seeders":50,"size":943718400}
            ])";
        }
        std::string http_get(const std::string&) override { return ""; }
        std::string http_post(const std::string&, const std::string&) override { return ""; }
        std::string http_delete(const std::string&) override { return ""; }
    };
    StubRadarr r;
    auto releases = r.get_releases_for_movie(99);
    REQUIRE(releases.size() == 2);
    REQUIRE(releases[0]["title"].asString() == "R1");
    REQUIRE(releases[0]["seeders"].asInt() == 200);
    REQUIRE(releases[1]["guid"].asString() == "magnet:?xt=urn:btih:def");
}

TEST_CASE("RadarrClient::get_history parses history records",
          "[radarr][history]") {
    class StubRadarr : public mb::RadarrClient {
    public:
        StubRadarr() : RadarrClient(Config{}) {}
        std::string http_get(const std::string& path) override {
            REQUIRE(path.find("/api/v3/history") == 0);
            REQUIRE(path.find("movieId=99") != std::string::npos);
            return R"({"records":[
              {"id":1,"movieId":99,"eventType":"grabbed",
               "sourceTitle":"Inception 2010 1080p WEB-DL x264","date":"2026-05-01T10:00:00Z"},
              {"id":2,"movieId":99,"eventType":"downloadFailed",
               "sourceTitle":"Inception 2010 1080p WEB-DL x264","date":"2026-05-01T10:30:00Z"}
            ]})";
        }
        std::string http_post(const std::string&, const std::string&) override { return ""; }
        std::string http_delete(const std::string&) override { return ""; }
    };
    StubRadarr r;
    auto events = r.get_history(99);
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].event_type == "grabbed");
    REQUIRE(events[1].event_type == "downloadFailed");
    REQUIRE(events[1].movie_id == 99);
}

TEST_CASE("RadarrMockClient::get_library_checked reports success with canned data",
          "[radarr][mock]") {
    media_browser::RadarrMockClient mock;
    auto checked = mock.get_library_checked();
    REQUIRE(checked.has_value());
    CHECK(checked->size() == mock.get_library().size());
}
