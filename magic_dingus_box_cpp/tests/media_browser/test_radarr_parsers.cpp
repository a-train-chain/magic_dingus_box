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
    // Radarr hands us "original"-size TMDB URLs; the parser must downsize
    // them to w500 so library posters (a) don't blow the artwork cache's
    // byte budget (a 2000x3000 poster is ~24MB of GPU RAM) and (b) share
    // the cache key with the w500 URLs Browse/Detail already use. The
    // fixture's remoteUrls are /t/p/original/... — verify they came out
    // rewritten to /t/p/w500/ and no "original" segment remains.
    REQUIRE(hits[0].poster_url.find("/t/p/w500/") != std::string::npos);
    REQUIRE(hits[0].poster_url.find("/original/") == std::string::npos);
    REQUIRE(hits[1].tmdb_id == 604);
    REQUIRE(hits[1].year == 2003);
}

TEST_CASE("normalize_tmdb_poster_url: only rewrites the size segment",
          "[radarr][parsers]") {
    // Exercised via parse_movie_lookup (pick_image is file-local). Use a
    // tiny inline JSON to assert edge behavior directly.
    auto one = [](const std::string& remote) {
        std::string json =
            "[{\"tmdbId\":1,\"images\":[{\"coverType\":\"poster\","
            "\"remoteUrl\":\"" + remote + "\"}]}]";
        auto h = media_browser::RadarrParsers::parse_movie_lookup(json);
        return h.empty() ? std::string("<none>") : h[0].poster_url;
    };
    // original -> w500
    REQUIRE(one("https://image.tmdb.org/t/p/original/abc.jpg")
            == "https://image.tmdb.org/t/p/w500/abc.jpg");
    // an explicit w-size is still normalized to w500
    REQUIRE(one("https://image.tmdb.org/t/p/w780/abc.jpg")
            == "https://image.tmdb.org/t/p/w500/abc.jpg");
    // already w500 stays w500 (idempotent)
    REQUIRE(one("https://image.tmdb.org/t/p/w500/abc.jpg")
            == "https://image.tmdb.org/t/p/w500/abc.jpg");
    // non-TMDB URL passes through untouched
    REQUIRE(one("https://example.com/posters/abc.jpg")
            == "https://example.com/posters/abc.jpg");
    // empty stays empty
    REQUIRE(one("") == "");
}

TEST_CASE("parse_movie_list: extracts movieFile.path as file_container_path",
          "[radarr][parsers]") {
    auto json = read_fixture("movie_list_with_file.json");
    auto movies = media_browser::RadarrParsers::parse_movie_list(json);
    REQUIRE(movies.size() == 1);
    REQUIRE(movies[0].has_file == true);
    REQUIRE(movies[0].file_path ==
            "Sintel (2010) [720p] [BluRay] [YTS.MX].mp4");
    REQUIRE(movies[0].file_container_path ==
            "/library/Sintel (2010)/Sintel (2010) [720p] [BluRay] [YTS.MX].mp4");
    // Release status + measured file duration feed the fake-download
    // warnings (pre-release banner, runtime-mismatch badge).
    REQUIRE(movies[0].status == "released");
    REQUIRE(movies[0].file_runtime_minutes == 14);  // "0:14:48" floored
}

TEST_CASE("parse_movie: single-movie GET carries status + file fields",
          "[radarr][parsers]") {
    const std::string json = R"({
        "id": 44, "tmdbId": 1368337, "title": "The Odyssey", "year": 2026,
        "runtime": 173, "status": "inCinemas", "monitored": true,
        "hasFile": true,
        "movieFile": {
            "path": "/library/The Odyssey (2026)/fake.mp4",
            "relativePath": "fake.mp4", "size": 900000000,
            "quality": {"quality": {"name": "WEBRip-1080p"}},
            "mediaInfo": {"runTime": "0:47:02"}
        }
    })";
    auto m = media_browser::RadarrParsers::parse_movie(json);
    REQUIRE(m.has_value());
    REQUIRE(m->status == "inCinemas");
    REQUIRE(m->runtime_minutes == 173);
    REQUIRE(m->file_runtime_minutes == 47);
    REQUIRE(m->file_container_path == "/library/The Odyssey (2026)/fake.mp4");
}

TEST_CASE("likely_prerelease_fakes_only: flags pre-home-release statuses",
          "[radarr][fake-detection]") {
    media_browser::Movie m;
    for (const char* s : {"tba", "announced", "inCinemas"}) {
        m.status = s;
        REQUIRE(media_browser::likely_prerelease_fakes_only(m));
    }
    m.status = "released";
    REQUIRE(!media_browser::likely_prerelease_fakes_only(m));
    m.status = "";  // unknown — don't cry wolf
    REQUIRE(!media_browser::likely_prerelease_fakes_only(m));
}

TEST_CASE("file_runtime_suspicious: flags >25% duration deviation",
          "[radarr][fake-detection]") {
    media_browser::Movie m;
    m.has_file = true;
    m.runtime_minutes = 173;

    m.file_runtime_minutes = 47;   // renamed junk / partial
    REQUIRE(media_browser::file_runtime_suspicious(m));
    m.file_runtime_minutes = 165;  // credits trimmed — fine
    REQUIRE(!media_browser::file_runtime_suspicious(m));
    m.file_runtime_minutes = 240;  // way too long is also wrong
    REQUIRE(media_browser::file_runtime_suspicious(m));

    // Missing data must never warn.
    m.file_runtime_minutes = 0;
    REQUIRE(!media_browser::file_runtime_suspicious(m));
    m.file_runtime_minutes = 47;
    m.runtime_minutes = 0;
    REQUIRE(!media_browser::file_runtime_suspicious(m));
    m.runtime_minutes = 173;
    m.has_file = false;
    REQUIRE(!media_browser::file_runtime_suspicious(m));
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
