#include <catch2/catch_test_macros.hpp>
#include <json/json.h>
#include "media_browser/radarr/radarr_client.h"

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
        std::string http_get(const std::string& path) override {
            REQUIRE(path == "/api/v3/release?movieId=99");
            return R"([
              {"guid":"magnet:?xt=urn:btih:abc","indexerId":7,"title":"R1","seeders":200,"size":2147483648},
              {"guid":"magnet:?xt=urn:btih:def","indexerId":7,"title":"R2","seeders":50,"size":943718400}
            ])";
        }
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
