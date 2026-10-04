// utils::services_env — the one reader for /opt/magic_dingus_box/services/.env.
//
// There used to be three hand-rolled copies with three different notions of
// a value: main.cpp's API-key reader kept surrounding quotes (so a
// RADARR_API_KEY="abc" line authenticated with the literal `"abc"`), while
// the two WIREGUARD_PRIVATE_KEY probes stripped any run of quote characters
// from either end. These cases pin the single, docker-compose-compatible
// rule that replaced them.

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

#include "utils/services_env.h"

using utils::env_value_from_stream;
using utils::read_env_value;

namespace {

std::string val(const std::string& text, const std::string& key) {
    std::istringstream in(text);
    return env_value_from_stream(in, key);
}

}  // namespace

TEST_CASE("services_env: plain KEY=value", "[services_env]") {
    CHECK(val("RADARR_API_KEY=abc123\n", "RADARR_API_KEY") == "abc123");
}

TEST_CASE("services_env: missing key is empty", "[services_env]") {
    CHECK(val("OTHER=1\n", "RADARR_API_KEY").empty());
    CHECK(val("", "RADARR_API_KEY").empty());
}

TEST_CASE("services_env: key must match whole name at line start",
          "[services_env]") {
    // A prefix of another key, a commented-out line, or an indented line
    // is not a definition.
    CHECK(val("RADARR_API_KEY_OLD=x\n", "RADARR_API_KEY").empty());
    CHECK(val("# RADARR_API_KEY=x\n", "RADARR_API_KEY").empty());
    CHECK(val("XRADARR_API_KEY=x\n", "RADARR_API_KEY").empty());
}

TEST_CASE("services_env: CRLF and trailing whitespace are trimmed",
          "[services_env]") {
    CHECK(val("K=abc\r\n", "K") == "abc");
    CHECK(val("K=abc  \t\n", "K") == "abc");
    CHECK(val("K=  abc\n", "K") == "abc");
}

TEST_CASE("services_env: one matching pair of quotes is stripped",
          "[services_env]") {
    CHECK(val("K=\"abc\"\n", "K") == "abc");
    CHECK(val("K='abc'\n", "K") == "abc");
    CHECK(val("K= \"abc\" \r\n", "K") == "abc");
    // Inner content is kept verbatim (base64 keys end in '=').
    CHECK(val("K=\"ab=c=\"\n", "K") == "ab=c=");
}

TEST_CASE("services_env: mismatched or lone quotes are left alone",
          "[services_env]") {
    CHECK(val("K=\"abc\n", "K") == "\"abc");
    CHECK(val("K=\"abc'\n", "K") == "\"abc'");
    CHECK(val("K=\"\n", "K") == "\"");
}

TEST_CASE("services_env: empty and quoted-empty values read as empty",
          "[services_env]") {
    CHECK(val("K=\n", "K").empty());
    CHECK(val("K=\"\"\n", "K").empty());
    CHECK(val("K=''\n", "K").empty());
    CHECK(val("K=   \n", "K").empty());
}

TEST_CASE("services_env: first definition wins", "[services_env]") {
    CHECK(val("K=first\nK=second\n", "K") == "first");
    // Even an empty first definition — matches every previous copy.
    CHECK(val("K=\nK=second\n", "K").empty());
}

TEST_CASE("services_env: unreadable file is empty, not an error",
          "[services_env]") {
    CHECK(read_env_value("/nonexistent/dir/.env", "K").empty());
}

TEST_CASE("services_env: reads a real file", "[services_env]") {
    char tmpl[] = "/tmp/mdb_services_env_XXXXXX";
    const int fd = mkstemp(tmpl);
    REQUIRE(fd >= 0);
    close(fd);
    {
        std::ofstream f(tmpl);
        f << "WIREGUARD_PRIVATE_KEY=\"aGVsbG8=\"\nRADARR_API_KEY=r1\n";
    }
    CHECK(read_env_value(tmpl, "WIREGUARD_PRIVATE_KEY") == "aGVsbG8=");
    CHECK(read_env_value(tmpl, "RADARR_API_KEY") == "r1");
    std::remove(tmpl);
}
