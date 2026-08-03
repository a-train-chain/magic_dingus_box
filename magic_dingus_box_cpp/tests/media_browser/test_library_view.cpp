// Tests for media_browser/ui/library_view — the mixed movie+TV Library's
// entry-building, filter and sort decision logic (Phase 3 Task 7).
//
// Why this file exists at all: this logic's ancestor lived in
// library_screen.cpp, which names ::ui::Renderer, so it compiled only into the
// kiosk binary and could not even be built on the mac dev box. It therefore
// had zero coverage, and that is how the "recently added" cutoff bug shipped —
// a discarded gmtime_r return produced a well-formed-but-wrong
// "1900-01-00T00:00:00Z" that every real date compared greater than, silently
// turning the date filter into a pass-everything filter with no crash and
// nothing logged.
//
// library_view.h deliberately names no Renderer. The impure half (reading the
// clock, the latched spdlog::warn, reading WatchStore, mutating the screen's
// members) stays in LibraryScreen; everything asserted below is a pure
// function of its arguments.
//
// The load-bearing rules pinned here, in reviewer-adjudicated order:
//
//   * TV file/total counts use the SAME season-0-excluded basis as
//     WatchStore::tv_watched_counts: sum Season::episode_file_count /
//     episode_count over seasons with season_number > 0. The series-level
//     statistics.episodeFileCount INCLUDES imported S0 specials; using it as
//     the watched denominator would leave such a series permanently
//     un-watchable. The series-level stat drives the INCLUSION rule only.
//   * Unwatched is one line — `keep = !e.watched` — over the precomputed
//     entry.watched, for BOTH kinds. A 0-file downloading series has
//     watched == false, so it IS kept (deliberately superseding the spec's
//     `watched_episode_count < episodeFileCount` formula, which would drop
//     it).
//   * TMDB's movie and TV id spaces overlap completely (1396 is Breaking Bad
//     AND an unrelated film), so watched/downloading state must never leak
//     across kinds sharing an id.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "app/app_state.h"
#include "media_browser/media_ref.h"
#include "media_browser/radarr/radarr_types.h"
#include "media_browser/sonarr/sonarr_types.h"
#include "media_browser/ui/library_view.h"

namespace mbu = media_browser::ui;
using media_browser::MediaKind;
using media_browser::MediaRef;
using media_browser::Movie;
using media_browser::Season;
using media_browser::Series;
using Entry  = mbu::LibraryEntry;
using F      = ::app::AppState::DisplaySettings::MbLibraryFilter;
using S      = ::app::AppState::DisplaySettings::MbLibrarySort;

namespace {

Movie make_movie(int tmdb_id, const std::string& title, int year,
                 const std::string& added_at, bool has_file,
                 int64_t file_size_bytes,
                 const std::string& poster_url = "") {
    Movie m;
    m.tmdb_id          = tmdb_id;
    m.title            = title;
    m.year             = year;
    m.added_at         = added_at;
    m.has_file         = has_file;
    m.file_size_bytes  = file_size_bytes;
    m.poster_url       = poster_url;
    return m;
}

// One season, in the shape the Sonarr parser produces.
Season make_season(int number, int episode_count, int episode_file_count) {
    Season s;
    s.season_number      = number;
    s.monitored          = true;
    s.episode_count      = episode_count;
    s.episode_file_count = episode_file_count;
    return s;
}

// `series_level_file_count` is the statistics.episodeFileCount field — set
// independently from the season sums on purpose, so a test can prove which
// basis each rule reads (inclusion: series-level; counts: season sums S1+).
Series make_series(int tmdb_id, const std::string& title, int year,
                   const std::string& added_at,
                   std::vector<Season> seasons,
                   int series_level_file_count,
                   int64_t size_on_disk_bytes = 0,
                   const std::string& poster_url = "") {
    Series s;
    s.tmdb_id            = tmdb_id;
    s.title              = title;
    s.year               = year;
    s.added_at           = added_at;
    s.seasons            = std::move(seasons);
    s.episode_file_count = series_level_file_count;
    s.size_on_disk_bytes = size_on_disk_bytes;
    s.poster_url         = poster_url;
    return s;
}

std::vector<std::string> titles(const std::vector<const Entry*>& view) {
    std::vector<std::string> out;
    out.reserve(view.size());
    for (const Entry* e : view) out.push_back(e->title);
    return out;
}

std::vector<std::string> entry_titles(const std::vector<Entry>& entries) {
    std::vector<std::string> out;
    out.reserve(entries.size());
    for (const Entry& e : entries) out.push_back(e.title);
    return out;
}

// Convenience empties for the many cases that don't care about a given input.
const std::unordered_set<int> kNoWatchedMovies;
const std::unordered_map<int, int> kNoTvCounts;
const std::unordered_set<MediaRef> kNoDownloads;

// The cutoff shape LibraryScreen produces: utils::iso8601_utc output, fixed
// width, so lexicographic order equals chronological order.
const std::string kCutoff = "2026-06-29T00:00:00Z";

MediaRef tv_ref(int id)    { return MediaRef{MediaKind::Tv, id}; }
MediaRef movie_ref(int id) { return MediaRef{MediaKind::Movie, id}; }

}  // namespace

// =====================================================================
// build_library_entries — movie entries
// =====================================================================

TEST_CASE("A movie entry carries the Movie's fields and a Movie-kind ref",
          "[library_view][entries][movie]") {
    std::vector<Movie> movies{
        make_movie(603, "The Matrix", 1999, "2026-07-01T00:00:00Z", true,
                   4'000'000'000LL, "http://img/matrix.jpg"),
    };
    const std::vector<Series> no_tv;

    const auto entries = mbu::build_library_entries(
        movies, no_tv, kNoWatchedMovies, kNoTvCounts, kNoDownloads);

    REQUIRE(entries.size() == 1);
    const Entry& e = entries[0];
    REQUIRE(e.ref == movie_ref(603));
    REQUIRE(e.title == "The Matrix");
    REQUIRE(e.year == 1999);
    REQUIRE(e.poster_url == "http://img/matrix.jpg");
    REQUIRE(e.added_at == "2026-07-01T00:00:00Z");
    REQUIRE(e.file_count == 1);
    REQUIRE(e.total_count == 1);
    REQUIRE_FALSE(e.downloading);
    REQUIRE_FALSE(e.watched);
    REQUIRE(e.movie == &movies[0]);
    REQUIRE(e.series == nullptr);
}

TEST_CASE("A file-less movie is still an entry, with file_count 0",
          "[library_view][entries][movie]") {
    // Movies have no inclusion rule — Radarr's library IS the movie list.
    std::vector<Movie> movies{
        make_movie(10, "Awaiting", 2026, kCutoff, false, 0),
    };
    const std::vector<Series> no_tv;

    const auto entries = mbu::build_library_entries(
        movies, no_tv, kNoWatchedMovies, kNoTvCounts, kNoDownloads);

    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].file_count == 0);
    REQUIRE(entries[0].total_count == 1);
    // No file, so it cannot have been watched.
    REQUIRE_FALSE(entries[0].watched);
}

TEST_CASE("Movie watched comes from watched_movie_ids membership",
          "[library_view][entries][movie][watched]") {
    std::vector<Movie> movies{
        make_movie(1, "Seen", 2020, kCutoff, true, 100),
        make_movie(2, "Unseen", 2021, kCutoff, true, 100),
    };
    const std::vector<Series> no_tv;
    const std::unordered_set<int> watched{1};

    const auto entries = mbu::build_library_entries(
        movies, no_tv, watched, kNoTvCounts, kNoDownloads);

    REQUIRE(entries.size() == 2);
    REQUIRE(entries[0].watched);
    REQUIRE_FALSE(entries[1].watched);
}

TEST_CASE("Movie downloading comes from a Movie-kind ref in downloading_refs",
          "[library_view][entries][movie][downloading]") {
    std::vector<Movie> movies{
        make_movie(7, "Fetching", 2026, kCutoff, false, 0),
        make_movie(8, "Idle", 2026, kCutoff, false, 0),
    };
    const std::vector<Series> no_tv;
    const std::unordered_set<MediaRef> downloading{movie_ref(7)};

    const auto entries = mbu::build_library_entries(
        movies, no_tv, kNoWatchedMovies, kNoTvCounts, downloading);

    REQUIRE(entries.size() == 2);
    REQUIRE(entries[0].downloading);
    REQUIRE_FALSE(entries[1].downloading);
}

// =====================================================================
// build_library_entries — TV inclusion rule
// =====================================================================

TEST_CASE("A 0-file, non-downloading series produces no entry",
          "[library_view][entries][tv][inclusion]") {
    // The spec's inclusion rule: a series appears once it has any file OR an
    // active download. A bare "added to Sonarr, nothing grabbed yet" series
    // stays off the Library grid (Browse/SeriesDetail is where it lives).
    const std::vector<Movie> no_movies;
    std::vector<Series> tv{
        make_series(100, "Nothing Yet", 2026, kCutoff,
                    {make_season(1, 10, 0)}, /*series_level_file_count=*/0),
    };

    const auto entries = mbu::build_library_entries(
        no_movies, tv, kNoWatchedMovies, kNoTvCounts, kNoDownloads);

    REQUIRE(entries.empty());
}

TEST_CASE("A 0-file series WITH an active download is present, downloading",
          "[library_view][entries][tv][inclusion]") {
    // A freshly-started season download has episodeFileCount == 0 until the
    // first import lands. Without the downloading_refs escape hatch it would
    // never appear, and no TV tile could ever show DOWNLOADING.
    const std::vector<Movie> no_movies;
    std::vector<Series> tv{
        make_series(100, "Grabbing S1", 2026, kCutoff,
                    {make_season(1, 10, 0)}, /*series_level_file_count=*/0),
    };
    const std::unordered_set<MediaRef> downloading{tv_ref(100)};

    const auto entries = mbu::build_library_entries(
        no_movies, tv, kNoWatchedMovies, kNoTvCounts, downloading);

    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].ref == tv_ref(100));
    REQUIRE(entries[0].downloading);
    REQUIRE(entries[0].file_count == 0);
    // 0 files can never read as watched — the file_count > 0 guard.
    REQUIRE_FALSE(entries[0].watched);
}

TEST_CASE("Inclusion reads the SERIES-LEVEL stat, not the season sums",
          "[library_view][entries][tv][inclusion]") {
    // A series whose only files are S0 specials: season-sum basis (S1+) says
    // 0 files, but the series-level statistics.episodeFileCount says 2. The
    // reviewer adjudication splits the two: the series-level stat may drive
    // INCLUSION only. So this series IS on the grid (the user has files for
    // it; hiding it would orphan real disk content) even though its
    // s0-excluded file_count is 0.
    const std::vector<Movie> no_movies;
    std::vector<Series> tv{
        make_series(200, "Specials Only", 2026, kCutoff,
                    {make_season(0, 5, 2), make_season(1, 8, 0)},
                    /*series_level_file_count=*/2),
    };

    const auto entries = mbu::build_library_entries(
        no_movies, tv, kNoWatchedMovies, kNoTvCounts, kNoDownloads);

    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].file_count == 0);   // S0 excluded from the counts
    REQUIRE(entries[0].total_count == 8);  // S1's episodes only
    REQUIRE_FALSE(entries[0].watched);     // 0 s0-excluded files -> unwatched
}

// =====================================================================
// build_library_entries — TV counts basis (season-0 exclusion)
// =====================================================================

TEST_CASE("TV file/total counts sum S1+ seasons and ignore the series stat",
          "[library_view][entries][tv][counts]") {
    // S0 has 2 files / 3 episodes and must not count. The series-level stat
    // is set to a nonsense 99 to prove the counts never read it.
    const std::vector<Movie> no_movies;
    std::vector<Series> tv{
        make_series(300, "Counted", 2020, kCutoff,
                    {make_season(0, 3, 2),
                     make_season(1, 10, 5),
                     make_season(2, 8, 3)},
                    /*series_level_file_count=*/99),
    };

    const auto entries = mbu::build_library_entries(
        no_movies, tv, kNoWatchedMovies, kNoTvCounts, kNoDownloads);

    REQUIRE(entries.size() == 1);
    const Entry& e = entries[0];
    REQUIRE(e.ref == tv_ref(300));
    REQUIRE(e.file_count == 8);    // 5 + 3, S0's 2 excluded
    REQUIRE(e.total_count == 18);  // 10 + 8, S0's 3 excluded
    REQUIRE(e.movie == nullptr);
    REQUIRE(e.series == &tv[0]);
}

TEST_CASE("A TV entry carries the Series' display fields",
          "[library_view][entries][tv]") {
    const std::vector<Movie> no_movies;
    std::vector<Series> tv{
        make_series(1396, "Breaking Bad", 2008, "2026-05-05T00:00:00Z",
                    {make_season(1, 7, 7)}, 7, 30'000'000'000LL,
                    "http://img/bb.jpg"),
    };

    const auto entries = mbu::build_library_entries(
        no_movies, tv, kNoWatchedMovies, kNoTvCounts, kNoDownloads);

    REQUIRE(entries.size() == 1);
    const Entry& e = entries[0];
    REQUIRE(e.title == "Breaking Bad");
    REQUIRE(e.year == 2008);
    REQUIRE(e.poster_url == "http://img/bb.jpg");
    REQUIRE(e.added_at == "2026-05-05T00:00:00Z");
}

TEST_CASE("Empty poster_url stays empty on both kinds (tint fallback input)",
          "[library_view][entries][poster]") {
    // The renderer treats an empty URL as "draw the tint placeholder". The
    // entry builder must pass emptiness through rather than inventing a
    // value — pinned so a future "default poster" idea is a conscious change.
    std::vector<Movie> movies{
        make_movie(1, "No Art Movie", 2020, kCutoff, true, 1),
    };
    std::vector<Series> tv{
        make_series(2, "No Art Show", 2021, kCutoff,
                    {make_season(1, 3, 3)}, 3),
    };

    const auto entries = mbu::build_library_entries(
        movies, tv, kNoWatchedMovies, kNoTvCounts, kNoDownloads);

    REQUIRE(entries.size() == 2);
    REQUIRE(entries[0].poster_url.empty());
    REQUIRE(entries[1].poster_url.empty());
}

// =====================================================================
// build_library_entries — TV watched (the same-basis rule + SPECIALS case)
// =====================================================================

TEST_CASE("TV watched: watched_count >= s0-excluded file_count, files > 0",
          "[library_view][entries][tv][watched]") {
    const std::vector<Movie> no_movies;
    std::vector<Series> tv{
        make_series(400, "Fully Watched", 2020, kCutoff,
                    {make_season(1, 10, 8)}, 8),
        make_series(401, "Partially Watched", 2020, kCutoff,
                    {make_season(1, 10, 8)}, 8),
        make_series(402, "Untouched", 2020, kCutoff,
                    {make_season(1, 10, 8)}, 8),
    };
    const std::unordered_map<int, int> counts{{400, 8}, {401, 3}};

    const auto entries = mbu::build_library_entries(
        no_movies, tv, kNoWatchedMovies, counts, kNoDownloads);

    REQUIRE(entries.size() == 3);
    REQUIRE(entries[0].watched);        // 8 >= 8
    REQUIRE_FALSE(entries[1].watched);  // 3 <  8
    REQUIRE_FALSE(entries[2].watched);  // no row at all -> 0 < 8
}

TEST_CASE("SPECIALS: imported S0 files cannot make a series un-watchable",
          "[library_view][entries][tv][watched][specials]") {
    // THE case the same-basis rule exists for. 2 S0 specials are imported, so
    // the series-level episodeFileCount is 10 — but WatchStore's
    // tv_watched_counts excludes S0, so the count can never exceed 8. Against
    // the series-level denominator this series would sit at 8/10 forever and
    // never leave the Unwatched filter. Against the season-sum basis it is
    // 8/8: watched.
    const std::vector<Movie> no_movies;
    std::vector<Series> tv{
        make_series(500, "Has Specials", 2015, kCutoff,
                    {make_season(0, 4, 2), make_season(1, 8, 8)},
                    /*series_level_file_count=*/10),
    };
    const std::unordered_map<int, int> counts{{500, 8}};  // all of S1 watched

    const auto entries = mbu::build_library_entries(
        no_movies, tv, kNoWatchedMovies, counts, kNoDownloads);

    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].file_count == 8);
    REQUIRE(entries[0].watched);
}

TEST_CASE("Watched-count overshoot (re-sourced series) still reads watched",
          "[library_view][entries][tv][watched]") {
    // Task 3's accepted v1 note: watch rows never GC, so after episodes are
    // deleted/re-sourced in Sonarr the stored count can EXCEED the current
    // file count. >= (not ==) keeps that state watched instead of flapping.
    const std::vector<Movie> no_movies;
    std::vector<Series> tv{
        make_series(600, "Re-sourced", 2018, kCutoff,
                    {make_season(1, 10, 4)}, 4),
    };
    const std::unordered_map<int, int> counts{{600, 9}};

    const auto entries = mbu::build_library_entries(
        no_movies, tv, kNoWatchedMovies, counts, kNoDownloads);

    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].watched);
}

// =====================================================================
// The movie/TV id-collision pin (tmdb 1396 is Breaking Bad AND a film)
// =====================================================================

TEST_CASE("Movie 1396 watched does not mark TV 1396 watched, and vice versa",
          "[library_view][entries][collision]") {
    std::vector<Movie> movies{
        make_movie(1396, "Colliding Film", 2001, kCutoff, true, 100),
    };
    std::vector<Series> tv{
        make_series(1396, "Breaking Bad", 2008, kCutoff,
                    {make_season(1, 7, 7)}, 7),
    };

    SECTION("movie watched, tv untouched") {
        const std::unordered_set<int> watched_movies{1396};
        const auto entries = mbu::build_library_entries(
            movies, tv, watched_movies, kNoTvCounts, kNoDownloads);
        REQUIRE(entries.size() == 2);
        // Movies precede series in the built vector.
        REQUIRE(entries[0].ref == movie_ref(1396));
        REQUIRE(entries[0].watched);
        REQUIRE(entries[1].ref == tv_ref(1396));
        REQUIRE_FALSE(entries[1].watched);
    }

    SECTION("tv watched, movie untouched") {
        const std::unordered_map<int, int> counts{{1396, 7}};
        const auto entries = mbu::build_library_entries(
            movies, tv, kNoWatchedMovies, counts, kNoDownloads);
        REQUIRE(entries.size() == 2);
        REQUIRE_FALSE(entries[0].watched);
        REQUIRE(entries[1].watched);
    }

    SECTION("a Tv-kind downloading ref does not light up the movie") {
        const std::unordered_set<MediaRef> downloading{tv_ref(1396)};
        const auto entries = mbu::build_library_entries(
            movies, tv, kNoWatchedMovies, kNoTvCounts, downloading);
        REQUIRE(entries.size() == 2);
        REQUIRE_FALSE(entries[0].downloading);
        REQUIRE(entries[1].downloading);
    }
}

// =====================================================================
// library_row_kept — Unwatched (real now: keep = !e.watched, both kinds)
// =====================================================================

TEST_CASE("Unwatched keeps exactly the entries with watched == false",
          "[library_view][filter][unwatched]") {
    std::vector<Movie> movies{
        make_movie(1, "Seen Film", 2020, kCutoff, true, 100),
        make_movie(2, "Unseen Film", 2021, kCutoff, true, 100),
        make_movie(3, "No File Film", 2022, kCutoff, false, 0),
    };
    std::vector<Series> tv{
        make_series(4, "Seen Show", 2019, kCutoff,
                    {make_season(1, 6, 6)}, 6),
        make_series(5, "Half Show", 2018, kCutoff,
                    {make_season(1, 6, 6)}, 6),
        make_series(6, "Fresh Grab", 2026, kCutoff,
                    {make_season(1, 10, 0)}, 0),
    };
    const std::unordered_set<int> watched_movies{1};
    const std::unordered_map<int, int> counts{{4, 6}, {5, 3}};
    const std::unordered_set<MediaRef> downloading{tv_ref(6)};

    const auto entries = mbu::build_library_entries(
        movies, tv, watched_movies, counts, downloading);
    REQUIRE(entries.size() == 6);

    // Row-level: one line, both kinds, cutoff args irrelevant.
    REQUIRE_FALSE(mbu::library_row_kept(F::Unwatched, entries[0], kCutoff, true));
    REQUIRE(mbu::library_row_kept(F::Unwatched, entries[1], kCutoff, true));
    REQUIRE(mbu::library_row_kept(F::Unwatched, entries[2], kCutoff, true));
    REQUIRE_FALSE(mbu::library_row_kept(F::Unwatched, entries[3], kCutoff, true));
    REQUIRE(mbu::library_row_kept(F::Unwatched, entries[4], kCutoff, true));
    // The 0-file downloading series: watched == false, so it IS KEPT. This
    // deliberately supersedes the spec's `watched_episode_count <
    // episodeFileCount` formula, which reads 0 < 0 and would drop the very
    // tile whose DOWNLOADING badge is the point.
    REQUIRE(mbu::library_row_kept(F::Unwatched, entries[5], kCutoff, true));

    const auto view = mbu::build_library_view(entries, F::Unwatched, S::Title,
                                              kCutoff, true);
    REQUIRE(titles(view) == std::vector<std::string>{
        "Fresh Grab", "Half Show", "No File Film", "Unseen Film"});
}

// =====================================================================
// library_row_kept — MissingFiles (file_count < total_count, both kinds)
// =====================================================================

TEST_CASE("MissingFiles keeps entries whose file_count < total_count",
          "[library_view][filter][missing]") {
    std::vector<Movie> movies{
        make_movie(1, "Complete Film", 2020, kCutoff, true, 100),
        make_movie(2, "Absent Film", 2021, kCutoff, false, 0),
    };
    std::vector<Series> tv{
        make_series(3, "Complete Show", 2019, kCutoff,
                    {make_season(1, 6, 6), make_season(2, 4, 4)}, 10),
        make_series(4, "Gappy Show", 2018, kCutoff,
                    {make_season(1, 6, 6), make_season(2, 4, 1)}, 7),
    };

    const auto entries = mbu::build_library_entries(
        movies, tv, kNoWatchedMovies, kNoTvCounts, kNoDownloads);
    REQUIRE(entries.size() == 4);

    REQUIRE_FALSE(mbu::library_row_kept(F::MissingFiles, entries[0], kCutoff, true));
    REQUIRE(mbu::library_row_kept(F::MissingFiles, entries[1], kCutoff, true));
    REQUIRE_FALSE(mbu::library_row_kept(F::MissingFiles, entries[2], kCutoff, true));
    REQUIRE(mbu::library_row_kept(F::MissingFiles, entries[3], kCutoff, true));

    // The cutoff arguments are irrelevant to this filter and must not leak
    // into it — same answers with the cutoff marked invalid.
    REQUIRE(mbu::library_row_kept(F::MissingFiles, entries[1], "", false));
    REQUIRE_FALSE(mbu::library_row_kept(F::MissingFiles, entries[0], "", false));

    const auto view = mbu::build_library_view(entries, F::MissingFiles,
                                              S::Title, kCutoff, true);
    REQUIRE(titles(view) == std::vector<std::string>{"Absent Film",
                                                     "Gappy Show"});
}

TEST_CASE("MissingFiles counts S0 gaps as no gap at all",
          "[library_view][filter][missing][specials]") {
    // Missing SPECIALS are not missing files: both counts are s0-excluded,
    // so a series complete in S1+ but with S0 holes must NOT show under
    // MissingFiles.
    const std::vector<Movie> no_movies;
    std::vector<Series> tv{
        make_series(9, "S0 Holes", 2017, kCutoff,
                    {make_season(0, 6, 1), make_season(1, 8, 8)}, 9),
    };

    const auto entries = mbu::build_library_entries(
        no_movies, tv, kNoWatchedMovies, kNoTvCounts, kNoDownloads);
    REQUIRE(entries.size() == 1);
    REQUIRE_FALSE(mbu::library_row_kept(F::MissingFiles, entries[0],
                                        kCutoff, true));
}

// =====================================================================
// library_row_kept — RecentlyAdded on entry.added_at (both kinds)
// =====================================================================

TEST_CASE("RecentlyAdded with an invalid cutoff keeps every entry",
          "[library_view][filter][recent_fallback]") {
    // The show-all fallback branch. When the cutoff cannot be formatted the
    // filter degrades to show-all on purpose: an empty grid reads as "your
    // library is empty", which is a scarier failure on an appliance than an
    // unfiltered one.
    std::vector<Movie> movies{
        make_movie(1, "Nosferatu", 1922, "1922-03-04T00:00:00Z", true, 100),
        make_movie(2, "Undated", 2026, "", true, 100),
    };
    std::vector<Series> tv{
        make_series(3, "Old Show", 1999, "1999-01-01T00:00:00Z",
                    {make_season(1, 5, 5)}, 5),
    };

    const auto entries = mbu::build_library_entries(
        movies, tv, kNoWatchedMovies, kNoTvCounts, kNoDownloads);
    REQUIRE(entries.size() == 3);

    SECTION("empty cutoff string (what iso8601_utc returns on failure)") {
        for (const Entry& e : entries) {
            REQUIRE(mbu::library_row_kept(F::RecentlyAdded, e, "", false));
        }
    }

    SECTION("the valid flag is authoritative, not the string's emptiness") {
        // A non-empty cutoff paired with valid == false must STILL keep
        // everything. Pins the `!recent_cutoff_valid ||` short-circuit: a
        // version that dropped the flag and relied on "" comparing less than
        // every date would pass the section above by accident and fail here.
        for (const Entry& e : entries) {
            REQUIRE(mbu::library_row_kept(F::RecentlyAdded, e, kCutoff, false));
        }
    }
}

TEST_CASE("RecentlyAdded with a valid cutoff compares >= against added_at",
          "[library_view][filter][recent]") {
    std::vector<Movie> movies{
        // Strictly after the cutoff.
        make_movie(1, "After", 2026, "2026-07-01T12:00:00Z", true, 100),
        // Exactly ON the cutoff — the >= boundary. A `>` would drop this.
        make_movie(2, "Equal", 2026, kCutoff, true, 100),
        // One second before the cutoff.
        make_movie(3, "Before", 2026, "2026-06-28T23:59:59Z", true, 100),
        // "" sorts below every real date and must be DROPPED rather than
        // silently treated as ancient-but-present.
        make_movie(4, "Undated", 2026, "", true, 100),
    };
    std::vector<Series> tv{
        // TV rides the same entry.added_at rule — a recent series is kept…
        make_series(5, "New Show", 2026, "2026-07-20T00:00:00Z",
                    {make_season(1, 5, 5)}, 5),
        // …and an old one is dropped.
        make_series(6, "Old Show", 2019, "2019-01-01T00:00:00Z",
                    {make_season(1, 5, 5)}, 5),
    };

    const auto entries = mbu::build_library_entries(
        movies, tv, kNoWatchedMovies, kNoTvCounts, kNoDownloads);
    REQUIRE(entries.size() == 6);

    REQUIRE(mbu::library_row_kept(F::RecentlyAdded, entries[0], kCutoff, true));
    REQUIRE(mbu::library_row_kept(F::RecentlyAdded, entries[1], kCutoff, true));
    REQUIRE_FALSE(mbu::library_row_kept(F::RecentlyAdded, entries[2], kCutoff, true));
    REQUIRE_FALSE(mbu::library_row_kept(F::RecentlyAdded, entries[3], kCutoff, true));
    REQUIRE(mbu::library_row_kept(F::RecentlyAdded, entries[4], kCutoff, true));
    REQUIRE_FALSE(mbu::library_row_kept(F::RecentlyAdded, entries[5], kCutoff, true));

    const auto view = mbu::build_library_view(entries, F::RecentlyAdded,
                                              S::Title, kCutoff, true);
    REQUIRE(titles(view) == std::vector<std::string>{"After", "Equal",
                                                     "New Show"});
}

// =====================================================================
// library_row_kept — All keeps everything
// =====================================================================

TEST_CASE("All keeps an entry that fails every other filter",
          "[library_view][filter][passthrough]") {
    // Watched + complete + undated fails Unwatched, MissingFiles and
    // RecentlyAdded (valid cutoff). All must still keep it.
    std::vector<Movie> movies{
        make_movie(1, "Fails Everything Else", 1980, "", true, 0),
    };
    const std::vector<Series> no_tv;
    const std::unordered_set<int> watched_movies{1};

    const auto entries = mbu::build_library_entries(
        movies, no_tv, watched_movies, kNoTvCounts, kNoDownloads);
    REQUIRE(entries.size() == 1);

    REQUIRE_FALSE(mbu::library_row_kept(F::Unwatched, entries[0], kCutoff, true));
    REQUIRE_FALSE(mbu::library_row_kept(F::MissingFiles, entries[0], kCutoff, true));
    REQUIRE_FALSE(mbu::library_row_kept(F::RecentlyAdded, entries[0], kCutoff, true));
    REQUIRE(mbu::library_row_kept(F::All, entries[0], kCutoff, true));
}

// =====================================================================
// Sorts (over mixed-kind entries)
// =====================================================================

namespace {

// A ready-made mixed library for the sort cases. Kept as a fixture builder
// (not a global) because entries hold pointers into these vectors — each test
// owns its copies for the entries' lifetime.
struct MixedFixture {
    std::vector<Movie> movies;
    std::vector<Series> tv;
    std::vector<Entry> entries;

    MixedFixture() {
        movies = {
            make_movie(1, "Middle Film", 2005, "2026-03-15T00:00:00Z", true,
                       2LL * 1024 * 1024 * 1024),
            make_movie(2, "old film", 1994, "2019-12-31T23:59:59Z", true,
                       700LL * 1024 * 1024),
        };
        tv = {
            make_series(3, "Newest Show", 2024, "2026-07-29T08:00:00Z",
                        {make_season(1, 8, 8)}, 8, 9LL * 1024 * 1024 * 1024),
            make_series(4, "Ancient Show", 1994, "2018-01-01T00:00:00Z",
                        {make_season(1, 6, 6)}, 6, 1LL * 1024 * 1024 * 1024),
        };
        entries = mbu::build_library_entries(
            movies, tv, kNoWatchedMovies, kNoTvCounts, kNoDownloads);
    }
};

}  // namespace

TEST_CASE("Recent sorts by added_at descending across kinds",
          "[library_view][sort]") {
    MixedFixture fx;
    const auto view = mbu::build_library_view(fx.entries, F::All, S::Recent,
                                              kCutoff, true);
    REQUIRE(titles(view) == std::vector<std::string>{
        "Newest Show", "Middle Film", "old film", "Ancient Show"});
}

TEST_CASE("Recent puts an empty added_at last", "[library_view][sort]") {
    // "" is less than every non-empty string, and the comparator is `>`, so
    // an undated entry sinks to the bottom rather than floating to the top.
    std::vector<Movie> movies{
        make_movie(1, "Undated", 2026, "", true, 1),
        make_movie(2, "Dated", 2026, "2001-01-01T00:00:00Z", true, 2),
    };
    const std::vector<Series> no_tv;
    const auto entries = mbu::build_library_entries(
        movies, no_tv, kNoWatchedMovies, kNoTvCounts, kNoDownloads);

    const auto view = mbu::build_library_view(entries, F::All, S::Recent,
                                              kCutoff, true);
    REQUIRE(titles(view) == std::vector<std::string>{"Dated", "Undated"});
}

TEST_CASE("Title sorts case-INSENSITIVELY across kinds",
          "[library_view][sort][title]") {
    // A naive `a->title < b->title` puts "Banana" (0x42) before "apple"
    // (0x61) and would fail this. strcasecmp is the shipped behavior.
    std::vector<Movie> movies{
        make_movie(1, "Banana", 2026, kCutoff, true, 1),
    };
    std::vector<Series> tv{
        make_series(2, "apple", 2020, kCutoff, {make_season(1, 3, 3)}, 3),
        make_series(3, "Cherry", 2021, kCutoff, {make_season(1, 3, 3)}, 3),
    };
    const auto entries = mbu::build_library_entries(
        movies, tv, kNoWatchedMovies, kNoTvCounts, kNoDownloads);

    const auto view = mbu::build_library_view(entries, F::All, S::Title,
                                              kCutoff, true);
    REQUIRE(titles(view) == std::vector<std::string>{"apple", "Banana",
                                                     "Cherry"});
}

TEST_CASE("Year sorts descending with a case-insensitive title tiebreak",
          "[library_view][sort][year]") {
    MixedFixture fx;  // two 1994 rows: "old film" (movie) + "Ancient Show" (tv)
    const auto view = mbu::build_library_view(fx.entries, F::All, S::Year,
                                              kCutoff, true);
    REQUIRE(titles(view) == std::vector<std::string>{
        "Newest Show", "Middle Film", "Ancient Show", "old film"});
}

TEST_CASE("Size sorts through the entry back-pointers",
          "[library_view][sort][size]") {
    // Movies read movie->file_size_bytes; series read
    // series->size_on_disk_bytes. The fixture interleaves them so a
    // comparator reading only one side would misorder: 9G show > 2G film >
    // 1G show > 700M film.
    MixedFixture fx;
    const auto view = mbu::build_library_view(fx.entries, F::All, S::Size,
                                              kCutoff, true);
    REQUIRE(titles(view) == std::vector<std::string>{
        "Newest Show", "Middle Film", "Ancient Show", "old film"});
}

TEST_CASE("Size sorts past the 32-bit boundary", "[library_view][sort][size]") {
    // Sizes are int64_t and real files exceed 2^31 bytes; a comparator that
    // truncated to int would order these wrong.
    std::vector<Movie> movies{
        make_movie(1, "TwoGigPlus", 2026, kCutoff, true, 2147483649LL),
        make_movie(2, "JustUnder", 2026, kCutoff, true, 2147483647LL),
    };
    std::vector<Series> tv{
        make_series(3, "FourGig", 2020, kCutoff,
                    {make_season(1, 3, 3)}, 3, 4LL * 1024 * 1024 * 1024),
    };
    const auto entries = mbu::build_library_entries(
        movies, tv, kNoWatchedMovies, kNoTvCounts, kNoDownloads);

    const auto view = mbu::build_library_view(entries, F::All, S::Size,
                                              kCutoff, true);
    REQUIRE(titles(view) == std::vector<std::string>{"FourGig", "TwoGigPlus",
                                                     "JustUnder"});
}

// =====================================================================
// Filter and sort compose
// =====================================================================

TEST_CASE("The filter runs before the sort", "[library_view][compose]") {
    // Asserts the SET and the ORDER together: the two surviving entries must
    // come back newest-first even though the dropped entries would have
    // sorted between them.
    std::vector<Movie> movies{
        make_movie(1, "KeptOld", 2026, "2026-07-01T00:00:00Z", false, 1),
        make_movie(2, "DroppedNewest", 2026, "2026-07-28T00:00:00Z", true, 2),
        make_movie(3, "KeptNew", 2026, "2026-07-20T00:00:00Z", false, 3),
        make_movie(4, "DroppedMiddle", 2026, "2026-07-10T00:00:00Z", true, 4),
    };
    const std::vector<Series> no_tv;
    const auto entries = mbu::build_library_entries(
        movies, no_tv, kNoWatchedMovies, kNoTvCounts, kNoDownloads);

    const auto view = mbu::build_library_view(entries, F::MissingFiles,
                                              S::Recent, kCutoff, true);
    REQUIRE(titles(view) == std::vector<std::string>{"KeptNew", "KeptOld"});
}

// =====================================================================
// Empty inputs
// =====================================================================

TEST_CASE("Empty inputs yield no entries and an empty view everywhere",
          "[library_view][empty]") {
    const std::vector<Movie> no_movies;
    const std::vector<Series> no_tv;

    const auto entries = mbu::build_library_entries(
        no_movies, no_tv, kNoWatchedMovies, kNoTvCounts, kNoDownloads);
    REQUIRE(entries.empty());

    const F filters[] = {F::All, F::Unwatched, F::MissingFiles,
                         F::RecentlyAdded};
    const S sorts[]   = {S::Recent, S::Title, S::Year, S::Size};
    for (F f : filters) {
        for (S s : sorts) {
            // Both cutoff states, so the show-all fallback is exercised on
            // an empty input too.
            REQUIRE(mbu::build_library_view(entries, f, s, kCutoff, true).empty());
            REQUIRE(mbu::build_library_view(entries, f, s, "", false).empty());
        }
    }
}

// =====================================================================
// The returned pointers are borrowed from the caller's vector
// =====================================================================

TEST_CASE("The view holds pointers into the caller's own entries",
          "[library_view][contract]") {
    std::vector<Movie> movies{
        make_movie(1, "Zeta", 1990, "1990-01-01T00:00:00Z", true, 1),
        make_movie(2, "Alpha", 2000, "2000-01-01T00:00:00Z", true, 2),
    };
    const std::vector<Series> no_tv;
    const auto entries = mbu::build_library_entries(
        movies, no_tv, kNoWatchedMovies, kNoTvCounts, kNoDownloads);

    const auto view = mbu::build_library_view(entries, F::All, S::Title,
                                              kCutoff, true);
    REQUIRE(view.size() == 2);
    // Title order is Alpha, Zeta — reversed from the input — so these
    // address comparisons also prove the sort permuted pointers rather than
    // copying values.
    REQUIRE(view[0] == &entries[1]);
    REQUIRE(view[1] == &entries[0]);
}

TEST_CASE("Entries borrow the caller's Movie/Series storage",
          "[library_view][contract]") {
    // The back-pointers alias the input vectors — the caller must keep both
    // alive and unmoved for the entries' lifetime (LibraryScreen swaps and
    // rebuilds all three on the render thread, in one apply_pending pass).
    std::vector<Movie> movies{make_movie(1, "Only", 2026, kCutoff, true, 1)};
    std::vector<Series> tv{
        make_series(2, "Show", 2020, kCutoff, {make_season(1, 2, 2)}, 2),
    };
    const auto entries = mbu::build_library_entries(
        movies, tv, kNoWatchedMovies, kNoTvCounts, kNoDownloads);

    REQUIRE(entries.size() == 2);
    movies[0].file_size_bytes = 777;
    tv[0].size_on_disk_bytes = 888;
    REQUIRE(entries[0].movie->file_size_bytes == 777);
    REQUIRE(entries[1].series->size_on_disk_bytes == 888);
}

// =====================================================================
// Reserve
// =====================================================================

TEST_CASE("The view reserves the full entry count up front",
          "[library_view][reserve]") {
    // build_library_view reserves entries.size() before filtering, so a
    // heavily-filtering pass allocates once instead of growing. Asserted
    // rather than assumed: with only one entry surviving out of eight, a
    // capacity of 8 can only come from the reserve.
    std::vector<Movie> movies;
    for (int i = 0; i < 8; ++i) {
        movies.push_back(make_movie(100 + i, "Movie" + std::to_string(i),
                                    2000 + i, kCutoff, i != 3, 100));
    }
    const std::vector<Series> no_tv;
    const auto entries = mbu::build_library_entries(
        movies, no_tv, kNoWatchedMovies, kNoTvCounts, kNoDownloads);

    const auto view = mbu::build_library_view(entries, F::MissingFiles,
                                              S::Title, kCutoff, true);
    REQUIRE(view.size() == 1);
    REQUIRE(view.capacity() >= entries.size());
}

// =====================================================================
// Entry ordering out of the builder (pre-sort contract)
// =====================================================================

TEST_CASE("build_library_entries emits movies then series, in input order",
          "[library_view][entries][order]") {
    // Sorting stays the caller's job (build_library_view). Pinning the
    // builder's raw order keeps index-based assertions above honest.
    std::vector<Movie> movies{
        make_movie(1, "M1", 2020, kCutoff, true, 1),
        make_movie(2, "M2", 2021, kCutoff, true, 1),
    };
    std::vector<Series> tv{
        make_series(3, "S1", 2019, kCutoff, {make_season(1, 2, 2)}, 2),
        make_series(4, "S2", 2018, kCutoff, {make_season(1, 2, 2)}, 2),
    };
    const auto entries = mbu::build_library_entries(
        movies, tv, kNoWatchedMovies, kNoTvCounts, kNoDownloads);
    REQUIRE(entry_titles(entries) == std::vector<std::string>{
        "M1", "M2", "S1", "S2"});
}
