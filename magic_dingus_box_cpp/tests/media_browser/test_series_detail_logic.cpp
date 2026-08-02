#include <catch2/catch_test_macros.hpp>

#include "media_browser/ui/series_detail_logic.h"

using namespace media_browser;
using namespace media_browser::ui;

namespace {
TmdbTvSeason tv_season(int n, int eps) {
    TmdbTvSeason s;
    s.season_number = n;
    s.episode_count = eps;
    return s;
}
Season sonarr_season(int n, bool mon, int eps, int files, int64_t bytes) {
    return Season{n, mon, eps, files, bytes};
}
}  // namespace

TEST_CASE("season rows: TMDB-only base, specials excluded, sorted", "[series_detail]") {
    std::vector<TmdbTvSeason> tmdb = {tv_season(2, 13), tv_season(0, 5), tv_season(1, 7)};
    auto rows = merge_season_rows(tmdb, nullptr, {});
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].season_number == 1);
    CHECK(rows[0].episode_count == 7);
    CHECK_FALSE(rows[0].monitored);
    CHECK(rows[0].episode_file_count == 0);
    CHECK(rows[0].state == SeasonState::None);
    CHECK(rows[1].season_number == 2);
}

TEST_CASE("season rows: Sonarr overlay wins on stats; unsettled keeps TMDB base",
          "[series_detail]") {
    std::vector<TmdbTvSeason> tmdb = {tv_season(1, 7), tv_season(2, 13)};

    SECTION("settled series overlays counts, files, monitored; extra season appended") {
        Series s;
        s.seasons = {sonarr_season(0, false, 5, 0, 0),
                     sonarr_season(1, true, 7, 7, 1),
                     sonarr_season(3, false, 10, 0, 0)};
        auto rows = merge_season_rows(tmdb, &s, {});
        REQUIRE(rows.size() == 3);  // 1, 2 (tmdb-only), 3 (sonarr-only); specials dropped
        CHECK(rows[0].monitored);
        CHECK(rows[0].episode_file_count == 7);
        CHECK(rows[0].state == SeasonState::Complete);
        CHECK(rows[1].season_number == 2);
        CHECK_FALSE(rows[1].monitored);
        CHECK(rows[2].season_number == 3);
        CHECK(rows[2].episode_count == 10);
    }

    SECTION("settled==false contract: empty sonarr seasons ⇒ rows are the TMDB base, never empty") {
        Series s;  // add_series with settled==false: seasons ALWAYS empty
        REQUIRE(s.seasons.empty());
        auto rows = merge_season_rows(tmdb, &s, {});
        REQUIRE(rows.size() == 2);  // the "0 seasons" hazard, pinned
        CHECK(rows[0].episode_count == 7);
    }
}

TEST_CASE("season state precedence: downloading > complete > partial > none",
          "[series_detail]") {
    CHECK(decide_season_state(10, 10, true) == SeasonState::Downloading);
    CHECK(decide_season_state(10, 10, false) == SeasonState::Complete);
    CHECK(decide_season_state(10, 3, false) == SeasonState::Partial);
    CHECK(decide_season_state(10, 0, false) == SeasonState::None);
    // Unknown episode_count with files present must not claim Complete.
    CHECK(decide_season_state(0, 3, false) == SeasonState::Partial);
    CHECK(decide_season_state(0, 0, false) == SeasonState::None);
}

TEST_CASE("next_unmonitored_season: first ascending unmonitored, nullopt when all monitored",
          "[series_detail]") {
    std::vector<SeasonRow> rows(3);
    rows[0].season_number = 1; rows[0].monitored = true;
    rows[1].season_number = 2; rows[1].monitored = false;
    rows[2].season_number = 3; rows[2].monitored = false;
    auto next = next_unmonitored_season(rows);
    REQUIRE(next.has_value());
    CHECK(*next == 2);
    rows[1].monitored = rows[2].monitored = true;
    CHECK_FALSE(next_unmonitored_season(rows).has_value());
    CHECK_FALSE(next_unmonitored_season({}).has_value());
}

TEST_CASE("estimate_remaining_bytes: missing episodes only, with defensive fallbacks",
          "[series_detail]") {
    std::vector<SeasonRow> rows(2);
    rows[0].episode_count = 10; rows[0].episode_file_count = 7;   // 3 missing
    rows[1].episode_count = 13; rows[1].episode_file_count = 0;   // 13 missing
    // 16 eps × 45 min × 70 MB/min × 1 MiB = 16*45*70 MiB
    const int64_t expected = 16LL * 45 * 70 * 1024 * 1024;
    CHECK(estimate_remaining_bytes(rows, 45, 70.0) == expected);
    // runtime<=0 falls back to 45; rate<=0 falls back to 70 — a zero estimate
    // would silently ALLOW a whole-series add past the blocking preflight.
    // The runtime<=0 path is also the PRE-ADD path: Sonarr has no record yet,
    // so 45 is an assumption and every label built from it says "(est)".
    CHECK(estimate_remaining_bytes(rows, 0, 70.0) == expected);
    CHECK(estimate_remaining_bytes(rows, 45, 0.0) == expected);
    // files >= count clamps to zero missing, never negative
    rows[0].episode_file_count = 12; rows[1].episode_file_count = 13;
    CHECK(estimate_remaining_bytes(rows, 45, 70.0) == 0);
}

TEST_CASE("pick_preferred_mb_per_min: 1080p family max, 720p fallback, 70 default",
          "[series_detail]") {
    std::vector<QualityDefinition> defs = {
        {4, "HDTV-720p", 40.0, 60.0},
        {9, "HDTV-1080p", 70.0, 100.0},
        {3, "WEBDL-1080p", 65.0, 100.0},
    };
    CHECK(pick_preferred_mb_per_min(defs) == 70.0);
    defs.erase(defs.begin() + 1);  // drop HDTV-1080p → WEBDL-1080p (65) wins
    CHECK(pick_preferred_mb_per_min(defs) == 65.0);
    defs.erase(defs.begin() + 1);  // only 720p left
    CHECK(pick_preferred_mb_per_min(defs) == 40.0);
    CHECK(pick_preferred_mb_per_min({}) == 70.0);
    // preferred<=0 rows (unlimited/null upstream) are skipped, not returned
    CHECK(pick_preferred_mb_per_min({{9, "HDTV-1080p", 0.0, 0.0}}) == 70.0);
}

TEST_CASE("whole_series_verdict: block at free minus 20 GiB floor; only a FAILED stat warns",
          "[series_detail]") {
    const int64_t GiB = 1024LL * 1024 * 1024;
    // free 100 GiB, floor 20 → budget 80
    CHECK(whole_series_verdict(80 * GiB, 100 * GiB) == DiskVerdict::Allow);
    CHECK(whole_series_verdict(80 * GiB + 1, 100 * GiB) == DiskVerdict::Block);
    // nullopt == the stat FAILED (nothing answered) — warn, never wedge.
    CHECK(whole_series_verdict(1, std::nullopt) == DiskVerdict::WarnOnly);
    // A READING of zero is the full-disk case, which is exactly what the
    // blocking preflight exists for. Failing open here would let the one
    // situation the feature was built to catch sail straight through.
    CHECK(whole_series_verdict(1, int64_t{0}) == DiskVerdict::Block);
}

TEST_CASE("series detail state resolver: precedence and copy", "[series_detail]") {
    SeriesDetailInputs in;
    in.tmdb_done = false;
    CHECK(decide_series_detail_state(in) == SeriesDetailState::Loading);
    in.tmdb_done = true; in.tmdb_ok = false;
    CHECK(decide_series_detail_state(in) == SeriesDetailState::TmdbError);
    in.tmdb_ok = true; in.sonarr_configured = false;
    CHECK(decide_series_detail_state(in) == SeriesDetailState::NotConfigured);
    in.sonarr_configured = true; in.sonarr_done = false;
    CHECK(decide_series_detail_state(in) == SeriesDetailState::Loading);
    in.sonarr_done = true; in.sonarr_ok = false;
    CHECK(decide_series_detail_state(in) == SeriesDetailState::SonarrUnreachable);
    in.sonarr_ok = true; in.in_library = false;
    CHECK(decide_series_detail_state(in) == SeriesDetailState::NotInLibrary);
    in.in_library = true;
    CHECK(decide_series_detail_state(in) == SeriesDetailState::InLibrary);

    // Copy: the NotConfigured string is the SAME literal browse ships —
    // "TV library not set up on this box" — so the two screens never disagree.
    CHECK(std::string(series_detail_state_message(SeriesDetailState::NotConfigured))
          == "TV library not set up on this box");
    CHECK(std::string(series_detail_state_message(SeriesDetailState::SonarrUnreachable))
          == "Sonarr service offline");
    CHECK(series_detail_state_message(SeriesDetailState::NotInLibrary) == nullptr);
    CHECK(series_detail_state_message(SeriesDetailState::InLibrary) == nullptr);
}
