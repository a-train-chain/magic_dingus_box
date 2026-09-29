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
