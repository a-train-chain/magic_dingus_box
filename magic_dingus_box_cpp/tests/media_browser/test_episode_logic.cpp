#include <catch2/catch_test_macros.hpp>

#include "media_browser/ui/episode_logic.h"

using namespace media_browser;
using namespace media_browser::ui;

namespace {

// episode_logic templates on the episode type and may touch ONLY
// season_number / episode_number / has_file / title — this deliberately
// minimal fake is the compile-time proof (EpisodeInfo itself arrives in
// Task 2). `title` was APPENDED for Task 5's decide_end_overlay (its
// Countdown title_line reads ep.title); trailing position keeps every
// pre-existing three-field brace-init valid (title value-initializes).
struct FakeEp {
    int season_number;
    int episode_number;
    bool has_file;
    std::string title;
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
    SECTION("season 0 listed first is skipped: fresh watch-next starts at S1") {
        // Specials sort before season 1 (season asc), so a special with a
        // file is the FIRST has_file entry the loop sees — this is the only
        // section that fails if `if (e.season_number == 0) continue;` is
        // deleted (every other episode list puts season 0 after the pick).
        std::vector<FakeEp> specials_first = {ep(0, 1, true), ep(1, 1, true)};
        const FakeEp* n =
            next_up(specials_first, watch_map{}, static_cast<const FakeEp*>(nullptr));
        REQUIRE(n != nullptr);
        CHECK(n->season_number == 1);
        CHECK(n->episode_number == 1);
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

TEST_CASE("decide_end_overlay resolves the end-of-episode overlay", "[episode_logic]") {
    SECTION("next episode available -> Countdown with the pinned title line") {
        std::vector<FakeEp> eps = {
            FakeEp{1, 4, true, "The Shadow"},
            FakeEp{1, 5, true, "The Wolf and the Lion"}};
        std::vector<SeasonRow> rows = {season_row(1, SeasonState::Complete, true, 5)};
        auto m = decide_end_overlay(rows, eps, watch_map{}, eps[0], "Game of Thrones");
        CHECK(m.kind == EndOverlayKind::Countdown);
        // Pinned: "Next: S1E5 · The Wolf and the Lion" (U+00B7 middle dot).
        CHECK(m.title_line == "Next: S1E5 \xC2\xB7 The Wolf and the Lion");
        // The countdown line ("Starting in N…") derives from the screen's
        // frame timer, never from the model — body stays empty here.
        CHECK(m.body_line == "");
        CHECK(m.primary_label == "Play now");
        CHECK(m.has_primary);
        CHECK(m.next_index == 1);
        CHECK(m.card.kind == SeasonEndKind::NextEpisode);
    }
    SECTION("next_index is the vector position — fileless gaps are skipped") {
        std::vector<FakeEp> eps = {
            FakeEp{1, 4, true, "A"},
            FakeEp{1, 5, false, "B"},
            FakeEp{1, 6, true, "C"}};
        std::vector<SeasonRow> rows = {season_row(1, SeasonState::Complete, true, 5)};
        auto m = decide_end_overlay(rows, eps, watch_map{}, eps[0], "T");
        CHECK(m.kind == EndOverlayKind::Countdown);
        CHECK(m.next_index == 2);
        CHECK(m.title_line == "Next: S1E6 \xC2\xB7 C");
    }
    SECTION("season-crossing countdown uses the next episode's own coords") {
        std::vector<FakeEp> eps = {
            FakeEp{1, 10, true, "Finale"},
            FakeEp{2, 1, true, "Premiere"}};
        std::vector<SeasonRow> rows = {
            season_row(1, SeasonState::Complete, true, 10),
            season_row(2, SeasonState::Partial, true, 1)};
        auto m = decide_end_overlay(rows, eps, watch_map{}, eps[0], "T");
        CHECK(m.kind == EndOverlayKind::Countdown);
        CHECK(m.next_index == 1);
        CHECK(m.title_line == "Next: S2E1 \xC2\xB7 Premiere");
    }
    SECTION("OfferNextSeason -> Card with the upsell primary") {
        std::vector<FakeEp> eps = {FakeEp{1, 10, true, "Finale"}};
        std::vector<SeasonRow> rows = {
            season_row(1, SeasonState::Complete, true, 10),
            season_row(2, SeasonState::None, false, 0)};
        auto m = decide_end_overlay(rows, eps, watch_map{}, eps[0], "T");
        CHECK(m.kind == EndOverlayKind::Card);
        CHECK(m.title_line == "Season 1 finished");
        CHECK(m.body_line == "Start the Season 2 download?");
        CHECK(m.primary_label == "Start Season 2");
        CHECK(m.has_primary);
        CHECK(m.next_index == -1);
        CHECK(m.card.kind == SeasonEndKind::OfferNextSeason);
        CHECK(m.card.next_season == 2);
    }
    SECTION("Downloading -> Card, queue pointer body, no primary") {
        std::vector<FakeEp> eps = {FakeEp{1, 10, true, "Finale"}};
        std::vector<SeasonRow> rows = {
            season_row(1, SeasonState::Complete, true, 10),
            season_row(2, SeasonState::Downloading, false, 0)};
        auto m = decide_end_overlay(rows, eps, watch_map{}, eps[0], "T");
        CHECK(m.kind == EndOverlayKind::Card);
        CHECK(m.title_line == "Season 2 is on its way");
        CHECK(m.body_line == "Check the Queue for progress.");
        CHECK(m.primary_label == "Done");
        CHECK_FALSE(m.has_primary);
        CHECK(m.next_index == -1);
        CHECK(m.card.kind == SeasonEndKind::Downloading);
    }
    SECTION("SeriesDone -> Card, empty body, no primary") {
        std::vector<FakeEp> eps = {FakeEp{1, 10, true, "Finale"}};
        std::vector<SeasonRow> rows = {season_row(1, SeasonState::Complete, true, 10)};
        auto m = decide_end_overlay(rows, eps, watch_map{}, eps[0], "T");
        CHECK(m.kind == EndOverlayKind::Card);
        CHECK(m.title_line == "That's everything!");
        CHECK(m.body_line == "");
        CHECK(m.primary_label == "Done");
        CHECK_FALSE(m.has_primary);
        CHECK(m.next_index == -1);
        CHECK(m.card.kind == SeasonEndKind::SeriesDone);
    }
    SECTION("has_primary matrix pin: Countdown/Offer true, Downloading/Done false") {
        // The four kinds' has_primary values are asserted individually above;
        // this section pins the DEFAULT-constructed model as the screen's
        // idle state: kind None, no primary, no next index.
        EndOverlayModel idle;
        CHECK(idle.kind == EndOverlayKind::None);
        CHECK_FALSE(idle.has_primary);
        CHECK(idle.next_index == -1);
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
