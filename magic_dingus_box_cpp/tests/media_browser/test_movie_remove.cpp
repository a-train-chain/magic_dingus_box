// Unit tests for remove_movie_orphan_proof — Detail's Confirm Remove
// sequence (CLAUDE.md "Confirm Remove flow"), extracted from DetailScreen so
// its abort rule is assertable: every READ that decides whether anything is
// left behind must answer before remove_movie(delete_files=true) runs. The
// screen version turned a failed queue read and a failed history read into
// "nothing to clean up" and went on to delete — orphaning seeding torrents
// forever under a "removed" result.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include "media_browser/movie_remove.h"
#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/radarr/radarr_mock.h"

namespace mb = media_browser;

namespace {

// The seeded RadarrMockClient (one library movie, radarr_id 1, with one
// queue row id 1), with each step's outcome injectable and every call
// recorded in order.
class ScriptedRadarr : public mb::RadarrMockClient {
public:
    bool fail_queue_read = false;
    bool fail_hash_read = false;
    bool fail_cancel = false;
    bool fail_remove = false;
    std::vector<std::string> hashes{"abc123"};
    std::vector<std::string> calls;

    std::optional<std::vector<mb::QueueItem>> get_queue_checked() override {
        calls.push_back("queue");
        if (fail_queue_read) {
            set_error("curl: Couldn't connect to server");
            return std::nullopt;
        }
        return RadarrMockClient::get_queue_checked();
    }
    bool cancel_queue_item(int id) override {
        calls.push_back("cancel:" + std::to_string(id));
        if (fail_cancel) {
            set_error("HTTP 500: boom");
            return false;
        }
        return RadarrMockClient::cancel_queue_item(id);
    }
    std::optional<std::vector<std::string>>
    get_movie_download_hashes_checked(int /*movie_id*/) override {
        calls.push_back("hashes");
        if (fail_hash_read) {
            set_error("curl: Timeout was reached");
            return std::nullopt;
        }
        return hashes;
    }
    bool remove_movie(int id, bool delete_files) override {
        calls.push_back(std::string("remove:") + std::to_string(id) +
                        (delete_files ? ":files" : ""));
        if (fail_remove) {
            set_error("HTTP 404: not found");
            return false;
        }
        return RadarrMockClient::remove_movie(id, delete_files);
    }

    bool removed_called() const {
        for (const auto& c : calls)
            if (c.rfind("remove:", 0) == 0) return true;
        return false;
    }
};

class RecordingQbit : public mb::QbittorrentClient {
public:
    RecordingQbit() : mb::QbittorrentClient(Config{}) {}
    std::vector<std::string> deleted;
    bool fail_delete = false;
    bool delete_torrent(const std::string& hash, bool delete_files) override {
        if (delete_files) deleted.push_back(hash);
        return !fail_delete;
    }
};

}  // namespace

TEST_CASE("movie remove: happy path cancels, purges, then deletes",
          "[movie_remove]") {
    ScriptedRadarr radarr;
    RecordingQbit qbit;
    const auto out = mb::remove_movie_orphan_proof(radarr, &qbit, 1);
    CHECK(out.removed);
    CHECK(out.message.empty());
    CHECK(out.cancelled == 1);
    CHECK(out.purged == 1);
    CHECK(qbit.deleted == std::vector<std::string>{"abc123"});
    // Order is the contract: reads and cleanup strictly before the delete.
    CHECK(radarr.calls == std::vector<std::string>{
                              "queue", "cancel:1", "hashes", "remove:1:files"});
    CHECK_FALSE(radarr.get_movie(1).has_value());
}

TEST_CASE("movie remove: a failed queue read aborts before anything destructive",
          "[movie_remove]") {
    ScriptedRadarr radarr;
    radarr.fail_queue_read = true;
    RecordingQbit qbit;
    const auto out = mb::remove_movie_orphan_proof(radarr, &qbit, 1);
    CHECK_FALSE(out.removed);
    CHECK_FALSE(radarr.removed_called());
    CHECK(qbit.deleted.empty());
    CHECK(out.message.find("NOT removed") != std::string::npos);
    CHECK(radarr.get_movie(1).has_value());  // still in the library
}

TEST_CASE("movie remove: a failed history read aborts before the delete",
          "[movie_remove]") {
    ScriptedRadarr radarr;
    radarr.fail_hash_read = true;
    RecordingQbit qbit;
    const auto out = mb::remove_movie_orphan_proof(radarr, &qbit, 1);
    CHECK_FALSE(out.removed);
    CHECK_FALSE(radarr.removed_called());
    CHECK(qbit.deleted.empty());
    CHECK(out.message.find("NOT removed") != std::string::npos);
    // Honesty: the queue cancel (removeFromClient=true) already took that
    // download's data, and the message must not imply nothing happened.
    CHECK(out.cancelled == 1);
    CHECK(out.message.find("Cancelled 1") != std::string::npos);
}

TEST_CASE("movie remove: a failed cancel aborts and names the cause",
          "[movie_remove]") {
    ScriptedRadarr radarr;
    radarr.fail_cancel = true;
    RecordingQbit qbit;
    const auto out = mb::remove_movie_orphan_proof(radarr, &qbit, 1);
    CHECK_FALSE(out.removed);
    CHECK_FALSE(radarr.removed_called());
    CHECK(out.message.find("NOT removed") != std::string::npos);
    CHECK(out.message.find("HTTP 500") != std::string::npos);
}

TEST_CASE("movie remove: no qBittorrent client skips the history walk by design",
          "[movie_remove]") {
    // A box with no qBit client cannot purge anything; the history read
    // would decide nothing, so it is neither made nor allowed to block.
    ScriptedRadarr radarr;
    radarr.fail_hash_read = true;
    const auto out = mb::remove_movie_orphan_proof(radarr, nullptr, 1);
    CHECK(out.removed);
    for (const auto& c : radarr.calls) CHECK(c != "hashes");
}

TEST_CASE("movie remove: a torrent qBit cannot delete does not block the remove",
          "[movie_remove]") {
    // qBit's delete is a no-op for an absent hash and a best-effort purge
    // otherwise — the TV flow's warn-and-continue rule.
    ScriptedRadarr radarr;
    RecordingQbit qbit;
    qbit.fail_delete = true;
    const auto out = mb::remove_movie_orphan_proof(radarr, &qbit, 1);
    CHECK(out.removed);
    CHECK(out.purged == 0);
}

TEST_CASE("movie remove: a refused delete reports failure with the reason",
          "[movie_remove]") {
    ScriptedRadarr radarr;
    radarr.fail_remove = true;
    RecordingQbit qbit;
    const auto out = mb::remove_movie_orphan_proof(radarr, &qbit, 1);
    CHECK_FALSE(out.removed);
    CHECK(out.message.find("HTTP 404") != std::string::npos);
}

TEST_CASE("movie remove: only this movie's queue rows are cancelled",
          "[movie_remove]") {
    ScriptedRadarr radarr;
    RecordingQbit qbit;
    // The mock's queue row belongs to movie 1; removing movie 2 (absent
    // from the queue) must cancel nothing.
    const auto out = mb::remove_movie_orphan_proof(radarr, &qbit, 2);
    CHECK(out.cancelled == 0);
    for (const auto& c : radarr.calls) CHECK(c.rfind("cancel:", 0) != 0);
}
