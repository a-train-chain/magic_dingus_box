#include <catch2/catch_test_macros.hpp>
#include "media_browser/qbittorrent/qbittorrent_client.h"
#include <filesystem>
#include <fstream>
#include <random>

namespace {
struct OwnedQbit : media_browser::QbittorrentClient {
    OwnedQbit() : QbittorrentClient(Config{}) {}
    std::string torrents = R"([{"hash":"aaaa","state":"downloading"},{"hash":"bbbb","state":"stoppedDL"},{"hash":"cccc","state":"pausedUP"}])";
    bool alt = false;
    std::vector<std::string> posts;
    std::vector<bool> syncs;
    int fail_sync_at = 0;
    bool sync_owned_record(const std::string& path, bool parent) override {
        syncs.push_back(parent);
        if (static_cast<int>(syncs.size()) == fail_sync_at) return false;
        return QbittorrentClient::sync_owned_record(path, parent);
    }
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
    std::filesystem::path directory;
    std::string path;
    Marker() {
        do {
            directory = std::filesystem::temp_directory_path() /
                ("mdb-quiet-" + std::to_string(std::random_device{}()));
        } while (!std::filesystem::create_directory(directory));
        path = (directory / "ownership").string();
    }
    ~Marker() { std::error_code ec; std::filesystem::remove_all(directory, ec); }
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

TEST_CASE("qbit intent requires file and directory durability before any owned mutation", "[qbit][quiet_ownership][durability]") {
    for (bool full : {false, true}) {
        for (int failed_sync : {1, 2}) {
            Marker marker; OwnedQbit q; q.fail_sync_at = failed_sync;
            CHECK_FALSE(q.pause_owned(full, marker.path));
            CHECK(q.posts.empty()); CHECK_FALSE(q.alt);
            CHECK(static_cast<int>(q.syncs.size()) == failed_sync);
        }
        Marker marker; OwnedQbit q;
        REQUIRE(q.pause_owned(full, marker.path));
        CHECK(q.syncs == std::vector<bool>{false, true});
        REQUIRE(q.posts.size() == 1);
    }
}

TEST_CASE("qbit recovery fails closed on an unsynced record and retries exact ownership", "[qbit][quiet_ownership][durability]") {
    for (int failure : {1, 2, 3, 4, 5}) {
        DYNAMIC_SECTION("sync failure " << failure) {
            Marker marker; OwnedQbit q; REQUIRE(q.pause_owned(true, marker.path));
            q.posts.clear(); q.syncs.clear(); q.fail_sync_at = failure;
            CHECK_FALSE(q.resume_owned(marker.path));
            if (failure <= 2) CHECK(q.posts.empty());
            if (failure <= 4) CHECK(std::filesystem::exists(marker.path));
            const auto posts = q.posts.size();
            q.fail_sync_at = 0;
            REQUIRE(q.resume_owned(marker.path));
            CHECK_FALSE(std::filesystem::exists(marker.path));
            // Cleared progress is persisted before deletion; a failed parent
            // sync after publication/deletion must not replay that progress.
            if (failure >= 4) CHECK(q.posts.size() == posts);
            for (const auto& post : q.posts) CHECK(post == "/api/v2/torrents/start hashes=aaaa");
        }
    }
}

TEST_CASE("a failed cap checkpoint defers hashes and retry preserves completed cap progress", "[qbit][quiet_ownership][durability]") {
    Marker marker; OwnedQbit q;
    REQUIRE(q.pause_owned(false, marker.path)); REQUIRE(q.pause_owned(true, marker.path));
    q.posts.clear(); q.syncs.clear(); q.fail_sync_at = 4; // cap progress renamed, parent fence failed
    CHECK_FALSE(q.resume_owned(marker.path)); CHECK_FALSE(q.alt);
    REQUIRE(q.posts == std::vector<std::string>{"/api/v2/transfer/toggleSpeedLimitsMode "});
    // The new record records that cap restoration already happened. Finish
    // its fence on retry, without undoing a cap the operator enabled since.
    q.alt = true; q.fail_sync_at = 0;
    REQUIRE(q.resume_owned(marker.path)); CHECK(q.alt);
    REQUIRE(q.posts.size() == 2);
    CHECK(q.posts.back() == "/api/v2/torrents/start hashes=aaaa");
}
