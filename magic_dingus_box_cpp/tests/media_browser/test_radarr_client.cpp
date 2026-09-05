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
    long http_delete(const std::string&) override { return 200; }
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
        long http_delete(const std::string&) override { return 200; }
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
        long http_delete(const std::string&) override { return 200; }
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

namespace {
class CheckedQueueRadarr : public mb::RadarrClient {
public:
    explicit CheckedQueueRadarr(bool fail)
        : RadarrClient(Config{}), fail_(fail) {}

    void seed_stale_error() { set_error("Radarr lookup failed for tmdb:558"); }

protected:
    std::string http_get(const std::string& path) override {
        REQUIRE(path == "/api/v3/queue?pageSize=100");
        if (fail_) {
            set_error("HTTP 503");
            return {};
        }
        return R"({"page":1,"pageSize":100,"totalRecords":0,"records":[]})";
    }

private:
    bool fail_;
};

class MalformedQueueRadarr : public mb::RadarrClient {
public:
    MalformedQueueRadarr() : RadarrClient(Config{}) {}

protected:
    std::string http_get(const std::string& path) override {
        REQUIRE(path == "/api/v3/queue?pageSize=100");
        return "<html>proxy error</html>";
    }
};

class RetryingAddRadarr : public mb::RadarrClient {
public:
    RetryingAddRadarr(int success_after_ms, int retry_window_ms)
        : RadarrClient(make_config(retry_window_ms)),
          success_after_ms_(success_after_ms) {}

    int lookup_calls = 0;
    int post_calls = 0;
    int fake_elapsed_ms = 0;

protected:
    HttpGetResult http_get_result(const std::string& path,
                                  int timeout_secs) override {
        REQUIRE(path == "/api/v3/movie/lookup?term=tmdb:558");
        REQUIRE(timeout_secs == 5);
        ++lookup_calls;
        if (fake_elapsed_ms < success_after_ms_) {
            // Simulate another worker overwriting the shared diagnostic after
            // this request obtained its own failure cause.
            set_error("unrelated shared queue error");
            return {{}, "HTTP 503"};
        }
        return {R"({"title":"Spider-Man 2","tmdbId":558})", {}};
    }

    std::string http_get(const std::string& path) override {
        if (path == "/api/v3/rootfolder") {
            return R"([{"id":1,"path":"/data/library","freeSpace":500000000000}])";
        }
        FAIL("Unexpected Radarr GET: " << path);
        return {};
    }

    std::string http_post(const std::string& path,
                          const std::string& /*body*/) override {
        REQUIRE(path == "/api/v3/movie");
        ++post_calls;
        return R"({"id":42})";
    }

    std::chrono::steady_clock::time_point metadata_retry_now() const override {
        return std::chrono::steady_clock::time_point{
            std::chrono::milliseconds(fake_elapsed_ms)};
    }

    void wait_for_metadata_retry(std::chrono::milliseconds delay) override {
        fake_elapsed_ms += static_cast<int>(delay.count());
    }

private:
    static Config make_config(int retry_window_ms) {
        Config cfg;
        cfg.metadata_lookup_retry_window_ms = retry_window_ms;
        cfg.metadata_lookup_retry_delay_ms = 1000;
        return cfg;
    }

    int success_after_ms_;
};
}  // namespace

TEST_CASE("Radarr checked queue distinguishes a successful empty queue from failure",
          "[radarr][queue]") {
    CheckedQueueRadarr empty_queue(/*fail=*/false);
    empty_queue.seed_stale_error();
    auto answered = empty_queue.get_queue_checked();
    REQUIRE(answered.has_value());
    CHECK(answered->empty());
    // The shared diagnostic may still belong to an unrelated operation;
    // callers must trust the engaged result rather than pairing the two.
    CHECK(empty_queue.last_error() == "Radarr lookup failed for tmdb:558");

    CheckedQueueRadarr unavailable(/*fail=*/true);
    auto failed = unavailable.get_queue_checked();
    CHECK_FALSE(failed.has_value());
    CHECK(unavailable.last_error() == "HTTP 503");
}

TEST_CASE("Radarr checked queue rejects a malformed successful response",
          "[radarr][queue]") {
    MalformedQueueRadarr radarr;
    CHECK_FALSE(radarr.get_queue_checked().has_value());
    CHECK(radarr.last_error() == "Invalid Radarr queue response");
}

TEST_CASE("Radarr add retries metadata lookup but POSTs the movie only once",
          "[radarr][add][retry]") {
    // Production evidence: Gluetun recovered 32 seconds after the outage
    // began. Immediate HTTP failures must not burn through the retry budget
    // before that recovery point.
    RetryingAddRadarr radarr(/*success_after_ms=*/32000,
                             /*retry_window_ms=*/45000);

    REQUIRE(radarr.add_movie(/*tmdb_id=*/558, /*quality_profile_id=*/7));
    CHECK(radarr.fake_elapsed_ms >= 32000);
    CHECK(radarr.lookup_calls >= 33);
    CHECK(radarr.post_calls == 1);
    CHECK(radarr.last_error().empty());
}

TEST_CASE("Radarr add reports the metadata cause and never POSTs after retry exhaustion",
          "[radarr][add][retry]") {
    RetryingAddRadarr radarr(/*success_after_ms=*/10000,
                             /*retry_window_ms=*/3000);

    CHECK_FALSE(radarr.add_movie(/*tmdb_id=*/558, /*quality_profile_id=*/7));
    CHECK(radarr.fake_elapsed_ms == 3000);
    CHECK(radarr.lookup_calls == 4);
    CHECK(radarr.post_calls == 0);
    CHECK(radarr.last_error().find("metadata lookup unavailable") !=
          std::string::npos);
    CHECK(radarr.last_error().find("HTTP 503") != std::string::npos);
    CHECK(radarr.last_error().find("unrelated shared queue error") ==
          std::string::npos);
}
