#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "media_browser/prowlarr/prowlarr_client.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

class PartialAvailabilityClient final : public mb::ProwlarrClient {
public:
    PartialAvailabilityClient()
        : ProwlarrClient([] {
              Config cfg;
              cfg.api_key = "test-key";
              cfg.timeout_secs = 5;
              cfg.search_timeout_secs = 15;
              return cfg;
          }()) {}

    bool all_sources_started_together() const {
        return all_started_.load(std::memory_order_acquire);
    }

protected:
    HttpGetResult http_get_result(const std::string& path,
                                  int timeout_secs) override {
        if (path == "/api/v1/indexer") {
            return {R"JSON([
              {"id":3,"name":"LimeTorrents","enable":true},
              {"id":4,"name":"The Pirate Bay","enable":true},
              {"id":7,"name":"YTS","enable":true},
              {"id":9,"name":"Disabled","enable":false}
            ])JSON", {}};
        }

        int id = 0;
        const std::string marker = "&indexerIds=";
        const auto pos = path.find(marker);
        if (pos != std::string::npos) {
            id = std::stoi(path.substr(pos + marker.size()));
        }
        if (id == 0 || timeout_secs != 15) return {};

        // A small barrier proves the calls are launched concurrently. A
        // sequential implementation leaves the first call waiting until the
        // deadline and never flips all_started_.
        {
            std::unique_lock<std::mutex> lk(start_mtx_);
            ++started_;
            if (started_ == 3) {
                all_started_.store(true, std::memory_order_release);
                start_cv_.notify_all();
            } else {
                start_cv_.wait_for(lk, std::chrono::milliseconds(250), [this] {
                    return all_started_.load(std::memory_order_acquire);
                });
            }
        }

        if (id == 4) return {{}, "simulated timeout"};
        if (id == 3) {
            return {R"JSON([
              {"title":"Spider-Man 2 2004 1080p x264-LIME",
               "indexer":"LimeTorrents","seeders":40}
            ])JSON", {}};
        }
        if (id == 7) {
            return {R"JSON([
              {"title":"Spider-Man 2 2004 720p x264-YTS",
               "indexer":"YTS","seeders":25},
              {"title":"Spider-Man 2 2004 1080p x264-YTS",
               "indexer":"YTS","seeders":30}
            ])JSON", {}};
        }
        return {};
    }

private:
    mutable std::mutex start_mtx_;
    std::condition_variable start_cv_;
    int started_ = 0;
    std::atomic<bool> all_started_{false};
};

class ThrowingAvailabilityClient final : public mb::ProwlarrClient {
public:
    ThrowingAvailabilityClient()
        : ProwlarrClient([] {
              Config cfg;
              cfg.api_key = "test-key";
              cfg.search_timeout_secs = 15;
              return cfg;
          }()) {}

protected:
    HttpGetResult http_get_result(const std::string& path, int) override {
        if (path == "/api/v1/indexer") {
            return {R"JSON([
              {"id":3,"name":"Healthy","enable":true},
              {"id":4,"name":"Throws","enable":true}
            ])JSON", {}};
        }
        if (path.find("&indexerIds=3") != std::string::npos) {
            return {R"JSON([
              {"title":"Spider-Man 2 2004 1080p x264-OK",
               "indexer":"Healthy","seeders":18}
            ])JSON", {}};
        }
        if (path.find("&indexerIds=4") != std::string::npos) {
            throw std::runtime_error("simulated source task failure");
        }
        return {};
    }
};

class SupersededAvailabilityClient final : public mb::ProwlarrClient {
public:
    SupersededAvailabilityClient()
        : ProwlarrClient([] {
              Config cfg;
              cfg.api_key = "test-key";
              cfg.base_url = "http://127.0.0.1:1";
              return cfg;
          }()) {}

    bool wait_until_old_source_is_blocked() {
        std::unique_lock<std::mutex> lk(gate_mtx_);
        return gate_cv_.wait_for(lk, std::chrono::seconds(1), [this] {
            return old_source_blocked_;
        });
    }

    void release_old_source() {
        {
            std::lock_guard<std::mutex> lk(gate_mtx_);
            release_old_ = true;
        }
        gate_cv_.notify_all();
    }

protected:
    HttpGetResult http_get_result(const std::string& path, int) override {
        if (path == "/api/v1/indexer") {
            return {R"JSON([{"id":7,"name":"YTS","enable":true}])JSON", {}};
        }
        if (path.find("Old%20Movie") != std::string::npos) {
            std::unique_lock<std::mutex> lk(gate_mtx_);
            old_source_blocked_ = true;
            gate_cv_.notify_all();
            gate_cv_.wait(lk, [this] { return release_old_; });
            lk.unlock();
            // Exercise the real failing HTTP path after the new generation
            // has already published Ready. The superseded curl error must not
            // leak into the active generation's error channel.
            return ProwlarrClient::http_get_result(path, 1);
        }
        if (path.find("New%20Movie") != std::string::npos) {
            return {R"JSON([
              {"title":"New Movie 2001","indexer":"NewIndexer","seeders":99}
            ])JSON", {}};
        }
        return {};
    }

private:
    std::mutex gate_mtx_;
    std::condition_variable gate_cv_;
    bool old_source_blocked_ = false;
    bool release_old_ = false;
};

class EmptyAvailabilityClient final : public mb::ProwlarrClient {
public:
    enum class StatusMode { AllFailed, OneHealthy, Unavailable, Malformed };

    explicit EmptyAvailabilityClient(StatusMode status_mode,
                                     bool enable_sources = true,
                                     bool with_timed_out_source = false)
        : ProwlarrClient([] {
              Config cfg;
              cfg.api_key = "test-key";
              return cfg;
          }()),
          status_mode_(status_mode),
          enable_sources_(enable_sources),
          with_timed_out_source_(with_timed_out_source) {}

protected:
    HttpGetResult http_get_result(const std::string& path, int) override {
        if (path == "/api/v1/indexer") {
            const char* enabled = enable_sources_ ? "true" : "false";
            return {std::string("[\n")
                      + "{\"id\":3,\"name\":\"Empty A\",\"enable\":" + enabled + "},"
                      + "{\"id\":7,\"name\":\"Empty B\",\"enable\":" + enabled + "}"
                      + (with_timed_out_source_
                             ? ",{\"id\":4,\"name\":\"Slow\",\"enable\":true}"
                             : "")
                      + "]",
                    {}};
        }
        if (path == "/api/v1/indexerstatus") {
            if (status_mode_ == StatusMode::AllFailed) {
                return {R"JSON([
                  {"indexerId":3,"disabledTill":"2099-01-01T00:00:00Z"},
                  {"indexerId":7,"disabledTill":"2099-01-01T00:00:00Z"}
                ])JSON", {}};
            }
            if (status_mode_ == StatusMode::Unavailable) {
                return {{}, "simulated status timeout"};
            }
            if (status_mode_ == StatusMode::Malformed) return {"not json", {}};
            return {R"JSON([
              {"indexerId":3,"disabledTill":"2099-01-01T00:00:00Z"}
            ])JSON", {}};
        }
        if (path.find("&indexerIds=4") != std::string::npos) {
            return {{}, "simulated timeout"};
        }
        if (path.find("&indexerIds=") != std::string::npos) return {"[]", {}};
        return {};
    }

private:
    StatusMode status_mode_;
    bool enable_sources_;
    bool with_timed_out_source_;
};

bool wait_for_terminal_state(mb::ProwlarrClient& client) {
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto state = client.state();
        if (state == mb::ProwlarrClient::State::Ready ||
            state == mb::ProwlarrClient::State::Failed) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

}

TEST_CASE("ProwlarrClient returns healthy indexer results when one source times out",
          "[prowlarr][search][partial]") {
    PartialAvailabilityClient client;

    client.search_async("Spider-Man 2", 2004);

    REQUIRE(wait_for_terminal_state(client));
    REQUIRE(client.state() == mb::ProwlarrClient::State::Ready);
    REQUIRE(client.all_sources_started_together());
    const auto result = client.peek_result();
    REQUIRE(result.has_value());
    CHECK(result->total_releases == 3);
    CHECK(result->best_seeders == 40);
    CHECK(result->total_seeders == 95);
    CHECK(client.peek_error().empty());
}

TEST_CASE("ProwlarrClient contains a source-task exception and keeps healthy results",
          "[prowlarr][search][partial][exception]") {
    ThrowingAvailabilityClient client;

    client.search_async("Spider-Man 2", 2004);

    REQUIRE(wait_for_terminal_state(client));
    REQUIRE(client.state() == mb::ProwlarrClient::State::Ready);
    const auto result = client.peek_result();
    REQUIRE(result.has_value());
    CHECK(result->total_releases == 1);
    CHECK(result->best_seeders == 18);
    CHECK(client.peek_error().empty());
}

TEST_CASE("ProwlarrClient never lets a superseded search replace the active result",
          "[prowlarr][search][generation]") {
    SupersededAvailabilityClient client;

    client.search_async("Old Movie", 2000);
    REQUIRE(client.wait_until_old_source_is_blocked());

    client.search_async("New Movie", 2001);
    REQUIRE(wait_for_terminal_state(client));
    REQUIRE(client.state() == mb::ProwlarrClient::State::Ready);
    REQUIRE(client.peek_result().has_value());
    REQUIRE(client.peek_result()->best_seeders == 99);

    client.release_old_source();
    client.cancel();  // deterministically joins both generations

    REQUIRE(client.state() == mb::ProwlarrClient::State::Ready);
    REQUIRE(client.peek_result().has_value());
    CHECK(client.peek_result()->best_seeders == 99);
    CHECK(client.peek_error().empty());
    const auto stats = client.get_last_indexer_stats();
    REQUIRE(stats.size() == 1);
    CHECK(stats[0].name == "NewIndexer");
}

TEST_CASE("ProwlarrClient reports failure when every empty source has failed status",
          "[prowlarr][search][empty][status]") {
    EmptyAvailabilityClient client(EmptyAvailabilityClient::StatusMode::AllFailed);

    client.search_async("Obscure Movie", 2004);

    REQUIRE(wait_for_terminal_state(client));
    CHECK(client.state() == mb::ProwlarrClient::State::Failed);
    CHECK_FALSE(client.peek_result().has_value());
    CHECK(client.peek_error() == "All availability sources failed");
}

TEST_CASE("ProwlarrClient does not claim no sources when a source timed out and the rest are empty",
          "[prowlarr][search][empty][partial]") {
    // YTS-style healthy source answers [] while the slow source (the one
    // most likely to carry an obscure title) never answers. That is
    // inconclusive — "No sources found" would be a false negative.
    EmptyAvailabilityClient client(EmptyAvailabilityClient::StatusMode::OneHealthy,
                                   /*enable_sources=*/true,
                                   /*with_timed_out_source=*/true);

    client.search_async("Obscure Movie", 2004);

    REQUIRE(wait_for_terminal_state(client));
    CHECK(client.state() == mb::ProwlarrClient::State::Failed);
}

TEST_CASE("ProwlarrClient keeps no-sources result when one empty source is healthy",
          "[prowlarr][search][empty][status]") {
    EmptyAvailabilityClient client(EmptyAvailabilityClient::StatusMode::OneHealthy);

    client.search_async("Obscure Movie", 2004);

    REQUIRE(wait_for_terminal_state(client));
    REQUIRE(client.state() == mb::ProwlarrClient::State::Ready);
    REQUIRE(client.peek_result().has_value());
    CHECK(client.peek_result()->total_releases == 0);
}

TEST_CASE("ProwlarrClient reports configuration failure with no enabled sources",
          "[prowlarr][search][empty][configuration]") {
    EmptyAvailabilityClient client(EmptyAvailabilityClient::StatusMode::OneHealthy,
                                   /*enable_sources=*/false);

    client.search_async("Obscure Movie", 2004);

    REQUIRE(wait_for_terminal_state(client));
    CHECK(client.state() == mb::ProwlarrClient::State::Failed);
    CHECK(client.peek_error() == "No availability sources are enabled");
}

TEST_CASE("ProwlarrClient does not claim no sources when status verification fails",
          "[prowlarr][search][empty][status]") {
    const auto mode = GENERATE(EmptyAvailabilityClient::StatusMode::Unavailable,
                               EmptyAvailabilityClient::StatusMode::Malformed);
    EmptyAvailabilityClient client(mode);

    client.search_async("Obscure Movie", 2004);

    REQUIRE(wait_for_terminal_state(client));
    CHECK(client.state() == mb::ProwlarrClient::State::Failed);
    CHECK_FALSE(client.peek_result().has_value());
    CHECK(client.peek_error() == "Could not verify empty availability results");
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
