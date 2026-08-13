#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "media_browser/sonarr/sonarr_parsers.h"

namespace fs = std::filesystem;
namespace mb = media_browser;

static std::string read_fixture(const std::string& name) {
    fs::path p = fs::path(__FILE__).parent_path() / "fixtures" / "sonarr" / name;
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

TEST_CASE("parse_series_lookup extracts the not-yet-added series", "[sonarr][parsers]") {
    auto hits = mb::SonarrParsers::parse_series_lookup(read_fixture("series_lookup.json"));
    REQUIRE(hits.size() == 1);
    const auto& h = hits[0];
    CHECK(h.title == "Breaking Bad");
    CHECK(h.tvdb_id == 81189);
    CHECK(h.tmdb_id == 1396);
    CHECK(h.imdb_id == "tt0903747");
    CHECK(h.year == 2008);
    // Sonarr's series.runtime is PER EPISODE — it is what the whole-series
    // disk estimate multiplies by, so it must survive the parse.
    CHECK(h.runtime_minutes == 47);
    CHECK(h.status == "ended");
    // Lookup results carry every season monitored:true — that is Sonarr's
    // pre-add default, NOT what the add will persist.
    REQUIRE(h.seasons.size() == 6);
    CHECK(h.seasons[0].season_number == 0);
    for (const auto& s : h.seasons) CHECK(s.monitored);
}

TEST_CASE("parse_series_lookup normalizes TMDB artwork to w500 and passes "
          "TVDB URLs through", "[sonarr][parsers]") {
    auto hits = mb::SonarrParsers::parse_series_lookup(read_fixture("series_lookup.json"));
    REQUIRE(hits.size() == 1);
    // TMDB poster: rewritten so it shares the artwork-cache key TmdbClient
    // already emits, and so a 2000x3000 original never reaches the 256MB cache.
    CHECK(hits[0].poster_url.find("/t/p/w500/") != std::string::npos);
    CHECK(hits[0].poster_url.find("/original/") == std::string::npos);
    // TVDB fanart: not a TMDB URL, so it passes through untouched.
    CHECK(hits[0].fanart_url ==
          "https://artworks.thetvdb.com/banners/fanart/original/81189-3.jpg");
}

TEST_CASE("parse_series reads per-season monitored flags and statistics",
          "[sonarr][parsers]") {
    auto s = mb::SonarrParsers::parse_series(read_fixture("series_added.json"));
    REQUIRE(s.has_value());
    CHECK(s->sonarr_id == 7);
    CHECK(s->tvdb_id == 81189);
    CHECK(s->tmdb_id == 1396);
    CHECK(s->monitored);
    CHECK(s->path == "/data/library/tv/Breaking Bad");
    CHECK(s->added_at == "2026-08-01T09:00:00Z");
    CHECK(s->episode_file_count == 7);
    CHECK(s->size_on_disk_bytes == 8589934592LL);

    REQUIRE(s->seasons.size() == 6);
    CHECK(s->seasons[0].season_number == 0);
    CHECK_FALSE(s->seasons[0].monitored);          // Specials
    CHECK(s->seasons[0].episode_count == 5);
    CHECK(s->seasons[1].season_number == 1);
    CHECK(s->seasons[1].monitored);                // the ONLY monitored season
    CHECK(s->seasons[1].episode_count == 7);
    CHECK(s->seasons[1].episode_file_count == 7);
    CHECK(s->seasons[1].size_on_disk_bytes == 8589934592LL);
    for (size_t i = 2; i < s->seasons.size(); ++i) {
        CHECK_FALSE(s->seasons[i].monitored);
    }
}

TEST_CASE("parse_series_list parses the library array", "[sonarr][parsers]") {
    auto list = mb::SonarrParsers::parse_series_list(read_fixture("series_list.json"));
    REQUIRE(list.size() == 1);
    CHECK(list[0].sonarr_id == 7);
    CHECK(list[0].title == "Breaking Bad");
    CHECK(list[0].seasons.size() == 2);
}

TEST_CASE("parse_series_list and parse_series reject wrong shapes",
          "[sonarr][parsers]") {
    CHECK(mb::SonarrParsers::parse_series_list("not json {{{").empty());
    CHECK(mb::SonarrParsers::parse_series_list(R"({"error":"x"})").empty());
    CHECK_FALSE(mb::SonarrParsers::parse_series("not json {{{").has_value());
    CHECK_FALSE(mb::SonarrParsers::parse_series("[]").has_value());
}

TEST_CASE("parse_queue keeps one record per EPISODE and never groups",
          "[sonarr][parsers][queue]") {
    auto q = mb::SonarrParsers::parse_queue(read_fixture("queue.json"));
    // A season pack is N episode rows sharing ONE downloadId. The client
    // deliberately does not collapse them — grouping is Phase 2c's UI job,
    // and it needs the raw rows plus the shared id to do it.
    REQUIRE(q.size() == 3);
    CHECK(q[0].id == 101);
    CHECK(q[1].id == 102);
    CHECK(q[2].id == 103);
    CHECK(q[0].download_id == q[1].download_id);
    CHECK(q[1].download_id == q[2].download_id);
    // Raw casing preserved (uppercase hex), matching RadarrParsers::parse_queue
    // — QueueScreen lowercases at comparison time against qBit.
    CHECK(q[0].download_id == "A1B2C3D4E5F60718293A4B5C6D7E8F9012345678");

    CHECK(q[0].series_id == 7);
    CHECK(q[0].episode_id == 5001);
    CHECK(q[0].season_number == 2);
    CHECK(q[0].size_bytes == 12884901888LL);
    CHECK(q[0].sizeleft_bytes == 6442450944LL);
    CHECK(q[0].progress > 0.49);
    CHECK(q[0].progress < 0.51);
    CHECK(q[0].eta_seconds == 4800);            // "01:20:00"
    CHECK(q[0].state == "downloading");
    CHECK(q[0].tracked_download_state == "downloading");
    // Embedded episode (requested via includeEpisode=true) gives 2c the
    // "S02E01 — Seven Thirty-Seven" label without a second round-trip.
    CHECK(q[0].episode.id == 5001);
    CHECK(q[0].episode.episode_number == 1);
    CHECK(q[0].episode.title == "Seven Thirty-Seven");
    CHECK(q[0].episode.air_date == "2009-03-08");
    CHECK(q[2].episode.episode_number == 3);
}

TEST_CASE("parse_queue accepts the staged sizeLeft rename", "[sonarr][parsers][queue]") {
    // 'sizeleft' is marked [Obsolete] upstream with 'SizeLeft' staged but
    // commented out. Parse both so a Sonarr upgrade cannot silently zero
    // every progress bar.
    const std::string json = R"({"records":[
      {"id": 1, "seriesId": 7, "episodeId": 2, "seasonNumber": 1,
       "title": "T", "size": 1000, "sizeLeft": 250, "status": "downloading"}
    ]})";
    auto q = mb::SonarrParsers::parse_queue(json);
    REQUIRE(q.size() == 1);
    CHECK(q[0].sizeleft_bytes == 250);
    CHECK(q[0].progress > 0.74);
    CHECK(q[0].progress < 0.76);
}

TEST_CASE("parse_queue survives an empty/absent records array",
          "[sonarr][parsers][queue]") {
    CHECK(mb::SonarrParsers::parse_queue(R"({"records":[]})").empty());
    CHECK(mb::SonarrParsers::parse_queue(R"({"page":1})").empty());
    CHECK(mb::SonarrParsers::parse_queue("not json {{{").empty());
}

TEST_CASE("parse_queue_total reads totalRecords for the pagination loop",
          "[sonarr][parsers][queue]") {
    // get_queue pages through the queue; without totalRecords it cannot tell
    // "that was the last page" from "the page happened to be full".
    CHECK(mb::SonarrParsers::parse_queue_total(read_fixture("queue.json")) == 3);
    CHECK(mb::SonarrParsers::parse_queue_total(R"({"totalRecords":250,"records":[]})") == 250);
    // Absent/malformed → 0, which the loop treats as "no total available"
    // and falls back to the short-page test.
    CHECK(mb::SonarrParsers::parse_queue_total(R"({"records":[]})") == 0);
    CHECK(mb::SonarrParsers::parse_queue_total("not json {{{") == 0);
}

TEST_CASE("parse_history_download_ids dedupes case-insensitively and lowercases",
          "[sonarr][parsers][history]") {
    auto ids = mb::SonarrParsers::parse_history_download_ids(
        read_fixture("history_series.json"));
    // 4 records: two are the same hash in different case, one distinct, one
    // empty. qBittorrent stores hashes lowercase, so the orphan-proof remove
    // must hand it lowercase and must not ask twice for the same torrent.
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == "a1b2c3d4e5f60718293a4b5c6d7e8f9012345678");
    CHECK(ids[1] == "ffeeddccbbaa99887766554433221100aabbccdd");
}

TEST_CASE("parse_history_download_ids accepts a paged body too",
          "[sonarr][parsers][history]") {
    // /history/series is unpaginated, but /history is paged — tolerate both
    // so a caller switching endpoints does not silently get nothing.
    const std::string paged =
        R"({"records":[{"id":1,"downloadId":"ABC"},{"id":2,"downloadId":"abc"}]})";
    auto ids = mb::SonarrParsers::parse_history_download_ids(paged);
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == "abc");
}

TEST_CASE("parse_quality_profiles and parse_root_folders reuse the Radarr shapes",
          "[sonarr][parsers]") {
    auto profiles = mb::SonarrParsers::parse_quality_profiles(
        read_fixture("quality_profiles.json"));
    REQUIRE(profiles.size() == 1);
    CHECK(profiles[0].id == 1);
    // Resolve BY NAME at every call site — the id is 1 on this box only.
    CHECK(profiles[0].name == "Any");

    auto roots = mb::SonarrParsers::parse_root_folders(read_fixture("root_folders.json"));
    REQUIRE(roots.size() == 1);
    CHECK(roots[0].id == 1);
    CHECK(roots[0].path == "/data/library/tv");
    CHECK(roots[0].free_space_bytes == 187904819200LL);
}

TEST_CASE("parse_quality_definitions: fixture-shaped rows, null preferred tolerated",
          "[sonarr][parsers]") {
    // Shape per GET /api/v3/qualitydefinition (Sonarr 4): sizes are
    // MB/min doubles; preferredSize may be null ("unlimited").
    const std::string body = R"([
        {"quality": {"id": 4, "name": "HDTV-720p"},
         "minSize": 17.1, "maxSize": 60.0, "preferredSize": 40.0},
        {"quality": {"id": 9, "name": "HDTV-1080p"},
         "minSize": 33.3, "maxSize": 100.0, "preferredSize": 70.0},
        {"quality": {"id": 3, "name": "Bluray-1080p"},
         "minSize": 50.4, "maxSize": 100.0, "preferredSize": null}
    ])";
    auto defs = mb::SonarrParsers::parse_quality_definitions(body);
    REQUIRE(defs.size() == 3);
    CHECK(defs[0].quality_id == 4);
    CHECK(defs[0].title == "HDTV-720p");
    CHECK(defs[0].preferred_mb_per_min == 40.0);
    CHECK(defs[0].max_mb_per_min == 60.0);
    CHECK(defs[1].preferred_mb_per_min == 70.0);
    CHECK(defs[2].preferred_mb_per_min == 0.0);  // null → 0, skipped downstream
}

TEST_CASE("parse_quality_definitions: non-array and garbage bodies parse to empty",
          "[sonarr][parsers]") {
    CHECK(mb::SonarrParsers::parse_quality_definitions("").empty());
    CHECK(mb::SonarrParsers::parse_quality_definitions("not json").empty());
    CHECK(mb::SonarrParsers::parse_quality_definitions(R"({"error":"x"})").empty());
    // Rows missing the quality object are skipped, not fatal.
    CHECK(mb::SonarrParsers::parse_quality_definitions(R"([{"minSize": 1.0}])").empty());
}

TEST_CASE("parse_season_history buckets by eventType (input is season-scoped)") {
    // Shape from the live capture (fixtures/sonarr_history_series.json —
    // captured WITH &seasonNumber, so records carry no season field).
    // downloadId deliberately mixed-case — parser lowercases. NOTE the
    // grabbed record's data.downloadUrl carries a REDACTED token in the
    // fixture (live Prowlarr key in the real response — never log these).
    const std::string json = R"([
      {"id": 501, "eventType": "grabbed", "downloadId": "ABCDEF123456",
       "data": {"downloadUrl": "http://localhost:9696/2/download?apikey=REDACTED"}},
      {"id": 502, "eventType": "downloadFolderImported", "downloadId": "ABCDEF123456"}
    ])";
    auto h = mb::SonarrParsers::parse_season_history(json);
    REQUIRE(h.grabbed_history_ids == std::vector<int>{501});
    REQUIRE(h.imported_history_ids == std::vector<int>{502});
    REQUIRE(h.download_hashes == std::vector<std::string>{"abcdef123456"});
}

TEST_CASE("parse_season_history dedupes hashes and tolerates junk") {
    const std::string json = R"([
      {"id": 1, "eventType": "grabbed", "downloadId": "AAAA"},
      {"id": 2, "eventType": "grabbed", "downloadId": "AAAA"},
      {"id": 3, "eventType": "grabbed"},
      "not-an-object"
    ])";
    auto h = mb::SonarrParsers::parse_season_history(json);
    // Record 3 has no downloadId and is STILL bucketed: the id is what
    // POST /history/failed/{id} needs, and requiring a downloadId for the
    // buckets dropped exactly the manually-imported records the worker's
    // imported-ids fallback exists to blocklist. The HASH set still needs
    // one — "" is not a torrent qBit can be asked about.
    REQUIRE(h.grabbed_history_ids == std::vector<int>{1, 2, 3});
    REQUIRE(h.download_hashes == std::vector<std::string>{"aaaa"});
}

TEST_CASE("parse_season_history keeps a downloadId-less imported record") {
    // The manually-imported case named in the season-delete worker's
    // stage-(d) fallback comment: no grab record at all, and the import
    // record carries no downloadId. Dropping it left the fallback with
    // nothing to fall back TO, so the delete proceeded with nothing
    // blocklisted.
    const std::string json = R"([
      {"id": 77, "eventType": "downloadFolderImported"}
    ])";
    auto h = mb::SonarrParsers::parse_season_history(json);
    REQUIRE(h.grabbed_history_ids.empty());
    REQUIRE(h.imported_history_ids == std::vector<int>{77});
    REQUIRE(h.download_hashes.empty());
}

TEST_CASE("parse_season_history accepts the paged {\"records\":[...]} shape") {
    // Its sibling parse_history_download_ids has always accepted both
    // shapes via records_of(). A reshaped body reaching the bare-array-only
    // form degraded to "authoritative: no history" — and the worker then
    // deletes files with no blocklist and no torrent purge.
    const std::string json = R"({"page":1,"totalRecords":1,"records":[
      {"id": 9, "eventType": "grabbed", "downloadId": "BEEF"}
    ]})";
    auto h = mb::SonarrParsers::parse_season_history(json);
    REQUIRE(h.grabbed_history_ids == std::vector<int>{9});
    REQUIRE(h.download_hashes == std::vector<std::string>{"beef"});
}

TEST_CASE("parse_season_history on malformed json yields empty") {
    auto h = mb::SonarrParsers::parse_season_history("{nope");
    REQUIRE(h.grabbed_history_ids.empty());
    REQUIRE(h.imported_history_ids.empty());
    REQUIRE(h.download_hashes.empty());
}

TEST_CASE("parse_episode_files extracts id + seasonNumber") {
    const std::string json = R"([
      {"id": 11, "seasonNumber": 1, "path": "/data/library/tv/x/S01E01.mkv"},
      {"id": 33, "seasonNumber": 3},
      {"noid": true}
    ])";
    auto files = mb::SonarrParsers::parse_episode_files(json);
    REQUIRE(files.size() == 2);
    REQUIRE(files[0].id == 11);
    REQUIRE(files[0].season_number == 1);
    REQUIRE(files[1].id == 33);
    REQUIRE(files[1].season_number == 3);
}
