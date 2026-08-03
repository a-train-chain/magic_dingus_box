// Unit tests for group_tv_queue() — the pure collapse of Sonarr's
// per-EPISODE queue rows into one entry per DOWNLOAD.
//
// The semantics under test are the ones that would silently produce a
// plausible-looking but wrong Queue screen:
//   - a season pack must render as ONE row, not N identical ones;
//   - sizes must be MAXed, because every row repeats the whole pack's size
//     (see the fixture assertion below — it reads Sonarr's real payload
//     shape, so a future parser change that switches to per-episode slices
//     breaks this test rather than the progress bar);
//   - a group must expose exactly ONE cancellable id, because one DELETE
//     kills the whole download server-side.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <unordered_map>
#include <vector>

#include "media_browser/ui/queue_groups.h"

namespace mbu = media_browser::ui;
using media_browser::SonarrQueueItem;

namespace {

// One episode row of a pack. Defaults mirror Sonarr's shape: every row of a
// download repeats the WHOLE pack's size/sizeleft.
SonarrQueueItem row(int id, const std::string& download_id,
                    const std::string& title, int season,
                    int64_t size = 12'884'901'888LL,
                    int64_t sizeleft = 6'442'450'944LL) {
    SonarrQueueItem q;
    q.id             = id;
    q.series_id      = 7;
    q.episode_id     = 5000 + id;
    q.season_number  = season;
    q.title          = title;
    q.size_bytes     = size;
    q.sizeleft_bytes = sizeleft;
    q.state          = "downloading";
    q.tracked_download_state = "downloading";
    q.download_id    = download_id;
    return q;
}

}  // namespace

TEST_CASE("a 10-row season pack collapses to one group", "[queue][tv][group]") {
    std::vector<SonarrQueueItem> rows;
    for (int ep = 1; ep <= 10; ++ep) {
        rows.push_back(row(100 + ep, "HASH-A",
                           "Game.of.Thrones.S01.1080p.BluRay.x264-GROUP", 1));
    }

    auto groups = mbu::group_tv_queue(rows);
    REQUIRE(groups.size() == 1);
    const auto& g = groups.front();
    CHECK(g.download_id == "HASH-A");
    CHECK(g.series_title == "Game.of.Thrones.S01.1080p.BluRay.x264-GROUP");
    CHECK(g.series_id == 7);           // first row's — the enrichment key
    CHECK(g.poster_url.empty());       // /queue has no images; enrichment fills
    CHECK(g.season_number == 1);
    CHECK(g.episode_count == 10);
    // MAX, not SUM: ten rows of a 12 GB pack are still a 12 GB pack.
    CHECK(g.size_bytes == 12'884'901'888LL);
    CHECK(g.sizeleft_bytes == 6'442'450'944LL);
    CHECK(g.status == "downloading");
    CHECK(g.tracked_download_state == "downloading");
    // The FIRST row's id, and the only one the UI may cancel.
    CHECK(g.first_queue_id == 101);
}

TEST_CASE("sizes take the max across rows, never the sum",
          "[queue][tv][group]") {
    // A mid-flight row whose size hasn't been filled in yet must not drag
    // the group's size down, and three full-size rows must not triple it.
    std::vector<SonarrQueueItem> rows{
        row(1, "HASH-A", "Pack", 3, /*size=*/0,             /*sizeleft=*/0),
        row(2, "HASH-A", "Pack", 3, /*size=*/8'000'000'000LL, /*sizeleft=*/2'000'000'000LL),
        row(3, "HASH-A", "Pack", 3, /*size=*/8'000'000'000LL, /*sizeleft=*/2'000'000'000LL),
    };

    auto groups = mbu::group_tv_queue(rows);
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].size_bytes == 8'000'000'000LL);
    CHECK(groups[0].sizeleft_bytes == 2'000'000'000LL);
    CHECK(groups[0].episode_count == 3);
}

TEST_CASE("distinct downloads stay distinct and keep input order",
          "[queue][tv][group]") {
    std::vector<SonarrQueueItem> rows{
        row(10, "HASH-A", "Series.A.S01", 1),
        row(11, "HASH-B", "Series.B.S04", 4),
        row(12, "HASH-A", "Series.A.S01", 1),
        row(13, "HASH-C", "Series.C.S02E05", 2),
        row(14, "HASH-B", "Series.B.S04", 4),
    };

    auto groups = mbu::group_tv_queue(rows);
    REQUIRE(groups.size() == 3);
    CHECK(groups[0].download_id == "HASH-A");
    CHECK(groups[0].episode_count == 2);
    CHECK(groups[0].first_queue_id == 10);
    CHECK(groups[1].download_id == "HASH-B");
    CHECK(groups[1].episode_count == 2);
    CHECK(groups[1].first_queue_id == 11);
    CHECK(groups[2].download_id == "HASH-C");
    CHECK(groups[2].episode_count == 1);
    CHECK(groups[2].season_number == 2);
}

TEST_CASE("an empty download_id falls back to the release title as the key",
          "[queue][tv][group]") {
    std::vector<SonarrQueueItem> rows{
        row(20, "", "Pending.Pack.S05", 5),
        row(21, "", "Pending.Pack.S05", 5),
        row(22, "", "Other.Pack.S01", 1),
    };

    auto groups = mbu::group_tv_queue(rows);
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].download_id.empty());
    CHECK(groups[0].series_title == "Pending.Pack.S05");
    CHECK(groups[0].episode_count == 2);
    CHECK(groups[0].first_queue_id == 20);
    CHECK(groups[1].series_title == "Other.Pack.S01");
    CHECK(groups[1].episode_count == 1);
}

TEST_CASE("rows with neither a hash nor a title are never merged",
          "[queue][tv][group]") {
    // Same rule cancel_ids_for_series() follows: an unkeyed row is taken
    // as-is. Collapsing them onto one empty key would hide real downloads
    // and skip real cancels.
    std::vector<SonarrQueueItem> rows{
        row(30, "", "", 1),
        row(31, "", "", 2),
    };

    auto groups = mbu::group_tv_queue(rows);
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].first_queue_id == 30);
    CHECK(groups[1].first_queue_id == 31);
}

TEST_CASE("a multi-season pack reports season -1", "[queue][tv][group]") {
    std::vector<SonarrQueueItem> rows{
        row(40, "HASH-Z", "Series.COMPLETE.1080p", 1),
        row(41, "HASH-Z", "Series.COMPLETE.1080p", 2),
        row(42, "HASH-Z", "Series.COMPLETE.1080p", 1),
    };

    auto groups = mbu::group_tv_queue(rows);
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].season_number == -1);
    CHECK(groups[0].episode_count == 3);
    // A season that reappears after the mismatch must not un-set the -1.
    CHECK(groups[0].first_queue_id == 40);
}

TEST_CASE("a single-season pack keeps its season number",
          "[queue][tv][group]") {
    std::vector<SonarrQueueItem> rows{
        row(50, "HASH-S", "Series.S07", 7),
        row(51, "HASH-S", "Series.S07", 7),
    };
    auto groups = mbu::group_tv_queue(rows);
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].season_number == 7);
}

TEST_CASE("an empty queue produces no groups", "[queue][tv][group]") {
    auto groups = mbu::group_tv_queue({});
    CHECK(groups.empty());
}

TEST_CASE("status and tracked state come from the first row",
          "[queue][tv][group]") {
    // Sonarr can report a pack mid-transition (row 1 imported, rows 2-3
    // still completing). The group takes row 1's, which is what the screen
    // reclassifies into "Importing…" / "warning".
    std::vector<SonarrQueueItem> rows{
        row(60, "HASH-I", "Series.S01", 1),
        row(61, "HASH-I", "Series.S01", 1),
    };
    rows[0].state = "completed";
    rows[0].tracked_download_state = "importPending";
    rows[1].state = "downloading";
    rows[1].tracked_download_state = "downloading";

    auto groups = mbu::group_tv_queue(rows);
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].status == "completed");
    CHECK(groups[0].tracked_download_state == "importPending");
}

// ---------------------------------------------------------------------------
// enrich_tv_groups() — the library cross-ref that replaces the release-name
// fallback with the clean series identity + poster. Kept separate from
// group_tv_queue() so the collapse stays pure over queue rows.
// ---------------------------------------------------------------------------

TEST_CASE("enrichment fills the poster and season-qualifies the title",
          "[queue][tv][group][enrich]") {
    std::vector<SonarrQueueItem> rows;
    for (int ep = 1; ep <= 10; ++ep) {
        rows.push_back(row(100 + ep, "HASH-A",
                           "Game.of.Thrones.S01.1080p.BluRay.x264-GROUP", 1));
    }
    auto groups = mbu::group_tv_queue(rows);
    REQUIRE(groups.size() == 1);

    std::unordered_map<int, mbu::SeriesRef> by_id;
    by_id.emplace(7, mbu::SeriesRef{"Game of Thrones",
                                    "https://img.example/got.jpg"});
    mbu::enrich_tv_groups(groups, by_id);

    CHECK(groups[0].series_title ==
          "Game of Thrones \xE2\x80\x94 Season 1");
    CHECK(groups[0].poster_url == "https://img.example/got.jpg");
    // Enrichment must not disturb the collapse facts.
    CHECK(groups[0].episode_count == 10);
    CHECK(groups[0].first_queue_id == 101);
}

TEST_CASE("a series the map does not know keeps the release-title fallback",
          "[queue][tv][group][enrich]") {
    auto groups = mbu::group_tv_queue(
        {row(1, "HASH-A", "Some.Show.S02.720p.WEB-GROUP", 2)});
    REQUIRE(groups.size() == 1);

    std::unordered_map<int, mbu::SeriesRef> by_id;
    by_id.emplace(999, mbu::SeriesRef{"Some Other Show", "https://x/y.jpg"});
    mbu::enrich_tv_groups(groups, by_id);

    // series_id 7 isn't in the map — nothing about the group changes.
    CHECK(groups[0].series_title == "Some.Show.S02.720p.WEB-GROUP");
    CHECK(groups[0].poster_url.empty());
}

TEST_CASE("a multi-season pack enriches to the bare series title",
          "[queue][tv][group][enrich]") {
    auto groups = mbu::group_tv_queue({
        row(40, "HASH-Z", "Series.COMPLETE.1080p", 1),
        row(41, "HASH-Z", "Series.COMPLETE.1080p", 2),
    });
    REQUIRE(groups.size() == 1);
    REQUIRE(groups[0].season_number == -1);

    std::unordered_map<int, mbu::SeriesRef> by_id;
    by_id.emplace(7, mbu::SeriesRef{"Breaking Bad", "https://img/bb.jpg"});
    mbu::enrich_tv_groups(groups, by_id);

    // No single season to name — the clean title stands alone.
    CHECK(groups[0].series_title == "Breaking Bad");
    CHECK(groups[0].poster_url == "https://img/bb.jpg");
}

TEST_CASE("an empty library map is a no-op", "[queue][tv][group][enrich]") {
    auto groups = mbu::group_tv_queue(
        {row(1, "HASH-A", "Show.S01.1080p", 1)});
    REQUIRE(groups.size() == 1);

    mbu::enrich_tv_groups(groups, {});

    CHECK(groups[0].series_title == "Show.S01.1080p");
    CHECK(groups[0].poster_url.empty());
    CHECK(groups[0].season_number == 1);
    CHECK(groups[0].episode_count == 1);
}

TEST_CASE("a library row with an empty title yields its poster only",
          "[queue][tv][group][enrich]") {
    // Defensive: a half-parsed library row must not blank the row identity —
    // the release name is still better than " — Season 2".
    auto groups = mbu::group_tv_queue(
        {row(1, "HASH-A", "Show.S02.1080p", 2)});
    REQUIRE(groups.size() == 1);

    std::unordered_map<int, mbu::SeriesRef> by_id;
    by_id.emplace(7, mbu::SeriesRef{"", "https://img/p.jpg"});
    mbu::enrich_tv_groups(groups, by_id);

    CHECK(groups[0].series_title == "Show.S02.1080p");
    CHECK(groups[0].poster_url == "https://img/p.jpg");
}
