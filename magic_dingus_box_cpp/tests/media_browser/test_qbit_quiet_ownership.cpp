#include <catch2/catch_test_macros.hpp>
#include "media_browser/qbittorrent/qbittorrent_client.h"
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace {
struct OwnedQbit : media_browser::QbittorrentClient {
    OwnedQbit() : QbittorrentClient(Config{}) {}
    std::string torrents = R"([{"hash":"aaaa","state":"downloading"},{"hash":"bbbb","state":"stoppedDL"},{"hash":"cccc","state":"pausedUP"}])";
    bool alt = false;
    std::vector<std::string> posts;
    std::string http_get(const std::string& path) override {
        if (path == "/api/v2/torrents/info") return torrents;
        if (path == "/api/v2/transfer/speedLimitsMode") return alt ? "1" : "0";
        set_error("unexpected GET"); return {};
    }
    std::string http_post(const std::string& path, const std::string& body) override {
        posts.push_back(path + " " + body);
        if (path == "/api/v2/transfer/toggleSpeedLimitsMode") alt = !alt;
        return {};
    }
};
struct Marker {
    std::string path = "/private/tmp/mdb-quiet-test-" + std::to_string(getpid());
    Marker() { std::filesystem::remove(path); }
    ~Marker() { std::filesystem::remove(path); std::filesystem::remove(path + ".tmp"); }
};
}
TEST_CASE("quiet full pause restores only torrents that were running", "[qbit][quiet_ownership]") {
    Marker marker; OwnedQbit q;
    CHECK(q.pause_owned(true, marker.path));
    REQUIRE(q.posts.size() == 1); CHECK(q.posts[0] == "/api/v2/torrents/stop hashes=aaaa");
    CHECK(q.resume_owned(marker.path));
    REQUIRE(q.posts.size() == 2); CHECK(q.posts[1] == "/api/v2/torrents/start hashes=aaaa");
    CHECK_FALSE(std::filesystem::exists(marker.path));
}
TEST_CASE("quiet mode never clears an operator's existing alternative speed cap", "[qbit][quiet_ownership]") {
    Marker marker; OwnedQbit q; q.alt = true;
    CHECK_FALSE(q.pause_owned(false, marker.path)); CHECK(q.resume_owned(marker.path));
    CHECK(q.alt); CHECK(q.posts.empty());
}
TEST_CASE("owned cap and torrent records accumulate and survive client restart", "[qbit][quiet_ownership]") {
    Marker marker; OwnedQbit q;
    CHECK(q.pause_owned(false, marker.path)); CHECK(q.alt);
    CHECK(q.pause_owned(true, marker.path));
    OwnedQbit restarted; restarted.alt = true;
    CHECK(restarted.resume_owned(marker.path)); CHECK_FALSE(restarted.alt);
    CHECK(restarted.posts == std::vector<std::string>{"/api/v2/transfer/toggleSpeedLimitsMode ", "/api/v2/torrents/start hashes=aaaa"});
}
TEST_CASE("legacy malformed or failed ownership reads cannot resume all torrents", "[qbit][quiet_ownership]") {
    Marker marker; OwnedQbit q;
    for (const auto* contents : {"1\n", "not-json", "{}"}) {
        std::ofstream(marker.path) << contents;
        CHECK_FALSE(q.resume_owned(marker.path)); CHECK(q.posts.empty());
    }
    std::filesystem::remove(marker.path);
    q.torrents = "not-json";
    CHECK_FALSE(q.pause_owned(true, marker.path)); CHECK(q.posts.empty());
}

TEST_CASE("a new pause migrates a legacy marker by observing current activity", "[qbit][quiet_ownership]") {
    Marker marker; OwnedQbit q;
    std::ofstream(marker.path) << "1\n";
    CHECK_FALSE(q.resume_owned(marker.path)); CHECK(q.posts.empty());
    CHECK(q.pause_owned(true, marker.path));
    CHECK(q.resume_owned(marker.path));
    CHECK(q.posts == std::vector<std::string>{"/api/v2/torrents/stop hashes=aaaa", "/api/v2/torrents/start hashes=aaaa"});
}
