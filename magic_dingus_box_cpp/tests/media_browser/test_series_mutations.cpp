// Unit tests for series_mutations.{h,cpp} — SeriesDetail's mutation-worker
// bodies (Download Season N, Add Season N, Whole series… press 1 + 2,
// Remove), extracted from series_detail_screen.cpp. What these pin:
//   * the Sonarr / qBittorrent call ORDER of each flow (one shared log), and
//     the calls a failure must NOT make (no search after a refused monitor,
//     no remove after an unanswered read);
//   * every toast's wording and precedence, which used to be reachable only
//     by driving the kiosk;
//   * what each outcome publishes (record, settled flag, removed, the
//     deferred start season, the disk verdict) — the screen's drain acts on
//     exactly these fields.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "media_browser/qbittorrent/qbittorrent_client.h"
#include "media_browser/series_mutations.h"
#include "media_browser/sonarr/sonarr_mock.h"

namespace mb = media_browser;
namespace ui = media_browser::ui;

namespace {

const std::string kDash = "\xE2\x80\x94";
constexpr int64_t kGiB = 1024LL * 1024 * 1024;

std::string join(const std::vector<int>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ",";
        s += std::to_string(v[i]);
    }
    return s;
}

mb::Series make_series(int sid, std::vector<mb::Season> seasons) {
    mb::Series s;
    s.sonarr_id = sid;
    s.title = "Show";
    s.seasons = std::move(seasons);
    return s;
}

mb::Season season(int n, bool monitored, int eps, int files) {
    mb::Season s;
    s.season_number = n;
    s.monitored = monitored;
    s.episode_count = eps;
    s.episode_file_count = files;
    return s;
}

mb::EpisodeInfo episode(int id, int season_n, int ep_n, bool has_file = false) {
    mb::EpisodeInfo e;
    e.id = id;
    e.season_number = season_n;
    e.episode_number = ep_n;
    e.has_file = has_file;
    return e;
}

// SonarrMockClient with every call these flows make recorded into a log,
// each outcome injectable, and last_error() behaving like the real client's
// (cleared on entry to every call, set by a failure).
class ScriptedSonarr : public mb::SonarrMockClient {
public:
    using mb::SonarrMockClient::cancel_queue_item;

    explicit ScriptedSonarr(std::vector<std::string>& log) : log_(log) {
        mb::QualityProfile p;
        p.id = 4; p.name = "HD"; profiles.push_back(p);
        p.id = 9; p.name = "Any"; profiles.push_back(p);
        episodes = {episode(31, 3, 1), episode(32, 3, 2), episode(0, 3, 3),
                    episode(21, 2, 1)};
        fresh = make_series(7, {season(1, true, 10, 10), season(3, true, 8, 0)});
    }

    // Fixtures.
    std::vector<mb::QualityProfile> profiles;
    std::vector<mb::EpisodeInfo> episodes;
    std::optional<mb::Series> fresh;  // get_series' answer
    mb::AddSeriesResult add_result{true, true, make_series(7, {})};
    std::vector<mb::RootFolder> roots;
    std::optional<std::vector<mb::SonarrQueueItem>> queue =
        std::vector<mb::SonarrQueueItem>{};
    std::optional<std::vector<std::string>> hashes =
        std::vector<std::string>{};

    // Failure injection.
    std::string profiles_error;  // set as last_error by get_quality_profiles
    bool fail_add = false;
    std::set<int> fail_season_monitor;
    bool fail_episodes_read = false;
    bool fail_episodes_monitor = false;
    bool fail_season_search = false;
    bool fail_series_search = false;
    bool fail_episode_search = false;
    std::set<int> fail_cancel_ids;
    bool fail_remove = false;

    std::vector<mb::QualityProfile> get_quality_profiles() override {
        rec("profiles");
        if (!profiles_error.empty()) set_error(profiles_error);
        return profiles;
    }
    mb::AddSeriesResult add_series(int tmdb_id, int qp, bool monitor,
                                   const std::string& title) override {
        rec("add:" + std::to_string(tmdb_id) + ":" + std::to_string(qp) +
            (monitor ? ":mon" : ":none") + ":" + title);
        if (fail_add) {
            set_error("add refused");
            return mb::AddSeriesResult{};
        }
        return add_result;
    }
    bool set_season_monitored(int sid, int s, bool on) override {
        rec("season_mon:" + std::to_string(sid) + ":" + std::to_string(s) +
            (on ? ":on" : ":off"));
        if (fail_season_monitor.count(s)) {
            set_error("season refused");
            return false;
        }
        return true;
    }
    std::optional<std::vector<mb::EpisodeInfo>> get_episodes_checked(
            int sid) override {
        rec("episodes:" + std::to_string(sid));
        if (fail_episodes_read) return std::nullopt;
        return episodes;
    }
    bool set_episodes_monitored(const std::vector<int>& ids, bool on) override {
        rec("eps_mon:" + join(ids) + (on ? ":on" : ":off"));
        return !fail_episodes_monitor;
    }
    bool trigger_season_search(int sid, int s) override {
        rec("season_search:" + std::to_string(sid) + ":" + std::to_string(s));
        return !fail_season_search;
    }
    bool trigger_series_search(int sid) override {
        rec("series_search:" + std::to_string(sid));
        return !fail_series_search;
    }
    bool trigger_episode_search(int eid) override {
        rec("episode_search:" + std::to_string(eid));
        return !fail_episode_search;
    }
    std::optional<mb::Series> get_series(int sid) override {
        rec("series:" + std::to_string(sid));
        return fresh;
    }
    std::vector<mb::RootFolder> get_root_folders() override {
        rec("roots");
        return roots;
    }
    std::optional<std::vector<mb::SonarrQueueItem>> get_queue_checked() override {
        rec("queue");
        return queue;
    }
    bool cancel_queue_item(int qid) override {
        rec("cancel:" + std::to_string(qid));
        if (fail_cancel_ids.count(qid)) {
            set_error("cancel " + std::to_string(qid) + " refused");
            return false;
        }
        return true;
    }
    std::optional<std::vector<std::string>>
    get_series_download_hashes_checked(int sid) override {
        rec("hashes:" + std::to_string(sid));
        return hashes;
    }
    bool remove_series(int sid, bool delete_files) override {
        rec("remove:" + std::to_string(sid) + (delete_files ? ":files" : ""));
        if (fail_remove) {
            set_error("remove refused");
            return false;
        }
        return true;
    }

private:
    void rec(const std::string& entry) {
        set_error({});  // the real client clears last_error on every call
        log_.push_back(entry);
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

mb::SonarrQueueItem qrow(int id, int series_id, int season_n,
                         const std::string& download_id) {
    mb::SonarrQueueItem q;
    q.id = id;
    q.series_id = series_id;
    q.season_number = season_n;
    q.download_id = download_id;
    return q;
}

}  // namespace

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------

TEST_CASE("pick_series_quality_profile_id: 'Any' by name, else the first, else 0",
          "[series_mutations]") {
    std::vector<mb::QualityProfile> ps;
    CHECK(mb::pick_series_quality_profile_id(ps) == 0);
    mb::QualityProfile p;
    p.id = 4; p.name = "HD"; ps.push_back(p);
    CHECK(mb::pick_series_quality_profile_id(ps) == 4);
    p.id = 9; p.name = "Any"; ps.push_back(p);
    CHECK(mb::pick_series_quality_profile_id(ps) == 9);
    // Name match is exact — "any" is not "Any".
    std::vector<mb::QualityProfile> lower{{5, "any", 0, {}}, {6, "Other", 0, {}}};
    CHECK(mb::pick_series_quality_profile_id(lower) == 5);
}

TEST_CASE("lowest_regular_season: skips specials, picks the lowest number",
          "[series_mutations]") {
    auto none = mb::lowest_regular_season(make_series(1, {season(0, true, 3, 3)}));
    CHECK(none.season == 0);
    auto s = mb::lowest_regular_season(make_series(
        1, {season(4, true, 8, 0), season(0, true, 3, 3), season(2, false, 8, 5),
            season(3, true, 8, 8)}));
    CHECK(s.season == 2);
    CHECK_FALSE(s.monitored);
    CHECK(s.files == 5);
}

TEST_CASE("compose_start_season_toast: re-monitor problems lead",
          "[series_mutations]") {
    const std::string reenable =
        "Show: Season 3 monitored, but its episodes couldn't be re-enabled " +
        kDash + " a previously deleted season may not re-download; try again";
    CHECK(mb::compose_start_season_toast("Show", 3, std::nullopt, false, true) ==
          reenable);
    CHECK(mb::compose_start_season_toast("Show", 3, 0, true, false) == reenable);
    CHECK(mb::compose_start_season_toast("Show", 3, 4, false, true) ==
          "Show: Season 3 search started");
    CHECK(mb::compose_start_season_toast("Show", 3, 4, false, false) ==
          "Show: Season 3 monitored, but the search didn't start " + kDash +
              " Sonarr will pick it up on RSS");
}

TEST_CASE("compose_whole_series_toast: precedence of the two signals",
          "[series_mutations]") {
    // Every PUT failed beats everything, even a successful search.
    CHECK(mb::compose_whole_series_toast("Show", 3, 3, 5, true) ==
          "Show: couldn't monitor seasons " + kDash + " is Sonarr running?");
    // total == 0: "all failed" never fires on an empty list.
    CHECK(mb::compose_whole_series_toast("Show", 0, 0, 0, true) ==
          "Show: whole-series search started");
    CHECK(mb::compose_whole_series_toast("Show", 3, 1, std::nullopt, true) ==
          "Show: seasons monitored, but their episodes couldn't be re-enabled " +
              kDash + " a previously deleted season may not re-download; try again");
    CHECK(mb::compose_whole_series_toast("Show", 3, 1, 5, false) ==
          "Show: seasons monitored, but the search didn't start " + kDash +
              " Sonarr will pick them up on RSS");
    CHECK(mb::compose_whole_series_toast("Show", 3, 1, 5, true) ==
          "Show: search started (1 of 3 seasons couldn't be monitored)");
    // A zero re-monitor count is tolerated on this path.
    CHECK(mb::compose_whole_series_toast("Show", 3, 0, 0, true) ==
          "Show: whole-series search started");
}

TEST_CASE("compose_preflight_toast: Block names both numbers, Allow is silent",
          "[series_mutations]") {
    CHECK(mb::compose_preflight_toast("Show", ui::DiskVerdict::Block, 30 * kGiB,
                                      25 * kGiB) ==
          "Show: not enough space " + kDash + " needs ~30 GB (est), 25 GB free "
          "(20 GB floor)");
    CHECK(mb::compose_preflight_toast("Show", ui::DiskVerdict::Block, 30 * kGiB,
                                      std::nullopt) ==
          "Show: not enough space " + kDash + " needs ~30 GB (est), 0 GB free "
          "(20 GB floor)");
    CHECK(mb::compose_preflight_toast("Show", ui::DiskVerdict::WarnOnly, 1, {}) ==
          "Show: couldn't check free space " + kDash + " confirm to proceed anyway");
    CHECK(mb::compose_preflight_toast("Show", ui::DiskVerdict::Allow, 1, 1).empty());
}

// ---------------------------------------------------------------------------
// monitor_episodes_for_seasons + fire_episode1_search
// ---------------------------------------------------------------------------

TEST_CASE("monitor_episodes_for_seasons: scoped ids, never id 0, split contract",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    CHECK(mb::monitor_episodes_for_seasons(s, 7, {}) == 0);
    CHECK(log.empty());  // nothing to do -> no GET

    CHECK(mb::monitor_episodes_for_seasons(s, 7, {3}) == 2);
    CHECK(log == std::vector<std::string>{"episodes:7", "eps_mon:31,32:on"});

    log.clear();
    CHECK(mb::monitor_episodes_for_seasons(s, 7, {9}) == 0);  // engaged, empty
    CHECK(log == std::vector<std::string>{"episodes:7", "eps_mon::on"});

    s.fail_episodes_read = true;
    CHECK_FALSE(mb::monitor_episodes_for_seasons(s, 7, {3}).has_value());
    s.fail_episodes_read = false;
    s.fail_episodes_monitor = true;
    CHECK_FALSE(mb::monitor_episodes_for_seasons(s, 7, {3}).has_value());
}

TEST_CASE("fire_episode1_search: only an unfiled E1 with a real id",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    mb::fire_episode1_search(s, 0, 3, "Show");
    mb::fire_episode1_search(s, 7, 0, "Show");
    CHECK(log.empty());

    mb::fire_episode1_search(s, 7, 3, "Show");
    CHECK(log == std::vector<std::string>{"episodes:7", "episode_search:31"});

    log.clear();
    s.episodes = {episode(31, 3, 1, /*has_file=*/true)};
    mb::fire_episode1_search(s, 7, 3, "Show");
    CHECK(log == std::vector<std::string>{"episodes:7"});

    log.clear();
    s.episodes = {episode(0, 3, 1)};
    mb::fire_episode1_search(s, 7, 3, "Show");
    CHECK(log == std::vector<std::string>{"episodes:7"});

    log.clear();
    s.episodes = {episode(40, 4, 1)};  // no S3E1 at all
    mb::fire_episode1_search(s, 7, 3, "Show");
    CHECK(log == std::vector<std::string>{"episodes:7"});
}

// ---------------------------------------------------------------------------
// run_start_season_download
// ---------------------------------------------------------------------------

TEST_CASE("run_start_season_download: happy path order and publish",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    const auto out = mb::run_start_season_download(s, 7, 3, "Show");
    CHECK(log == std::vector<std::string>{
                     "season_mon:7:3:on", "episodes:7", "eps_mon:31,32:on",
                     "season_search:7:3", "episodes:7", "episode_search:31",
                     "series:7"});
    CHECK(out.toast == "Show: Season 3 search started");
    REQUIRE(out.series.has_value());
    CHECK(out.series->sonarr_id == 7);
    CHECK(out.settled);
    CHECK_FALSE(out.removed);
    CHECK_FALSE(out.start_season.has_value());
    CHECK_FALSE(out.have_verdict);
}

TEST_CASE("run_start_season_download: refused monitor stops before anything else",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    s.fail_season_monitor = {3};
    const auto out = mb::run_start_season_download(s, 7, 3, "Show");
    CHECK(log == std::vector<std::string>{"season_mon:7:3:on"});
    CHECK(out.toast ==
          "Show: couldn't monitor season 3 " + kDash + " season refused");
    CHECK_FALSE(out.series.has_value());
    CHECK(out.settled);
}

TEST_CASE("run_start_season_download: a suspiciously empty re-monitor never searches",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    s.episodes = {episode(21, 2, 1)};  // nothing for season 3
    const auto out = mb::run_start_season_download(s, 7, 3, "Show");
    CHECK(log == std::vector<std::string>{"season_mon:7:3:on", "episodes:7",
                                          "eps_mon::on", "series:7"});
    CHECK(out.toast.find("its episodes couldn't be re-enabled") != std::string::npos);
}

TEST_CASE("run_start_season_download: a failed re-monitor still searches",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    s.fail_episodes_read = true;
    const auto out = mb::run_start_season_download(s, 7, 3, "Show");
    // Search fires (nullopt is not "suspiciously empty"); Quick Start's own
    // episode read fails too and quietly skips.
    CHECK(log == std::vector<std::string>{"season_mon:7:3:on", "episodes:7",
                                          "season_search:7:3", "episodes:7",
                                          "series:7"});
    CHECK(out.toast.find("its episodes couldn't be re-enabled") != std::string::npos);
}

TEST_CASE("run_start_season_download: search refused skips Quick Start",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    s.fail_season_search = true;
    s.fresh.reset();
    const auto out = mb::run_start_season_download(s, 7, 3, "Show");
    CHECK(log == std::vector<std::string>{"season_mon:7:3:on", "episodes:7",
                                          "eps_mon:31,32:on",
                                          "season_search:7:3", "series:7"});
    CHECK(out.toast == "Show: Season 3 monitored, but the search didn't start " +
                           kDash + " Sonarr will pick it up on RSS");
    CHECK_FALSE(out.series.has_value());
    CHECK(out.settled);
}

TEST_CASE("run_start_season_download: an unrefreshed re-read publishes unsettled",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    s.fresh = make_series(7, {season(3, true, 0, 0)});
    const auto out = mb::run_start_season_download(s, 7, 3, "Show");
    REQUIRE(out.series.has_value());
    CHECK_FALSE(out.settled);
}

// ---------------------------------------------------------------------------
// run_add_at_season
// ---------------------------------------------------------------------------

TEST_CASE("run_add_at_season: no quality profile vs. no answer",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    s.profiles.clear();
    auto out = mb::run_add_at_season(s, 1396, 1, "Show");
    CHECK(out.toast ==
          "Show: Sonarr has no quality profile " + kDash + " not added");
    CHECK(log == std::vector<std::string>{"profiles"});

    s.profiles_error = "timeout";
    out = mb::run_add_at_season(s, 1396, 1, "Show");
    CHECK(out.toast == "Show: couldn't reach Sonarr " + kDash + " timeout");
}

TEST_CASE("run_add_at_season: Season > 1 adds with nothing monitored, starts later",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    s.add_result = {true, true, make_series(7, {season(1, false, 8, 0)})};
    const auto out = mb::run_add_at_season(s, 1396, 4, "Show");
    CHECK(log == std::vector<std::string>{"profiles", "add:1396:9:none:Show"});
    CHECK(out.toast.empty());
    REQUIRE(out.start_season.has_value());
    CHECK(*out.start_season == 4);
    CHECK(out.start_title == "Show");
    REQUIRE(out.series.has_value());
    CHECK(out.settled);
}

TEST_CASE("run_add_at_season: Season > 1 unsettled never falls back to Season 1",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    s.add_result = {true, false, make_series(7, {})};
    auto out = mb::run_add_at_season(s, 1396, 4, "Show");
    CHECK(out.toast == "Show: added " + kDash + " choose the season again in a moment");
    CHECK_FALSE(out.start_season.has_value());
    REQUIRE(out.series.has_value());
    CHECK_FALSE(out.settled);

    // Settled but no id: same honest stop.
    s.add_result = {true, true, make_series(0, {season(1, false, 8, 0)})};
    out = mb::run_add_at_season(s, 1396, 4, "Show");
    CHECK_FALSE(out.start_season.has_value());
    CHECK_FALSE(out.settled);

    s.fail_add = true;
    out = mb::run_add_at_season(s, 1396, 4, "Show");
    CHECK(out.toast == "Show: add failed " + kDash + " add refused");
    CHECK_FALSE(out.series.has_value());
}

TEST_CASE("run_add_at_season: Season 1 — every settled branch",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);

    SECTION("unsettled add: syncing") {
        s.add_result = {true, false, make_series(7, {})};
        const auto out = mb::run_add_at_season(s, 1396, 1, "Show");
        CHECK(log == std::vector<std::string>{"profiles", "add:1396:9:mon:Show"});
        CHECK(out.toast == "Show: added " + kDash + " syncing seasons\xE2\x80\xA6");
        CHECK_FALSE(out.settled);
        REQUIRE(out.series.has_value());
    }
    SECTION("monitored, nothing on disk: search started + Quick Start") {
        s.episodes = {episode(11, 1, 1)};
        s.add_result = {true, true,
                        make_series(7, {season(0, false, 2, 0),
                                        season(1, true, 8, 0)})};
        const auto out = mb::run_add_at_season(s, 1396, 1, "Show");
        CHECK(log == std::vector<std::string>{"profiles", "add:1396:9:mon:Show",
                                              "episodes:7", "episode_search:11"});
        CHECK(out.toast == "Show: Season 1 search started");
        REQUIRE(out.series.has_value());
        CHECK(out.settled);
    }
    SECTION("monitored with files: already in the library, nothing started") {
        s.add_result = {true, true, make_series(7, {season(1, true, 8, 8)})};
        const auto out = mb::run_add_at_season(s, 1396, 1, "Show");
        CHECK(log == std::vector<std::string>{"profiles", "add:1396:9:mon:Show"});
        CHECK(out.toast == "Show: already in your TV library");
    }
    SECTION("find-existing branch, unmonitored: monitor + search explicitly") {
        s.episodes = {episode(51, 2, 1)};
        // No season 1 at all: the lowest regular season is 2.
        s.add_result = {true, true, make_series(7, {season(2, false, 8, 0)})};
        const auto out = mb::run_add_at_season(s, 1396, 1, "Show");
        CHECK(log == std::vector<std::string>{
                         "profiles", "add:1396:9:mon:Show", "season_mon:7:2:on",
                         "season_search:7:2", "episodes:7", "episode_search:51",
                         "series:7"});
        CHECK(out.toast == "Show: Season 2 search started");
        REQUIRE(out.series.has_value());
        CHECK(out.series->seasons.size() == 2);  // the re-read, not the add's
    }
    SECTION("find-existing branch, monitor refused") {
        s.fail_season_monitor = {1};
        s.add_result = {true, true, make_series(7, {season(1, false, 8, 0)})};
        const auto out = mb::run_add_at_season(s, 1396, 1, "Show");
        CHECK(out.toast ==
              "Show: couldn't monitor season 1 " + kDash + " season refused");
        CHECK_FALSE(out.series.has_value());
    }
    SECTION("find-existing branch, search refused: RSS, add's record if re-read fails") {
        s.fail_season_search = true;
        s.fresh.reset();
        s.add_result = {true, true, make_series(7, {season(1, false, 8, 0)})};
        const auto out = mb::run_add_at_season(s, 1396, 1, "Show");
        CHECK(log == std::vector<std::string>{"profiles", "add:1396:9:mon:Show",
                                              "season_mon:7:1:on",
                                              "season_search:7:1", "series:7"});
        CHECK(out.toast == "Show: Season 1 monitored, but the search didn't "
                           "start " + kDash + " Sonarr will pick it up on RSS");
        REQUIRE(out.series.has_value());
        CHECK(out.series->seasons.size() == 1);
        CHECK(out.settled);
    }
    SECTION("no ordinary season: claim nothing") {
        s.add_result = {true, true, make_series(7, {season(0, true, 2, 0)})};
        const auto out = mb::run_add_at_season(s, 1396, 1, "Show");
        CHECK(out.toast == "Show: added to your TV library");
        CHECK(out.settled);
    }
}

// ---------------------------------------------------------------------------
// run_whole_series
// ---------------------------------------------------------------------------

TEST_CASE("run_whole_series: in-library — PUT failures counted, re-monitor scoped",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    s.fail_season_monitor = {2};
    const auto out = mb::run_whole_series(s, false, 1396, 7, {2, 3, 4}, "Show");
    CHECK(log == std::vector<std::string>{
                     "season_mon:7:2:on", "season_mon:7:3:on", "season_mon:7:4:on",
                     "episodes:7", "eps_mon:31,32:on", "series_search:7",
                     "series:7"});
    CHECK(out.toast == "Show: search started (1 of 3 seasons couldn't be monitored)");
    REQUIRE(out.series.has_value());
    CHECK(out.settled);
}

TEST_CASE("run_whole_series: every PUT failed", "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    s.fail_season_monitor = {2, 3};
    const auto out = mb::run_whole_series(s, false, 1396, 7, {2, 3}, "Show");
    CHECK(out.toast == "Show: couldn't monitor seasons " + kDash +
                           " is Sonarr running?");
}

TEST_CASE("run_whole_series: no id yet", "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    const auto out = mb::run_whole_series(s, false, 1396, 0, {1}, "Show");
    CHECK(log.empty());
    CHECK(out.toast == "Show: added, but Sonarr hasn't assigned an id yet " +
                           kDash + " try Whole series again in a moment");
    CHECK_FALSE(out.series.has_value());
    CHECK(out.settled);
}

TEST_CASE("run_whole_series: pre-add", "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);

    SECTION("settled add, then the in-library sequence on its id") {
        s.add_result = {true, true, make_series(12, {season(1, true, 8, 0)})};
        const auto out = mb::run_whole_series(s, true, 1396, 0, {2}, "Show");
        CHECK(log == std::vector<std::string>{
                         "profiles", "add:1396:9:mon:Show", "season_mon:12:2:on",
                         "episodes:12", "eps_mon:21:on", "series_search:12",
                         "series:12"});
        CHECK(out.toast == "Show: whole-series search started");
    }
    SECTION("unsettled add stops before any season PUT") {
        s.add_result = {true, false, make_series(12, {})};
        const auto out = mb::run_whole_series(s, true, 1396, 0, {1, 2}, "Show");
        CHECK(log == std::vector<std::string>{"profiles", "add:1396:9:mon:Show"});
        CHECK(out.toast == "Show: added " + kDash + " syncing seasons; the "
                           "whole-series option returns when Sonarr finishes");
        CHECK_FALSE(out.settled);
        REQUIRE(out.series.has_value());
    }
    SECTION("settled add without an id publishes the added record unsettled") {
        s.add_result = {true, true, make_series(0, {season(1, true, 8, 0)})};
        const auto out = mb::run_whole_series(s, true, 1396, 0, {1}, "Show");
        CHECK(out.toast.find("hasn't assigned an id yet") != std::string::npos);
        REQUIRE(out.series.has_value());
        CHECK_FALSE(out.settled);
    }
    SECTION("re-read fails: the added record, unsettled") {
        s.fresh.reset();
        s.add_result = {true, true, make_series(12, {season(1, true, 8, 0)})};
        const auto out = mb::run_whole_series(s, true, 1396, 0, {2}, "Show");
        REQUIRE(out.series.has_value());
        CHECK(out.series->sonarr_id == 12);
        CHECK_FALSE(out.settled);
    }
    SECTION("add refused") {
        s.fail_add = true;
        const auto out = mb::run_whole_series(s, true, 1396, 0, {2}, "Show");
        CHECK(out.toast == "Show: add failed " + kDash + " add refused");
        CHECK_FALSE(out.series.has_value());
    }
    SECTION("no profile") {
        s.profiles.clear();
        const auto out = mb::run_whole_series(s, true, 1396, 0, {2}, "Show");
        CHECK(out.toast ==
              "Show: Sonarr has no quality profile " + kDash + " not added");
    }
}

TEST_CASE("run_whole_series: search refused and re-monitor failure toasts",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    s.fail_series_search = true;
    auto out = mb::run_whole_series(s, false, 1396, 7, {3}, "Show");
    CHECK(out.toast.find("but the search didn't start") != std::string::npos);

    s.fail_series_search = false;
    s.fail_episodes_read = true;
    out = mb::run_whole_series(s, false, 1396, 7, {3}, "Show");
    CHECK(out.toast.find("their episodes couldn't be re-enabled") !=
          std::string::npos);
}

// ---------------------------------------------------------------------------
// run_whole_series_preflight
// ---------------------------------------------------------------------------

TEST_CASE("run_whole_series_preflight: Sonarr's positive reading wins",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    s.roots = {{1, "/data/library/movies", 900 * kGiB, 0},
               {2, "/data/library/tv", 500 * kGiB, 0}};
    std::vector<std::string> probed;
    const auto out = mb::run_whole_series_preflight(
        s, 30 * kGiB, "Show", [&](const std::string& p) {
            probed.push_back(p);
            return std::optional<int64_t>(0);
        });
    CHECK(probed.empty());
    CHECK(out.have_verdict);
    CHECK(out.verdict == ui::DiskVerdict::Allow);
    CHECK(out.estimate == 30 * kGiB);
    CHECK(out.toast.empty());
}

TEST_CASE("run_whole_series_preflight: an ambiguous Sonarr zero falls to the host path",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    s.roots = {{2, "/data/library/tv", 0, 0}};
    std::vector<std::string> probed;
    auto out = mb::run_whole_series_preflight(
        s, 30 * kGiB, "Show", [&](const std::string& p) {
            probed.push_back(p);
            return std::optional<int64_t>(40 * kGiB);
        });
    // The MATCHED folder resolved through the client's prefix map, not the
    // compiled literal.
    CHECK(probed == std::vector<std::string>{"/mnt/ssd/library/tv/"});
    CHECK(out.verdict == ui::DiskVerdict::Block);  // 30 > 40 - 20
    CHECK(out.toast == "Show: not enough space " + kDash +
                           " needs ~30 GB (est), 40 GB free (20 GB floor)");

    // A REAL zero from the host is a full disk: Block, not WarnOnly.
    out = mb::run_whole_series_preflight(
        s, 1, "Show", [](const std::string&) { return std::optional<int64_t>(0); });
    CHECK(out.verdict == ui::DiskVerdict::Block);

    // A failed stat is no reading at all: warn, never wedge.
    out = mb::run_whole_series_preflight(
        s, 1, "Show", [](const std::string&) { return std::optional<int64_t>(); });
    CHECK(out.verdict == ui::DiskVerdict::WarnOnly);
    CHECK(out.toast == "Show: couldn't check free space " + kDash +
                           " confirm to proceed anyway");
}

TEST_CASE("run_whole_series_preflight: no /tv folder uses the compiled default",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    s.roots = {{1, "/data/library/movies", 900 * kGiB, 0}};
    std::vector<std::string> probed;
    const auto out = mb::run_whole_series_preflight(
        s, 1, "Show", [&](const std::string& p) {
            probed.push_back(p);
            return std::optional<int64_t>(100 * kGiB);
        });
    CHECK(probed == std::vector<std::string>{"/mnt/ssd/library/tv"});
    CHECK(out.verdict == ui::DiskVerdict::Allow);
}

// ---------------------------------------------------------------------------
// run_remove_series
// ---------------------------------------------------------------------------

TEST_CASE("run_remove_series: happy path — one cancel per download, purge, remove",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    RecordingQbit q(log);
    s.queue = std::vector<mb::SonarrQueueItem>{
        qrow(100, 7, 1, "AAA"), qrow(101, 7, 1, "AAA"), qrow(102, 7, 2, "BBB"),
        qrow(103, 8, 1, "CCC")};
    s.hashes = std::vector<std::string>{"aaa", "zzz"};
    const auto out = mb::run_remove_series(s, &q, 7, "Show");
    CHECK(log == std::vector<std::string>{
                     "queue", "cancel:100", "cancel:102", "hashes:7",
                     "qbit_delete:aaa:files", "qbit_delete:zzz:files",
                     "remove:7:files"});
    CHECK(out.removed);
    CHECK(out.toast == "Show: removed from the TV library");
}

TEST_CASE("run_remove_series: an unanswered queue read aborts before anything",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    RecordingQbit q(log);
    s.queue.reset();
    const auto out = mb::run_remove_series(s, &q, 7, "Show");
    CHECK(log == std::vector<std::string>{"queue"});
    CHECK_FALSE(out.removed);
    CHECK(out.toast == "Show: couldn't check for in-flight downloads " + kDash +
                           " series NOT removed; retry is safe");
}

TEST_CASE("run_remove_series: a failed cancel aborts and keeps its diagnosis",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    RecordingQbit q(log);
    s.queue = std::vector<mb::SonarrQueueItem>{qrow(100, 7, 1, "AAA"),
                                               qrow(102, 7, 2, "BBB"),
                                               qrow(104, 7, 3, "DDD")};
    s.fail_cancel_ids = {102};
    auto out = mb::run_remove_series(s, &q, 7, "Show");
    // The later success (104) cleared last_error; the toast still carries the
    // reason captured AT the failing cancel.
    CHECK(log == std::vector<std::string>{"queue", "cancel:100", "cancel:102",
                                          "cancel:104"});
    CHECK_FALSE(out.removed);
    CHECK(out.toast == "Show: cancelled 2 download(s), then failed " + kDash +
                           " series NOT removed; retry is safe (cancel 102 refused)");

    log.clear();
    s.queue = std::vector<mb::SonarrQueueItem>{qrow(102, 7, 2, "BBB")};
    out = mb::run_remove_series(s, &q, 7, "Show");
    CHECK(out.toast == "Show: couldn't cancel 1 download(s) " + kDash +
                           " NOT removed (cancel 102 refused)");
}

TEST_CASE("run_remove_series: an unanswered history walk aborts with disclosure",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    RecordingQbit q(log);
    s.hashes.reset();
    auto out = mb::run_remove_series(s, &q, 7, "Show");
    CHECK(log == std::vector<std::string>{"queue", "hashes:7"});
    CHECK(out.toast == "Show: couldn't check for seeding torrents " + kDash +
                           " series NOT removed " + kDash + " Sonarr didn't answer");

    s.queue = std::vector<mb::SonarrQueueItem>{qrow(100, 7, 1, "AAA")};
    out = mb::run_remove_series(s, &q, 7, "Show");
    CHECK(out.toast == "Show: cancelled 1 download(s), then couldn't check for "
                       "seeding torrents " + kDash + " series NOT removed " +
                       kDash + " Sonarr didn't answer");
    CHECK_FALSE(out.removed);
}

TEST_CASE("run_remove_series: no qBittorrent skips the history walk",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    s.hashes.reset();  // would abort if it were consulted
    const auto out = mb::run_remove_series(s, nullptr, 7, "Show");
    CHECK(log == std::vector<std::string>{"queue", "remove:7:files"});
    CHECK(out.removed);
}

TEST_CASE("run_remove_series: a refused qBit delete warns and continues; a refused "
          "remove reports",
          "[series_mutations]") {
    std::vector<std::string> log;
    ScriptedSonarr s(log);
    RecordingQbit q(log);
    q.fail_hashes = {"aaa"};
    s.hashes = std::vector<std::string>{"aaa"};
    s.fail_remove = true;
    const auto out = mb::run_remove_series(s, &q, 7, "Show");
    CHECK(log == std::vector<std::string>{"queue", "hashes:7",
                                          "qbit_delete:aaa:files",
                                          "remove:7:files"});
    CHECK_FALSE(out.removed);
    CHECK(out.toast == "Show: remove failed " + kDash + " remove refused");
}
