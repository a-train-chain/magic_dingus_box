#include <catch2/catch_test_macros.hpp>

#include "media_browser/ui/episode_logic.h"

using namespace media_browser;
using namespace media_browser::ui;

namespace {

// episode_logic templates on the episode type and may touch ONLY
// season_number / episode_number / has_file — this deliberately minimal
// fake is the compile-time proof (EpisodeInfo itself arrives in Task 2).
struct FakeEp {
    int season_number;
    int episode_number;
    bool has_file;
};

FakeEp ep(int season, int episode, bool file) { return FakeEp{season, episode, file}; }

WatchRowLite watched_row() {
    WatchRowLite r;
    r.watched = true;
    return r;
}

SeasonRow season_row(int n, SeasonState st, bool monitored, int files) {
    SeasonRow r;
    r.season_number = n;
    r.state = st;
    r.monitored = monitored;
    r.episode_file_count = files;
    return r;
}

}  // namespace

TEST_CASE("watched/resumable thresholds", "[episode_logic]") {
    SECTION("watched: 92% of duration, zero/invalid duration never watched") {
        CHECK_FALSE(is_watched_position(55, 60));  // 0.9166 < 0.92
        CHECK(is_watched_position(56, 60));        // 0.9333 >= 0.92
        CHECK_FALSE(is_watched_position(0, 0));
        CHECK_FALSE(is_watched_position(120, 0));  // duration<=0 ⇒ false, not division blowup
    }
    SECTION("resumable: >=60s in and not past the watched threshold") {
        CHECK_FALSE(is_resumable_position(59, 3600));
        CHECK(is_resumable_position(61, 3600));
        CHECK_FALSE(is_resumable_position(3550, 3600));  // past watched ⇒ not resumable
    }
    SECTION("constants pinned") {
        CHECK(kWatchedFraction == 0.92);
        CHECK(kResumableMinSeconds == 60.0);
        CHECK(kNextUpCountdownSeconds == 8);
        CHECK(kCheckpointIntervalMs == 30000);
    }
}

TEST_CASE("WatchKey equality and hashing", "[episode_logic]") {
    watch_map m;
    m[WatchKey{1, 4}] = watched_row();
    CHECK(m.count(WatchKey{1, 4}) == 1);
    CHECK(m.count(WatchKey{1, 5}) == 0);
    CHECK(m.count(WatchKey{2, 4}) == 0);
    CHECK(WatchKey{1, 4} == WatchKey{1, 4});
    CHECK_FALSE(WatchKey{1, 4} == WatchKey{2, 4});
}

TEST_CASE("next_up from a current episode ignores the watch map", "[episode_logic]") {
    SECTION("gap in files: E4 finished, E5 has no file, E6 does -> E6") {
        std::vector<FakeEp> eps = {ep(1, 4, true), ep(1, 5, false), ep(1, 6, true)};
        const FakeEp* n = next_up(eps, watch_map{}, &eps[0]);
        REQUIRE(n != nullptr);
        CHECK(n->season_number == 1);
        CHECK(n->episode_number == 6);
    }
    SECTION("season crossing: S1E10 -> S2E1 with a file") {
        std::vector<FakeEp> eps = {ep(1, 10, true), ep(2, 1, true)};
        const FakeEp* n = next_up(eps, watch_map{}, &eps[0]);
        REQUIRE(n != nullptr);
        CHECK(n->season_number == 2);
        CHECK(n->episode_number == 1);
    }
    SECTION("nothing after -> nullptr") {
        std::vector<FakeEp> eps = {ep(1, 9, true), ep(1, 10, true)};
        CHECK(next_up(eps, watch_map{}, &eps[1]) == nullptr);
    }
    SECTION("season 0 entries are ignored even with files") {
        std::vector<FakeEp> eps = {ep(1, 10, true), ep(0, 1, true)};
        CHECK(next_up(eps, watch_map{}, &eps[0]) == nullptr);
    }
    SECTION("watched-overlap pin: a watched next episode still plays (sequential auto-play)") {
        // Mid-binge rewatch: S2E1 is already watched in the map, but the
        // current!=nullptr form must NOT skip ahead over it.
        std::vector<FakeEp> eps = {ep(1, 10, true), ep(2, 1, true)};
        watch_map watch;
        watch[WatchKey{2, 1}] = watched_row();
        const FakeEp* n = next_up(eps, watch, &eps[0]);
        REQUIRE(n != nullptr);
        CHECK(n->season_number == 2);
        CHECK(n->episode_number == 1);
    }
}

TEST_CASE("next_up with current==nullptr skips watched episodes", "[episode_logic]") {
    std::vector<FakeEp> eps = {ep(1, 1, true), ep(1, 2, false), ep(1, 3, true)};

    SECTION("first unwatched-with-file wins; watched-with-file is skipped") {
        watch_map watch;
        watch[WatchKey{1, 1}] = watched_row();
        const FakeEp* n = next_up(eps, watch, static_cast<const FakeEp*>(nullptr));
        REQUIRE(n != nullptr);
        CHECK(n->season_number == 1);
        CHECK(n->episode_number == 3);
    }
    SECTION("empty watch map -> the very first episode with a file") {
        const FakeEp* n = next_up(eps, watch_map{}, static_cast<const FakeEp*>(nullptr));
        REQUIRE(n != nullptr);
        CHECK(n->episode_number == 1);
    }
    SECTION("all watched -> nullptr") {
        watch_map watch;
        watch[WatchKey{1, 1}] = watched_row();
        watch[WatchKey{1, 3}] = watched_row();
        CHECK(next_up(eps, watch, static_cast<const FakeEp*>(nullptr)) == nullptr);
    }
}

TEST_CASE("season_end_card decision matrix", "[episode_logic]") {
    // Finished the last available episode of season 1 in every section below.
    std::vector<FakeEp> eps = {ep(1, 9, true), ep(1, 10, true)};
    const FakeEp& finished = eps[1];

    SECTION("next file exists -> NextEpisode") {
        std::vector<FakeEp> more = {ep(1, 10, true), ep(2, 1, true)};
        std::vector<SeasonRow> rows = {
            season_row(1, SeasonState::Complete, true, 10),
            season_row(2, SeasonState::Partial, true, 1)};
        auto card = season_end_card(rows, more, watch_map{}, more[0]);
        CHECK(card.kind == SeasonEndKind::NextEpisode);
    }
    SECTION("watched-overlap pin: watched S2E1 still yields NextEpisode") {
        std::vector<FakeEp> more = {ep(1, 10, true), ep(2, 1, true)};
        std::vector<SeasonRow> rows = {
            season_row(1, SeasonState::Complete, true, 10),
            season_row(2, SeasonState::Partial, true, 1)};
        watch_map watch;
        watch[WatchKey{2, 1}] = watched_row();
        auto card = season_end_card(rows, more, watch, more[0]);
        CHECK(card.kind == SeasonEndKind::NextEpisode);
    }
    SECTION("next season unmonitored -> OfferNextSeason with both season fields") {
        std::vector<SeasonRow> rows = {
            season_row(1, SeasonState::Complete, true, 10),
            season_row(2, SeasonState::None, false, 0)};
        auto card = season_end_card(rows, eps, watch_map{}, finished);
        CHECK(card.kind == SeasonEndKind::OfferNextSeason);
        CHECK(card.finished_season == 1);
        CHECK(card.next_season == 2);
    }
    SECTION("next season monitored with zero files -> Downloading") {
        std::vector<SeasonRow> rows = {
            season_row(1, SeasonState::Complete, true, 10),
            season_row(2, SeasonState::None, true, 0)};
        auto card = season_end_card(rows, eps, watch_map{}, finished);
        CHECK(card.kind == SeasonEndKind::Downloading);
        CHECK(card.finished_season == 1);
        CHECK(card.next_season == 2);
    }
    SECTION("next season in Downloading state -> Downloading") {
        std::vector<SeasonRow> rows = {
            season_row(1, SeasonState::Complete, true, 10),
            season_row(2, SeasonState::Downloading, false, 0)};
        auto card = season_end_card(rows, eps, watch_map{}, finished);
        CHECK(card.kind == SeasonEndKind::Downloading);
        CHECK(card.finished_season == 1);
        CHECK(card.next_season == 2);
    }
    SECTION("no next season row -> SeriesDone") {
        std::vector<SeasonRow> rows = {season_row(1, SeasonState::Complete, true, 10)};
        auto card = season_end_card(rows, eps, watch_map{}, finished);
        CHECK(card.kind == SeasonEndKind::SeriesDone);
        CHECK(card.finished_season == 1);
        CHECK(card.next_season == 0);  // no next row exists
    }
    SECTION("stale stats: next row claims files but next_up found none -> SeriesDone") {
        std::vector<SeasonRow> rows = {
            season_row(1, SeasonState::Complete, true, 10),
            season_row(2, SeasonState::Partial, true, 3)};
        auto card = season_end_card(rows, eps, watch_map{}, finished);
        CHECK(card.kind == SeasonEndKind::SeriesDone);
        CHECK(card.finished_season == 1);
        CHECK(card.next_season == 2);
    }
    SECTION("non-contiguous numbering: S1 finished, next row is S3") {
        std::vector<SeasonRow> rows = {
            season_row(1, SeasonState::Complete, true, 10),
            season_row(3, SeasonState::None, false, 0)};
        auto card = season_end_card(rows, eps, watch_map{}, finished);
        CHECK(card.kind == SeasonEndKind::OfferNextSeason);
        CHECK(card.finished_season == 1);
        CHECK(card.next_season == 3);
    }
}

TEST_CASE("season end copy is pinned", "[episode_logic]") {
    SECTION("OfferNextSeason title uses the FINISHED season") {
        SeasonEndCard card{SeasonEndKind::OfferNextSeason, 1, 2};
        CHECK(season_end_title(card, "Breaking Bad") == "Season 1 finished");
        CHECK(season_end_button_label(card) == "Start Season 2");
    }
    SECTION("Downloading title uses the NEXT season, not the finished one") {
        SeasonEndCard card{SeasonEndKind::Downloading, 1, 2};
        CHECK(season_end_title(card, "Breaking Bad") == "Season 2 is on its way");
        CHECK(season_end_button_label(card) == "Done");
    }
    SECTION("SeriesDone") {
        SeasonEndCard card{SeasonEndKind::SeriesDone, 1, 0};
        CHECK(season_end_title(card, "Breaking Bad") == "That's everything!");
        CHECK(season_end_button_label(card) == "Done");
    }
}

TEST_CASE("format_position_hms", "[episode_logic]") {
    CHECK(format_position_hms(7623) == "2:07:03");
    CHECK(format_position_hms(1394) == "23:14");
    CHECK(format_position_hms(125) == "2:05");
    CHECK(format_position_hms(143) == "2:23");
    CHECK(format_position_hms(0) == "0:00");
    CHECK(format_position_hms(3600) == "1:00:00");
    CHECK(format_position_hms(59) == "0:59");
}

TEST_CASE("WatchIdentity is MediaRef-keyed pure data", "[episode_logic]") {
    WatchIdentity id;
    id.ref = MediaRef{MediaKind::Tv, 1396};
    id.season = 2;
    id.episode = 5;
    CHECK(id.ref.kind == MediaKind::Tv);
    CHECK(id.ref.id == 1396);
    CHECK(id.season == 2);
    CHECK(id.episode == 5);
    // Defaults: season/episode 0 (a movie identity carries no episode coords).
    WatchIdentity movie;
    CHECK(movie.season == 0);
    CHECK(movie.episode == 0);
}
