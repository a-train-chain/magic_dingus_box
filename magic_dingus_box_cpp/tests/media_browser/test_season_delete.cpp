// Unit tests for run_delete_season / compose_season_delete_toast —
// SeriesDetail's "Delete Season N…" sequence (CLAUDE.md "Confirm delete
// Season N flow (per-season orphan-proof cleanup)"), extracted from the
// screen's mutation worker. What these pin:
//   * the call ORDER across Sonarr and qBittorrent (one shared log), with
//     the AutoRedownloadGuard held across every destructive stage;
//   * the abort rule — nothing destructive runs after an unanswered read or
//     a refused mutation in stages (a)-(d);
//   * the toast-disclosure rules — an abort after (c) or (e) says what was
//     already destroyed, a defeated guard restore says the flag is stuck
//     OFF, and a run with no history record says nothing was blocklisted;
//   * the guard is restored on every exit path, including a throw.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/season_delete.h"
#include "media_browser/sonarr/sonarr_mock.h"

namespace mb = media_browser;

namespace {

constexpr int kSeries = 7;
constexpr int kSeason = 3;
const std::string kDash = "\xE2\x80\x94";

std::string join(const std::vector<int>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ",";
        s += std::to_string(v[i]);
    }
    return s;
}

// SonarrMockClient with every call the season delete makes recorded into a
// log shared with the qBit fake (so cross-client ORDER is assertable), each
// outcome injectable, and an optional throw at a named call.
class ScriptedSonarr : public mb::SonarrMockClient {
public:
    explicit ScriptedSonarr(std::vector<std::string>& log) : log_(log) {
        // Season 3 of series 7: two real episodes plus an id-0 record that
        // must never be PUT; one season-2 episode that must not be touched.
        mb::EpisodeInfo e;
        e.season_number = 3; e.id = 31; episodes.push_back(e);
        e.season_number = 3; e.id = 32; episodes.push_back(e);
        e.season_number = 3; e.id = 0;  episodes.push_back(e);
        e.season_number = 2; e.id = 21; episodes.push_back(e);

        // Queue: a two-row season pack (ONE download -> ONE cancel), plus
        // rows for another season and another series that must survive.
        mb::SonarrQueueItem q;
        q.series_id = kSeries; q.season_number = 3; q.download_id = "AAA";
        q.id = 100; queue.push_back(q);
        q.id = 101; queue.push_back(q);
        q.season_number = 2; q.download_id = "BBB"; q.id = 102; queue.push_back(q);
        q.series_id = 8; q.season_number = 3; q.download_id = "CCC";
        q.id = 103; queue.push_back(q);

        history.grabbed_history_ids = {501, 502};
        history.imported_history_ids = {601};
        history.download_hashes = {"aaa", "bbb"};

        mb::EpisodeFileInfo f;
        f.season_number = 3; f.id = 901; files.push_back(f);
        f.season_number = 3; f.id = 902; files.push_back(f);
        f.season_number = 2; f.id = 903; files.push_back(f);
    }

    // Fixtures.
    std::vector<mb::EpisodeInfo> episodes;
    std::vector<mb::SonarrQueueItem> queue;
    mb::SeasonHistory history;
    std::vector<mb::EpisodeFileInfo> files;
    bool redownload_on = true;  // the owner's autoRedownloadFailed

    // Failure injection.
    bool fail_season_monitor = false;
    bool fail_episodes_read = false;
    bool fail_episodes_monitor = false;
    bool fail_history = false;
    bool fail_config_read = false;
    bool fail_disable = false;   // the guard's PUT(false)
    bool fail_restore = false;   // every PUT(true)
    bool fail_queue_read = false;
    std::set<int> fail_cancel_ids;
    std::set<int> fail_mark_ids;
    bool fail_files_read = false;
    bool fail_delete = false;
    std::string throw_at;  // log-entry prefix at which to throw

    int restore_attempts = 0;

    bool set_season_monitored(int sid, int season, bool monitored) override {
        rec("season_mon:" + std::to_string(sid) + ":" + std::to_string(season) +
            (monitored ? ":on" : ":off"));
        return !fail_season_monitor;
    }
    std::optional<std::vector<mb::EpisodeInfo>> get_episodes_checked(
            int sid) override {
        rec("episodes:" + std::to_string(sid));
        if (fail_episodes_read) return std::nullopt;
        return episodes;
    }
    bool set_episodes_monitored(const std::vector<int>& ids,
                                bool monitored) override {
        rec("eps_mon:" + join(ids) + (monitored ? ":on" : ":off"));
        return !fail_episodes_monitor;
    }
    std::optional<mb::SeasonHistory> get_season_history_checked(
            int sid, int season) override {
        rec("history:" + std::to_string(sid) + ":" + std::to_string(season));
        if (fail_history) return std::nullopt;
        return history;
    }
    std::optional<mb::DownloadClientConfig> get_download_client_config()
            override {
        rec("dlcfg");
        if (fail_config_read) return std::nullopt;
        mb::DownloadClientConfig c;
        c.id = 1;
        c.auto_redownload_failed = redownload_on;
        c.raw = R"({"id":1,"autoRedownloadFailed":true})";
        return c;
    }
    bool set_auto_redownload_failed(const mb::DownloadClientConfig&,
                                    bool enabled) override {
        rec(enabled ? "redl:on" : "redl:off");
        if (enabled) {
            ++restore_attempts;
            if (fail_restore) return false;
        } else if (fail_disable) {
            return false;
        }
        redownload_on = enabled;
        return true;
    }
    std::optional<std::vector<mb::SonarrQueueItem>> get_queue_checked()
            override {
        rec("queue");
        if (fail_queue_read) return std::nullopt;
        return queue;
    }
    bool cancel_queue_item(int qid, bool blocklist) override {
        rec("cancel:" + std::to_string(qid) + (blocklist ? ":bl" : ""));
        return fail_cancel_ids.count(qid) == 0;
    }
    bool mark_history_failed(int hid) override {
        rec("failed:" + std::to_string(hid));
        return fail_mark_ids.count(hid) == 0;
    }
    std::optional<std::vector<mb::EpisodeFileInfo>> get_episode_files_checked(
            int sid) override {
        rec("files:" + std::to_string(sid));
        if (fail_files_read) return std::nullopt;
        return files;
    }
    bool delete_episode_files(const std::vector<int>& ids) override {
        rec("delete_files:" + join(ids));
        return !fail_delete;
    }

private:
    void rec(const std::string& entry) {
        log_.push_back(entry);
        if (!throw_at.empty() && entry.rfind(throw_at, 0) == 0)
            throw std::runtime_error("scripted throw at " + entry);
    }
    std::vector<std::string>& log_;
};

class RecordingQbit : public mb::QbittorrentClient {
public:
    explicit RecordingQbit(std::vector<std::string>& log)
        : mb::QbittorrentClient(Config{}), log_(log) {}
    std::set<std::string> fail_hashes;
    bool delete_torrent(const std::string& hash, bool delete_files) override {
        log_.push_back("qbit_delete:" + hash + (delete_files ? ":files" : ""));
        return fail_hashes.count(hash) == 0;
    }

private:
    std::vector<std::string>& log_;
};

mb::SeasonDeleteInputs inputs(int expected_files = 2) {
    mb::SeasonDeleteInputs in;
    in.title = "Show";
    in.expected_files = expected_files;
    return in;
}

bool logged(const std::vector<std::string>& log, const std::string& prefix) {
    for (const auto& e : log)
        if (e.rfind(prefix, 0) == 0) return true;
    return false;
}

// Nothing destructive ran: no cancel, no mark-failed, no purge, no delete.
void check_nothing_destructive(const std::vector<std::string>& log) {
    CHECK_FALSE(logged(log, "cancel:"));
    CHECK_FALSE(logged(log, "failed:"));
    CHECK_FALSE(logged(log, "qbit_delete:"));
    CHECK_FALSE(logged(log, "delete_files:"));
}

const std::string kAbortTail =
    " " + kDash + " season NOT deleted; retry is safe";
const std::string kCancelDisclosure =
    " " + kDash + " 1 in-flight download(s) were already cancelled and their "
    "partial data removed";
const std::string kPurgeDisclosure =
    "; any torrents for this season and their downloaded copies have "
    "already been removed";
const std::string kStuckOffWarning =
    " (WARNING: Sonarr's automatic re-download is still switched OFF " +
    kDash + " turn it back on in Sonarr under Settings > Download Clients)";

}  // namespace

TEST_CASE("season delete: happy path runs every stage in contract order",
          "[season_delete]") {
    std::vector<std::string> log;
    ScriptedSonarr sonarr(log);
    RecordingQbit qbit(log);
    const auto out =
        mb::run_delete_season(sonarr, &qbit, kSeries, kSeason, inputs());

    CHECK(log == std::vector<std::string>{
                     // (a) season flag, then this season's real episode ids
                     "season_mon:7:3:off", "episodes:7", "eps_mon:31,32:off",
                     // (b) authoritative history
                     "history:7:3",
                     // guard armed BEFORE anything destructive
                     "dlcfg", "redl:off",
                     // (c) one cancel per download, with blocklist
                     "queue", "cancel:100:bl",
                     // (d) GRABBED records only (imported 601 untouched)
                     "failed:501", "failed:502",
                     // (e) purge with data
                     "qbit_delete:aaa:files", "qbit_delete:bbb:files",
                     // (f) fresh listing, this season's files, LAST
                     "files:7", "delete_files:901,902",
                     // guard restored after the destructive work
                     "redl:on"});

    CHECK(out.removed);
    CHECK(out.abort_stage == mb::SeasonDeleteStage::None);
    CHECK(out.abort_reason.empty());
    CHECK(out.cancelled == 1);
    CHECK(out.records_to_blocklist == 2);
    CHECK(out.marked_failed == 2);
    CHECK(out.torrents_purged == 2);
    CHECK(out.torrents_left == 0);
    CHECK(out.files_deleted == 2);
    CHECK(out.guard_armed);
    CHECK_FALSE(out.redownload_restore_failed);
    CHECK(sonarr.redownload_on);

    CHECK(mb::compose_season_delete_toast(out) ==
          "Show: Season 3 removed " + kDash +
              " pick Season 3 in the list to download it again");
}

TEST_CASE("season delete: imported records are only a fallback for no grab",
          "[season_delete]") {
    std::vector<std::string> log;
    ScriptedSonarr sonarr(log);
    sonarr.history.grabbed_history_ids.clear();
    RecordingQbit qbit(log);
    const auto out =
        mb::run_delete_season(sonarr, &qbit, kSeries, kSeason, inputs());
    CHECK(out.removed);
    CHECK(logged(log, "failed:601"));
    CHECK(out.records_to_blocklist == 1);
    CHECK(out.marked_failed == 1);
    // A blocklist DID happen, so no "could come back" clause.
    CHECK(mb::compose_season_delete_toast(out).find("no release found") ==
          std::string::npos);
}

TEST_CASE("season delete: no grabbed and no imported record says nothing "
          "was blocklisted",
          "[season_delete]") {
    std::vector<std::string> log;
    ScriptedSonarr sonarr(log);
    sonarr.history = mb::SeasonHistory{};
    RecordingQbit qbit(log);
    const auto out =
        mb::run_delete_season(sonarr, &qbit, kSeries, kSeason, inputs());
    CHECK(out.removed);
    CHECK_FALSE(logged(log, "failed:"));
    CHECK_FALSE(logged(log, "qbit_delete:"));
    CHECK(out.records_to_blocklist == 0);
    CHECK(mb::compose_season_delete_toast(out) ==
          "Show: Season 3 removed " + kDash +
              " pick Season 3 in the list to download it again"
              " (no release found to blocklist " + kDash +
              " the same copy could come back)");
}

TEST_CASE("season delete: stage (a) failures abort before history",
          "[season_delete]") {
    struct Case {
        const char* name;
        void (*arm)(ScriptedSonarr&);
        const char* reason;
    };
    const Case cases[] = {
        {"season flag", [](ScriptedSonarr& s) { s.fail_season_monitor = true; },
         "couldn't unmonitor Season 3"},
        {"episode read", [](ScriptedSonarr& s) { s.fail_episodes_read = true; },
         "couldn't list episodes"},
        // Engaged-but-empty for THIS season is a misclassified failure, not
        // "nothing to unmonitor" (set_episodes_monitored({}) is a no-HTTP
        // true).
        {"no season episodes",
         [](ScriptedSonarr& s) {
             s.episodes.erase(s.episodes.begin(), s.episodes.begin() + 3);
         },
         "couldn't list the season's episodes"},
        {"episode flags",
         [](ScriptedSonarr& s) { s.fail_episodes_monitor = true; },
         "couldn't unmonitor the season's episodes"},
    };
    for (const auto& c : cases) {
        DYNAMIC_SECTION(c.name) {
            std::vector<std::string> log;
            ScriptedSonarr sonarr(log);
            c.arm(sonarr);
            RecordingQbit qbit(log);
            const auto out = mb::run_delete_season(sonarr, &qbit, kSeries,
                                                   kSeason, inputs());
            CHECK_FALSE(out.removed);
            CHECK(out.abort_stage == mb::SeasonDeleteStage::Unmonitor);
            CHECK_FALSE(logged(log, "history:"));
            CHECK_FALSE(logged(log, "dlcfg"));
            check_nothing_destructive(log);
            CHECK(mb::compose_season_delete_toast(out) ==
                  std::string("Show: ") + c.reason + kAbortTail);
        }
    }
}

TEST_CASE("season delete: failed history read aborts before anything "
          "destructive",
          "[season_delete]") {
    std::vector<std::string> log;
    ScriptedSonarr sonarr(log);
    sonarr.fail_history = true;
    RecordingQbit qbit(log);
    const auto out =
        mb::run_delete_season(sonarr, &qbit, kSeries, kSeason, inputs());
    CHECK_FALSE(out.removed);
    CHECK(out.abort_stage == mb::SeasonDeleteStage::History);
    CHECK(log.back() == "history:7:3");
    // The guard is never even built: no config read, no PUT.
    CHECK_FALSE(logged(log, "dlcfg"));
    CHECK_FALSE(logged(log, "redl:"));
    CHECK_FALSE(out.guard_armed);
    check_nothing_destructive(log);
    CHECK(mb::compose_season_delete_toast(out) ==
          "Show: Sonarr history unavailable" + kAbortTail);
}

TEST_CASE("season delete: a guard that cannot arm aborts before anything "
          "destructive",
          "[season_delete]") {
    std::vector<std::string> log;
    ScriptedSonarr sonarr(log);
    SECTION("config unreadable") {
        sonarr.fail_config_read = true;
    }
    SECTION("disable PUT refused") {
        sonarr.fail_disable = true;
    }
    RecordingQbit qbit(log);
    const auto out =
        mb::run_delete_season(sonarr, &qbit, kSeries, kSeason, inputs());
    CHECK_FALSE(out.removed);
    CHECK(out.abort_stage == mb::SeasonDeleteStage::ArmGuard);
    CHECK_FALSE(out.guard_armed);
    CHECK_FALSE(logged(log, "queue"));
    check_nothing_destructive(log);
    // Nothing was changed, so nothing is "restored" — no PUT(true).
    CHECK_FALSE(logged(log, "redl:on"));
    CHECK_FALSE(out.redownload_restore_failed);
    CHECK(mb::compose_season_delete_toast(out) ==
          "Show: couldn't pause Sonarr's automatic re-download" + kAbortTail);
}

TEST_CASE("season delete: owner already had auto-redownload off — armed, "
          "no PUT either way",
          "[season_delete]") {
    std::vector<std::string> log;
    ScriptedSonarr sonarr(log);
    sonarr.redownload_on = false;
    RecordingQbit qbit(log);
    const auto out =
        mb::run_delete_season(sonarr, &qbit, kSeries, kSeason, inputs());
    CHECK(out.removed);
    CHECK(out.guard_armed);
    CHECK_FALSE(logged(log, "redl:"));
    CHECK_FALSE(sonarr.redownload_on);
}

TEST_CASE("season delete: stage (c) failures abort with the guard restored",
          "[season_delete]") {
    std::vector<std::string> log;
    ScriptedSonarr sonarr(log);
    RecordingQbit qbit(log);
    std::string reason;
    SECTION("queue read") {
        sonarr.fail_queue_read = true;
        reason = "couldn't check for in-flight downloads";
    }
    SECTION("cancel refused") {
        sonarr.fail_cancel_ids = {100};
        reason = "couldn't cancel an in-flight download";
    }
    const auto out =
        mb::run_delete_season(sonarr, &qbit, kSeries, kSeason, inputs());
    CHECK_FALSE(out.removed);
    CHECK(out.abort_stage == mb::SeasonDeleteStage::CancelQueue);
    CHECK(out.cancelled == 0);
    CHECK_FALSE(logged(log, "failed:"));
    CHECK_FALSE(logged(log, "delete_files:"));
    CHECK(log.back() == "redl:on");
    CHECK(sonarr.redownload_on);
    CHECK(mb::compose_season_delete_toast(out) ==
          "Show: " + reason + kAbortTail);
}

TEST_CASE("season delete: mark-failed refusal still discloses step (c)'s "
          "cancels",
          "[season_delete]") {
    std::vector<std::string> log;
    ScriptedSonarr sonarr(log);
    sonarr.fail_mark_ids = {502};
    RecordingQbit qbit(log);
    const auto out =
        mb::run_delete_season(sonarr, &qbit, kSeries, kSeason, inputs());
    CHECK_FALSE(out.removed);
    CHECK(out.abort_stage == mb::SeasonDeleteStage::MarkFailed);
    CHECK(out.cancelled == 1);
    CHECK(out.marked_failed == 1);
    CHECK_FALSE(logged(log, "qbit_delete:"));
    CHECK_FALSE(logged(log, "files:"));
    CHECK_FALSE(logged(log, "delete_files:"));
    CHECK(log.back() == "redl:on");
    CHECK(mb::compose_season_delete_toast(out) ==
          "Show: couldn't blocklist the downloaded release" +
              kCancelDisclosure + kAbortTail);
}

TEST_CASE("season delete: torrent purge failures warn and continue",
          "[season_delete]") {
    std::vector<std::string> log;
    ScriptedSonarr sonarr(log);
    RecordingQbit qbit(log);
    qbit.fail_hashes = {"aaa"};
    const auto out =
        mb::run_delete_season(sonarr, &qbit, kSeries, kSeason, inputs());
    CHECK(out.removed);
    CHECK(out.torrents_left == 1);
    CHECK(out.torrents_purged == 1);
    // Both hashes were attempted, and the files were still deleted.
    CHECK(logged(log, "qbit_delete:bbb"));
    CHECK(logged(log, "delete_files:901,902"));
    CHECK(mb::compose_season_delete_toast(out) ==
          "Show: Season 3 removed " + kDash +
              " pick Season 3 in the list to download it again"
              " (a torrent needs manual cleanup in qBittorrent)");
}

TEST_CASE("season delete: null qBittorrent skips the purge", "[season_delete]") {
    std::vector<std::string> log;
    ScriptedSonarr sonarr(log);
    const auto out =
        mb::run_delete_season(sonarr, nullptr, kSeries, kSeason, inputs());
    CHECK(out.removed);
    CHECK(out.torrents_purged == 0);
    CHECK(out.torrents_left == 0);
    CHECK_FALSE(logged(log, "qbit_delete:"));
}

TEST_CASE("season delete: stage (f) aborts disclose step (e)'s purge",
          "[season_delete]") {
    std::vector<std::string> log;
    ScriptedSonarr sonarr(log);
    RecordingQbit qbit(log);
    std::string reason;
    SECTION("listing failed") {
        sonarr.fail_files_read = true;
        reason = "couldn't list episode files";
    }
    SECTION("listing empty against known files") {
        sonarr.files.clear();
        reason = "couldn't list the season's files";
    }
    SECTION("delete refused") {
        sonarr.fail_delete = true;
        reason = "couldn't delete the season's files";
    }
    const auto out =
        mb::run_delete_season(sonarr, &qbit, kSeries, kSeason, inputs());
    CHECK_FALSE(out.removed);
    CHECK(out.abort_stage == mb::SeasonDeleteStage::DeleteFiles);
    CHECK(out.files_deleted == 0);
    CHECK(log.back() == "redl:on");
    CHECK(mb::compose_season_delete_toast(out) ==
          "Show: " + reason + kCancelDisclosure + kPurgeDisclosure +
              kAbortTail);
}

TEST_CASE("season delete: purge-only disclosure leads with its own dash",
          "[season_delete]") {
    // No queue rows for the season, so the abort names only the purge.
    std::vector<std::string> log;
    ScriptedSonarr sonarr(log);
    sonarr.queue.clear();
    sonarr.fail_files_read = true;
    RecordingQbit qbit(log);
    const auto out =
        mb::run_delete_season(sonarr, &qbit, kSeries, kSeason, inputs());
    CHECK(mb::compose_season_delete_toast(out) ==
          "Show: couldn't list episode files " + kDash +
              " any torrents for this season and their downloaded copies "
              "have already been removed" + kAbortTail);
}

TEST_CASE("season delete: a download-only season with no files completes",
          "[season_delete]") {
    std::vector<std::string> log;
    ScriptedSonarr sonarr(log);
    sonarr.files.clear();
    RecordingQbit qbit(log);
    const auto out = mb::run_delete_season(sonarr, &qbit, kSeries, kSeason,
                                           inputs(/*expected_files=*/0));
    CHECK(out.removed);
    CHECK(out.files_deleted == 0);
    CHECK(logged(log, "delete_files:"));  // empty list: the client no-ops
}

TEST_CASE("season delete: a defeated guard restore is SAID in the toast",
          "[season_delete]") {
    std::vector<std::string> log;
    ScriptedSonarr sonarr(log);
    sonarr.fail_restore = true;
    RecordingQbit qbit(log);

    SECTION("success path") {
        const auto out =
            mb::run_delete_season(sonarr, &qbit, kSeries, kSeason, inputs());
        CHECK(out.removed);
        CHECK(out.redownload_restore_failed);
        // Exactly three attempts — the guard's destructor must not start a
        // second retry round after the explicit restore already lost.
        CHECK(sonarr.restore_attempts == 3);
        CHECK(mb::compose_season_delete_toast(out) ==
              "Show: Season 3 removed " + kDash +
                  " pick Season 3 in the list to download it again" +
                  kStuckOffWarning);
    }
    SECTION("abort path") {
        sonarr.fail_mark_ids = {501};
        const auto out =
            mb::run_delete_season(sonarr, &qbit, kSeries, kSeason, inputs());
        CHECK_FALSE(out.removed);
        CHECK(out.redownload_restore_failed);
        CHECK(sonarr.restore_attempts == 3);
        CHECK(mb::compose_season_delete_toast(out) ==
              "Show: couldn't blocklist the downloaded release" +
                  kCancelDisclosure + kAbortTail + kStuckOffWarning);
    }
}

TEST_CASE("season delete: a throw from the client still restores the guard",
          "[season_delete]") {
    std::vector<std::string> log;
    ScriptedSonarr sonarr(log);
    RecordingQbit qbit(log);
    SECTION("throw during (c)") { sonarr.throw_at = "cancel:"; }
    SECTION("throw during (d)") { sonarr.throw_at = "failed:502"; }
    SECTION("throw during (f)") { sonarr.throw_at = "delete_files:"; }
    CHECK_THROWS_AS(
        mb::run_delete_season(sonarr, &qbit, kSeries, kSeason, inputs()),
        std::runtime_error);
    CHECK(log.back() == "redl:on");
    CHECK(sonarr.redownload_on);
    CHECK(sonarr.restore_attempts == 1);
}

TEST_CASE("season delete: a throw before the guard arms makes no PUT",
          "[season_delete]") {
    std::vector<std::string> log;
    ScriptedSonarr sonarr(log);
    sonarr.throw_at = "history:";
    RecordingQbit qbit(log);
    CHECK_THROWS(
        mb::run_delete_season(sonarr, &qbit, kSeries, kSeason, inputs()));
    CHECK_FALSE(logged(log, "redl:"));
    check_nothing_destructive(log);
}
