#include <catch2/catch_test_macros.hpp>

#include "media_browser/ui/season_choice.h"

using namespace media_browser::ui;

namespace {
SeasonRow row(int n, SeasonState st, int eps = 10, int files = 0) {
    SeasonRow r;
    r.season_number = n;
    r.episode_count = eps;
    r.episode_file_count = files;
    r.state = st;
    return r;
}
watch_map watched_through(int last_season, int eps_per_season = 10) {
    watch_map w;
    for (int s = 1; s <= last_season; ++s)
        for (int e = 1; e <= eps_per_season; ++e)
            w[WatchKey{s, e}] = WatchRowLite{2700, 2700, true};
    return w;
}
}  // namespace

TEST_CASE("eligible: only seasons with nothing on disk and nothing in flight",
          "[season_choice]") {
    const std::vector<SeasonRow> rows = {
        row(1, SeasonState::Complete, 10, 10), row(2, SeasonState::Partial, 10, 3),
        row(3, SeasonState::Downloading), row(4, SeasonState::None),
        row(5, SeasonState::None)};
    CHECK(eligible_seasons(rows) == std::vector<int>{4, 5});
}

TEST_CASE("eligible: specials never appear", "[season_choice]") {
    const std::vector<SeasonRow> rows = {row(0, SeasonState::None), row(1, SeasonState::None)};
    CHECK(eligible_seasons(rows) == std::vector<int>{1});
}

TEST_CASE("suggest: emptied Game of Thrones watched through S4 -> 5", "[season_choice]") {
    std::vector<SeasonRow> rows;
    for (int s = 1; s <= 8; ++s) rows.push_back(row(s, SeasonState::None));
    CHECK(suggested_season(rows, watched_through(4)) == 5);
}

TEST_CASE("suggest: new show with no history -> 1", "[season_choice]") {
    const std::vector<SeasonRow> rows = {row(1, SeasonState::None), row(2, SeasonState::None),
                                         row(3, SeasonState::None)};
    CHECK(suggested_season(rows, {}) == 1);
}

TEST_CASE("suggest: past what is downloading -> next after the in-flight season",
          "[season_choice]") {
    const std::vector<SeasonRow> rows = {
        row(1, SeasonState::Complete, 10, 10), row(2, SeasonState::Downloading),
        row(3, SeasonState::None), row(4, SeasonState::None), row(5, SeasonState::None)};
    CHECK(suggested_season(rows, watched_through(1)) == 3);
}

TEST_CASE("suggest: nothing eligible after the frontier -> lowest eligible (gap)",
          "[season_choice]") {
    const std::vector<SeasonRow> rows = {
        row(1, SeasonState::None), row(2, SeasonState::Complete, 10, 10),
        row(3, SeasonState::Complete, 10, 10)};
    watch_map w = watched_through(3);
    CHECK(suggested_season(rows, w) == 1);
}

TEST_CASE("suggest: a resume position counts as watched progress", "[season_choice]") {
    const std::vector<SeasonRow> rows = {row(1, SeasonState::None), row(2, SeasonState::None),
                                         row(3, SeasonState::None)};
    watch_map w;
    w[WatchKey{2, 4}] = WatchRowLite{900, 2700, false};  // mid-episode in S2
    CHECK(suggested_season(rows, w) == 3);
}

TEST_CASE("suggest: everything on disk -> nullopt", "[season_choice]") {
    const std::vector<SeasonRow> rows = {row(1, SeasonState::Complete, 10, 10),
                                         row(2, SeasonState::Complete, 10, 10)};
    CHECK_FALSE(suggested_season(rows, {}).has_value());
}

TEST_CASE("suggest: watched specials never move the frontier", "[season_choice]") {
    const std::vector<SeasonRow> rows = {row(1, SeasonState::None), row(2, SeasonState::None)};
    watch_map w;
    w[WatchKey{0, 1}] = WatchRowLite{100, 100, true};
    CHECK(suggested_season(rows, w) == 1);
}

TEST_CASE("chooser: opens on the suggested season and steps through candidates, clamped",
          "[season_choice][chooser]") {
    SeasonChooser c;
    CHECK_FALSE(c.current().has_value());
    c.open({3, 5, 6, 8}, 5);
    REQUIRE(c.choosing);
    CHECK(c.current() == 5);
    c.step(+1); CHECK(c.current() == 6);
    c.step(+5); CHECK(c.current() == 8);   // clamped, no wrap
    c.step(-9); CHECK(c.current() == 3);
}

TEST_CASE("chooser: open snaps a non-candidate start to the nearest candidate",
          "[season_choice][chooser]") {
    SeasonChooser c;
    c.open({2, 7}, 5);
    CHECK(c.current() == 7);  // nearest at or above wins a tie-break upward
    SeasonChooser d;
    d.open({2, 7}, 9);
    CHECK(d.current() == 7);
}

TEST_CASE("chooser: confirm returns the season and goes idle; cancel returns nothing",
          "[season_choice][chooser]") {
    SeasonChooser c;
    c.open({4, 5}, 4);
    c.step(+1);
    CHECK(c.confirm() == 5);
    CHECK_FALSE(c.choosing);
    CHECK_FALSE(c.confirm().has_value());
    c.open({4, 5}, 4);
    c.cancel();
    CHECK_FALSE(c.choosing);
}

TEST_CASE("chooser: revalidate snaps or cancels when candidates change",
          "[season_choice][chooser]") {
    SeasonChooser c;
    c.open({4, 5, 6}, 5);
    c.revalidate({4, 6});          // S5 started downloading elsewhere
    CHECK(c.current() == 6);
    c.revalidate({});              // everything now on disk / in flight
    CHECK_FALSE(c.choosing);
}

TEST_CASE("chooser: an empty candidate list never opens", "[season_choice][chooser]") {
    SeasonChooser c;
    c.open({}, 1);
    CHECK_FALSE(c.choosing);
}

TEST_CASE("chooser label: arrows, season, GiB, (est) only when estimated",
          "[season_choice][chooser]") {
    const int64_t gib = 1024LL * 1024 * 1024;
    CHECK(chooser_label(5, 22 * gib, true) ==
          "\xE2\x80\xB9 Season 5 \xC2\xB7 ~22 GB (est) \xE2\x80\xBA");
    CHECK(chooser_label(12, 3 * gib, false) ==
          "\xE2\x80\xB9 Season 12 \xC2\xB7 ~3 GB \xE2\x80\xBA");
}

TEST_CASE("primary press: first press opens on the suggestion, second starts it",
          "[season_choice][press]") {
    // Emptied GoT, watched through S4: opens on 5, nothing started yet.
    std::vector<SeasonRow> rows;
    for (int s = 1; s <= 8; ++s) rows.push_back(row(s, SeasonState::None));
    SeasonChooser c;
    const auto p1 = press_primary(c, rows, watched_through(4));
    CHECK(p1.kind == PrimaryPress::Kind::Opened);
    REQUIRE(c.choosing);
    CHECK(c.current() == 5);
    const auto p2 = press_primary(c, rows, watched_through(4));
    CHECK(p2.kind == PrimaryPress::Kind::Start);
    CHECK(p2.season == 5);
    CHECK_FALSE(c.choosing);  // back to idle after starting
}

TEST_CASE("primary press: a rotated choice is what starts", "[season_choice][press]") {
    std::vector<SeasonRow> rows;
    for (int s = 1; s <= 8; ++s) rows.push_back(row(s, SeasonState::None));
    SeasonChooser c;
    REQUIRE(press_primary(c, rows, watched_through(4)).kind ==
            PrimaryPress::Kind::Opened);
    c.step(+1);  // 6
    const auto p = press_primary(c, rows, watched_through(4));
    CHECK(p.kind == PrimaryPress::Kind::Start);
    CHECK(p.season == 6);
}

TEST_CASE("primary press: new show opens on Season 1", "[season_choice][press]") {
    std::vector<SeasonRow> rows = {row(0, SeasonState::None),
                                   row(1, SeasonState::None),
                                   row(2, SeasonState::None)};
    SeasonChooser c;
    CHECK(press_primary(c, rows, {}).kind == PrimaryPress::Kind::Opened);
    CHECK(c.current() == 1);  // specials are never a candidate
}

TEST_CASE("primary press: nothing eligible falls through, chooser stays idle",
          "[season_choice][press]") {
    SeasonChooser c;
    CHECK(press_primary(c, {}, {}).kind == PrimaryPress::Kind::Fallthrough);
    CHECK_FALSE(c.choosing);
    const std::vector<SeasonRow> all_on_disk = {
        row(1, SeasonState::Complete, 10, 10)};
    CHECK(press_primary(c, all_on_disk, {}).kind ==
          PrimaryPress::Kind::Fallthrough);
    CHECK_FALSE(c.choosing);
}

TEST_CASE("primary press: a cancelled chooser re-opens on the next press",
          "[season_choice][press]") {
    std::vector<SeasonRow> rows = {row(1, SeasonState::None),
                                   row(2, SeasonState::None)};
    SeasonChooser c;
    REQUIRE(press_primary(c, rows, {}).kind == PrimaryPress::Kind::Opened);
    c.cancel();  // BTN4 / page flip
    CHECK(press_primary(c, rows, {}).kind == PrimaryPress::Kind::Opened);
    CHECK(c.choosing);
}
