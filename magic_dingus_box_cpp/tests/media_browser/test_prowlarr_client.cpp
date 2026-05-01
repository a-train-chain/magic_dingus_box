#include <catch2/catch_test_macros.hpp>
#include "media_browser/prowlarr/prowlarr_client.h"

namespace mb = media_browser;

namespace {

// Two-indexer fixture: one healthy, one with zero results.
constexpr const char* kSearchResponse = R"JSON([
  {"title":"Inception 2010 1080p WEB-DL x264-GROUPA","indexer":"1337x",
   "seeders":200,"leechers":12,"size":2147483648,"protocol":"torrent",
   "guid":"https://1337x.to/torrent/abc","downloadUrl":"magnet:?xt=urn:btih:abc"},
  {"title":"Inception 2010 720p BluRay x264-YIFY","indexer":"YTS",
   "seeders":180,"leechers":8,"size":943718400,"protocol":"torrent",
   "guid":"https://yts.mx/torrent/def","downloadUrl":"magnet:?xt=urn:btih:def"},
  {"title":"Inception 2010 1080p WEB x265-RARE","indexer":"1337x",
   "seeders":3,"leechers":1,"size":1610612736,"protocol":"torrent",
   "guid":"https://1337x.to/torrent/ghi","downloadUrl":"magnet:?xt=urn:btih:ghi"}
])JSON";

}

TEST_CASE("ProwlarrClient parses per-release records from search response",
          "[prowlarr][parser]") {
    auto records = mb::ProwlarrClient::parse_search_response(kSearchResponse);
    REQUIRE(records.size() == 3);
    REQUIRE(records[0].title == "Inception 2010 1080p WEB-DL x264-GROUPA");
    REQUIRE(records[0].indexer == "1337x");
    REQUIRE(records[0].seeders == 200);
    REQUIRE(records[0].size_bytes == 2147483648LL);
    REQUIRE(records[0].guid == "https://1337x.to/torrent/abc");
}

TEST_CASE("ProwlarrClient aggregates per-indexer stats",
          "[prowlarr][stats]") {
    auto records = mb::ProwlarrClient::parse_search_response(kSearchResponse);
    auto stats = mb::ProwlarrClient::aggregate_indexer_stats(
        records, /*seed_threshold=*/10);
    // Expect two indexers in the map.
    REQUIRE(stats.size() == 2);
    auto find = [&](const std::string& name) {
        for (const auto& s : stats) if (s.name == name) return s;
        FAIL("indexer not found: " << name);
        return mb::ProwlarrClient::IndexerStats{};
    };
    auto x1337 = find("1337x");
    REQUIRE(x1337.result_count == 2);
    REQUIRE(x1337.results_above_seed_threshold == 1);  // only the 200-seeder
    auto yts = find("YTS");
    REQUIRE(yts.result_count == 1);
    REQUIRE(yts.results_above_seed_threshold == 1);
}

TEST_CASE("ProwlarrClient parses indexer list from /api/v1/indexer response",
          "[prowlarr][indexer-list]") {
    constexpr const char* kIndexerResp = R"JSON([
      {"id":1,"name":"LimeTorrents","enable":true,"implementation":"Cardigann"},
      {"id":2,"name":"YTS","enable":true,"implementation":"Cardigann"},
      {"id":3,"name":"EZTV","enable":false,"implementation":"Cardigann"}
    ])JSON";
    auto indexers = mb::ProwlarrClient::parse_indexer_list(kIndexerResp);
    REQUIRE(indexers.size() == 3);
    REQUIRE(indexers[0].id == 1);
    REQUIRE(indexers[0].name == "LimeTorrents");
    REQUIRE(indexers[0].enabled == true);
    REQUIRE(indexers[2].name == "EZTV");
    REQUIRE(indexers[2].enabled == false);
}

TEST_CASE("ProwlarrClient indexer list parser handles empty/malformed responses",
          "[prowlarr][indexer-list]") {
    REQUIRE(mb::ProwlarrClient::parse_indexer_list("").empty());
    REQUIRE(mb::ProwlarrClient::parse_indexer_list("not json").empty());
    REQUIRE(mb::ProwlarrClient::parse_indexer_list("{}").empty());  // not array
    REQUIRE(mb::ProwlarrClient::parse_indexer_list("[]").empty());  // empty array
}
