#include <catch2/catch_test_macros.hpp>
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
