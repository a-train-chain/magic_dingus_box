#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "media_browser/radarr/radarr_parsers.h"

namespace fs = std::filesystem;

static std::string read_fixture(const std::string& name) {
    fs::path p = fs::path(__FILE__).parent_path() / "fixtures" / "radarr" / name;
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

TEST_CASE("parse_movie_lookup: extracts 2 hits from fixture", "[radarr][parsers]") {
    auto json = read_fixture("movie_lookup.json");
    auto hits = media_browser::RadarrParsers::parse_movie_lookup(json);
    REQUIRE(hits.size() == 2);
    REQUIRE(hits[0].tmdb_id == 603);
    REQUIRE(hits[0].title == "The Matrix");
    REQUIRE(hits[0].year == 1999);
    REQUIRE(hits[0].runtime_minutes == 136);
    REQUIRE(hits[0].rating == Catch::Approx(8.2));
    REQUIRE(hits[0].imdb_id == "tt0133093");
    REQUIRE(!hits[0].poster_url.empty());
    REQUIRE(!hits[0].fanart_url.empty());
    REQUIRE(hits[1].tmdb_id == 604);
    REQUIRE(hits[1].year == 2003);
}

TEST_CASE("parse_queue: extracts 1 queue item", "[radarr][parsers]") {
    auto json = read_fixture("queue.json");
    auto items = media_browser::RadarrParsers::parse_queue(json);
    REQUIRE(items.size() == 1);
    REQUIRE(items[0].id == 42);
    REQUIRE(items[0].movie_id == 5);
    REQUIRE(items[0].title.find("Sita Sings") != std::string::npos);
    REQUIRE(items[0].size_bytes == 1234567890);
    REQUIRE(items[0].sizeleft_bytes == 600000000);
    REQUIRE(items[0].state == "downloading");
    // Progress derived: (size - sizeleft) / size
    REQUIRE(items[0].progress == Catch::Approx(0.5139).margin(0.01));
    // Live telemetry from new parser fields
    // timeleft "00:15:30" -> 15*60 + 30 = 930 seconds
    REQUIRE(items[0].eta_seconds == 930);
    REQUIRE(items[0].seeds == 42);
    REQUIRE(items[0].peers == 7);
    // Rates intentionally not populated by Radarr's queue shape
    REQUIRE(items[0].download_rate_bps == 0);
    REQUIRE(items[0].upload_rate_bps == 0);
}

TEST_CASE("parse_queue: timeleft parser handles D.HH:MM:SS and bad input",
          "[radarr][parsers]") {
    // Direct test via a synthetic JSON blob — exercise days-prefix path
    // and malformed input to confirm graceful fallback to 0.
    const std::string j = R"({
      "records": [
        { "id": 1, "title": "days",     "timeleft": "2.03:04:05" },
        { "id": 2, "title": "bad",      "timeleft": "not-a-time" },
        { "id": 3, "title": "empty",    "timeleft": "" },
        { "id": 4, "title": "missing" }
      ]
    })";
    auto items = media_browser::RadarrParsers::parse_queue(j);
    REQUIRE(items.size() == 4);
    // 2 days + 3h + 4m + 5s = 183845
    REQUIRE(items[0].eta_seconds == 2 * 86400 + 3 * 3600 + 4 * 60 + 5);
    REQUIRE(items[1].eta_seconds == 0);
    REQUIRE(items[2].eta_seconds == 0);
    REQUIRE(items[3].eta_seconds == 0);
}

TEST_CASE("parse_quality_profiles: extracts profiles", "[radarr][parsers]") {
    auto json = read_fixture("quality_profiles.json");
    auto profiles = media_browser::RadarrParsers::parse_quality_profiles(json);
    REQUIRE(profiles.size() == 2);
    REQUIRE(profiles[0].id == 1);
    REQUIRE(profiles[0].name == "Any");
    REQUIRE(profiles[1].id == 2);
    REQUIRE(profiles[1].name == "1080p Standard");
    REQUIRE(profiles[1].cutoff_quality_id == 7);
}

TEST_CASE("parse_system_status: extracts version", "[radarr][parsers]") {
    auto json = read_fixture("system_status.json");
    auto status = media_browser::RadarrParsers::parse_system_status(json);
    REQUIRE(status.has_value());
    REQUIRE(status->version == "5.14.0.9383");
}

TEST_CASE("parse_movie_lookup: handles empty array", "[radarr][parsers]") {
    auto hits = media_browser::RadarrParsers::parse_movie_lookup("[]");
    REQUIRE(hits.empty());
}

TEST_CASE("parse_system_status: handles invalid JSON", "[radarr][parsers]") {
    auto status = media_browser::RadarrParsers::parse_system_status("not json");
    REQUIRE(!status.has_value());
}
