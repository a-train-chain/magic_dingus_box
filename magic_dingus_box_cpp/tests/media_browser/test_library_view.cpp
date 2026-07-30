// Tests for media_browser/ui/library_view — the filter + sort decision logic
// that LibraryScreen::rebuild_view() used to inline.
//
// Why this file exists at all: that logic lived in library_screen.cpp, which
// names ::ui::Renderer, so it compiles only into the kiosk binary and cannot
// even be built on the mac dev box. It therefore had zero coverage, and that is
// how the "recently added" cutoff bug shipped — a discarded gmtime_r return
// produced a well-formed-but-wrong "1900-01-00T00:00:00Z" that every real date
// compared greater than, silently turning the date filter into a
// pass-everything filter with no crash and nothing logged.
//
// library_view.h deliberately names no Renderer. The impure half (reading the
// clock, the latched spdlog::warn, mutating the screen's members) stays in
// rebuild_view(); everything asserted below is a pure function of its
// arguments.
//
// The cutoff is injected as (string, bool) rather than computed here, so the
// show-all fallback branch — the one the bug lived in — is directly reachable
// from a test instead of only on a machine whose gmtime_r has failed.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "app/app_state.h"
#include "media_browser/radarr/radarr_types.h"
#include "media_browser/ui/library_view.h"

namespace mbu = media_browser::ui;
using Movie  = media_browser::Movie;
using F      = ::app::AppState::DisplaySettings::MbLibraryFilter;
using S      = ::app::AppState::DisplaySettings::MbLibrarySort;

namespace {

Movie make_movie(const std::string& title, int year,
                 const std::string& added_at, bool has_file,
                 int64_t file_size_bytes) {
    Movie m;
    m.title            = title;
    m.year             = year;
    m.added_at         = added_at;
    m.has_file         = has_file;
    m.file_size_bytes  = file_size_bytes;
    return m;
}

std::vector<std::string> titles(const std::vector<const Movie*>& view) {
    std::vector<std::string> out;
    out.reserve(view.size());
    for (const Movie* m : view) out.push_back(m->title);
    return out;
}

// The cutoff shape LibraryScreen produces: utils::iso8601_utc output, fixed
// width, so lexicographic order equals chronological order.
const std::string kCutoff = "2026-06-29T00:00:00Z";

}  // namespace

// =====================================================================
// library_row_kept — RecentlyAdded, INVALID cutoff (the uncovered branch)
// =====================================================================

TEST_CASE("RecentlyAdded with an invalid cutoff keeps every row",
          "[library_view][filter][recent_fallback]") {
    // This is the branch the whole seam exists for. When the cutoff cannot be
    // formatted the filter degrades to show-all on purpose: an empty grid reads
    // as "your library is empty", which is a scarier failure on an appliance
    // than an unfiltered one.

    SECTION("empty cutoff string (what iso8601_utc returns on failure)") {
        const Movie ancient = make_movie("Nosferatu", 1922,
                                         "1922-03-04T00:00:00Z", true, 100);
        const Movie no_date = make_movie("Undated", 2026, "", true, 100);
        const Movie recent  = make_movie("Fresh", 2026,
                                         "2026-07-28T00:00:00Z", true, 100);

        REQUIRE(mbu::library_row_kept(F::RecentlyAdded, ancient, "", false));
        REQUIRE(mbu::library_row_kept(F::RecentlyAdded, no_date, "", false));
        REQUIRE(mbu::library_row_kept(F::RecentlyAdded, recent, "", false));
    }

    SECTION("the valid flag is authoritative, not the string's emptiness") {
        // A non-empty cutoff paired with valid == false must STILL keep
        // everything. Asserting this pins the `!recent_cutoff_valid ||`
        // short-circuit specifically: a version that dropped the flag and
        // relied on "" comparing less than every date would pass the section
        // above by accident and fail here.
        const Movie ancient = make_movie("Nosferatu", 1922,
                                         "1922-03-04T00:00:00Z", true, 100);
        const Movie no_date = make_movie("Undated", 2026, "", true, 100);

        REQUIRE(mbu::library_row_kept(F::RecentlyAdded, ancient, kCutoff, false));
        REQUIRE(mbu::library_row_kept(F::RecentlyAdded, no_date, kCutoff, false));
    }
}

TEST_CASE("build_library_view keeps the whole library when the cutoff is invalid",
          "[library_view][filter][recent_fallback]") {
    std::vector<Movie> library{
        make_movie("Nosferatu", 1922, "1922-03-04T00:00:00Z", true, 100),
        make_movie("Undated", 2026, "", false, 0),
        make_movie("Fresh", 2026, "2026-07-28T00:00:00Z", true, 200),
    };

    const auto view = mbu::build_library_view(library, F::RecentlyAdded,
                                             S::Title, "", false);
    REQUIRE(view.size() == 3);
    REQUIRE(titles(view) == std::vector<std::string>{"Fresh", "Nosferatu",
                                                     "Undated"});
}

// =====================================================================
// library_row_kept — RecentlyAdded, VALID cutoff
// =====================================================================

TEST_CASE("RecentlyAdded with a valid cutoff compares >= against added_at",
          "[library_view][filter][recent]") {
    // Strictly after the cutoff.
    const Movie after = make_movie("After", 2026, "2026-07-01T12:00:00Z",
                                   true, 100);
    // Exactly ON the cutoff — the >= boundary. A `>` would drop this.
    const Movie equal = make_movie("Equal", 2026, kCutoff, true, 100);
    // One second before the cutoff.
    const Movie before = make_movie("Before", 2026, "2026-06-28T23:59:59Z",
                                    true, 100);
    // Radarr should never emit this, but "" sorts below every real date and
    // must be DROPPED rather than silently treated as ancient-but-present.
    const Movie no_date = make_movie("Undated", 2026, "", true, 100);

    REQUIRE(mbu::library_row_kept(F::RecentlyAdded, after, kCutoff, true));
    REQUIRE(mbu::library_row_kept(F::RecentlyAdded, equal, kCutoff, true));
    REQUIRE_FALSE(mbu::library_row_kept(F::RecentlyAdded, before, kCutoff, true));
    REQUIRE_FALSE(mbu::library_row_kept(F::RecentlyAdded, no_date, kCutoff, true));
}

TEST_CASE("build_library_view drops pre-cutoff and undated rows",
          "[library_view][filter][recent]") {
    std::vector<Movie> library{
        make_movie("After", 2026, "2026-07-01T12:00:00Z", true, 100),
        make_movie("Undated", 2026, "", true, 100),
        make_movie("Before", 2026, "2026-06-28T23:59:59Z", true, 100),
        make_movie("Equal", 2026, kCutoff, true, 100),
    };

    const auto view = mbu::build_library_view(library, F::RecentlyAdded,
                                             S::Title, kCutoff, true);
    REQUIRE(titles(view) == std::vector<std::string>{"After", "Equal"});
}

// =====================================================================
// library_row_kept — MissingFiles
// =====================================================================

TEST_CASE("MissingFiles keeps exactly the rows with no file",
          "[library_view][filter][missing]") {
    const Movie with_file    = make_movie("Present", 2026, kCutoff, true, 100);
    const Movie without_file = make_movie("Absent", 2026, kCutoff, false, 0);

    REQUIRE_FALSE(mbu::library_row_kept(F::MissingFiles, with_file, kCutoff, true));
    REQUIRE(mbu::library_row_kept(F::MissingFiles, without_file, kCutoff, true));

    // The cutoff arguments are irrelevant to this filter and must not leak
    // into it — assert the same answers with the cutoff marked invalid.
    REQUIRE_FALSE(mbu::library_row_kept(F::MissingFiles, with_file, "", false));
    REQUIRE(mbu::library_row_kept(F::MissingFiles, without_file, "", false));
}

TEST_CASE("build_library_view with MissingFiles narrows to the missing rows",
          "[library_view][filter][missing]") {
    std::vector<Movie> library{
        make_movie("HasFile", 2026, kCutoff, true, 500),
        make_movie("NoFileA", 2026, kCutoff, false, 0),
        make_movie("AlsoHasFile", 2026, kCutoff, true, 400),
        make_movie("NoFileB", 2026, kCutoff, false, 0),
    };

    const auto view = mbu::build_library_view(library, F::MissingFiles,
                                             S::Title, kCutoff, true);
    REQUIRE(titles(view) == std::vector<std::string>{"NoFileA", "NoFileB"});
}

// =====================================================================
// library_row_kept — All and Unwatched (both pass-everything today)
// =====================================================================

TEST_CASE("All and Unwatched keep a row that fails every other filter",
          "[library_view][filter][passthrough]") {
    // has_file == true fails MissingFiles; an empty added_at fails
    // RecentlyAdded against a valid cutoff. Both of the pass-through filters
    // must still keep it.
    const Movie worst_case = make_movie("Fails Everything Else", 1980, "",
                                        true, 0);

    REQUIRE_FALSE(mbu::library_row_kept(F::MissingFiles, worst_case, kCutoff, true));
    REQUIRE_FALSE(mbu::library_row_kept(F::RecentlyAdded, worst_case, kCutoff, true));

    REQUIRE(mbu::library_row_kept(F::All, worst_case, kCutoff, true));
    // Unwatched is a deliberate placeholder: the kiosk tracks no watched
    // history yet, so it accepts every row. Pinned here so a future
    // `keep = !m.watched;` is a conscious, test-updating change rather than a
    // silent one.
    REQUIRE(mbu::library_row_kept(F::Unwatched, worst_case, kCutoff, true));
}

TEST_CASE("build_library_view with All or Unwatched keeps the whole library",
          "[library_view][filter][passthrough]") {
    std::vector<Movie> library{
        make_movie("Bravo", 2001, "", true, 0),
        make_movie("Alpha", 1999, "1999-01-01T00:00:00Z", false, 10),
        make_movie("Charlie", 2020, "2020-05-05T00:00:00Z", true, 20),
    };

    const auto all = mbu::build_library_view(library, F::All, S::Title,
                                            kCutoff, true);
    const auto unwatched = mbu::build_library_view(library, F::Unwatched,
                                                  S::Title, kCutoff, true);

    const std::vector<std::string> expected{"Alpha", "Bravo", "Charlie"};
    REQUIRE(titles(all) == expected);
    REQUIRE(titles(unwatched) == expected);
}

// =====================================================================
// Sorts
// =====================================================================

TEST_CASE("Recent sorts by added_at descending", "[library_view][sort]") {
    std::vector<Movie> library{
        make_movie("Middle", 2026, "2026-03-15T00:00:00Z", true, 1),
        make_movie("Oldest", 2026, "2019-12-31T23:59:59Z", true, 2),
        make_movie("Newest", 2026, "2026-07-29T08:00:00Z", true, 3),
    };

    const auto view = mbu::build_library_view(library, F::All, S::Recent,
                                             kCutoff, true);
    REQUIRE(titles(view) == std::vector<std::string>{"Newest", "Middle",
                                                     "Oldest"});
}

TEST_CASE("Recent puts an empty added_at last", "[library_view][sort]") {
    // "" is less than every non-empty string, and the comparator is `>`, so an
    // undated row sinks to the bottom of a Recent sort rather than floating to
    // the top. Worth pinning: the All filter lets undated rows through, so this
    // ordering is reachable in the kiosk.
    std::vector<Movie> library{
        make_movie("Undated", 2026, "", true, 1),
        make_movie("Dated", 2026, "2001-01-01T00:00:00Z", true, 2),
    };

    const auto view = mbu::build_library_view(library, F::All, S::Recent,
                                             kCutoff, true);
    REQUIRE(titles(view) == std::vector<std::string>{"Dated", "Undated"});
}

TEST_CASE("Title sorts case-INSENSITIVELY", "[library_view][sort][title]") {
    // A naive `a->title < b->title` puts "Banana" (0x42) before "apple" (0x61)
    // and would fail this. strcasecmp is the behavior the kiosk shipped.
    std::vector<Movie> library{
        make_movie("Banana", 2026, kCutoff, true, 1),
        make_movie("apple", 2026, kCutoff, true, 2),
        make_movie("Cherry", 2026, kCutoff, true, 3),
    };

    const auto view = mbu::build_library_view(library, F::All, S::Title,
                                             kCutoff, true);
    REQUIRE(titles(view) == std::vector<std::string>{"apple", "Banana",
                                                     "Cherry"});
}

TEST_CASE("Title sorts a realistic mixed-case set ascending",
          "[library_view][sort][title]") {
    std::vector<Movie> library{
        make_movie("the Matrix", 1999, kCutoff, true, 1),
        make_movie("Alien", 1979, kCutoff, true, 2),
        make_movie("blade runner", 1982, kCutoff, true, 3),
        make_movie("Zodiac", 2007, kCutoff, true, 4),
    };

    const auto view = mbu::build_library_view(library, F::All, S::Title,
                                             kCutoff, true);
    REQUIRE(titles(view) == std::vector<std::string>{
        "Alien", "blade runner", "the Matrix", "Zodiac"});
}

TEST_CASE("Year sorts descending", "[library_view][sort][year]") {
    std::vector<Movie> library{
        make_movie("Nineties", 1994, kCutoff, true, 1),
        make_movie("Modern", 2024, kCutoff, true, 2),
        make_movie("Silent", 1922, kCutoff, true, 3),
    };

    const auto view = mbu::build_library_view(library, F::All, S::Year,
                                             kCutoff, true);
    REQUIRE(titles(view) == std::vector<std::string>{"Modern", "Nineties",
                                                     "Silent"});
}

TEST_CASE("Year breaks ties with a case-insensitive title compare",
          "[library_view][sort][year]") {
    // Three 2001 titles whose case-sensitive and case-insensitive orders
    // differ: byte order would give "Zulu" < "aardvark" < "beta"; strcasecmp
    // gives aardvark < beta < Zulu.
    std::vector<Movie> library{
        make_movie("Zulu", 2001, kCutoff, true, 1),
        make_movie("beta", 2001, kCutoff, true, 2),
        make_movie("Newer", 2010, kCutoff, true, 3),
        make_movie("aardvark", 2001, kCutoff, true, 4),
    };

    const auto view = mbu::build_library_view(library, F::All, S::Year,
                                             kCutoff, true);
    REQUIRE(titles(view) == std::vector<std::string>{
        "Newer", "aardvark", "beta", "Zulu"});
}

TEST_CASE("Size sorts by file_size_bytes descending",
          "[library_view][sort][size]") {
    std::vector<Movie> library{
        make_movie("Medium", 2026, kCutoff, true, 2LL * 1024 * 1024 * 1024),
        make_movie("Empty", 2026, kCutoff, false, 0),
        make_movie("Huge", 2026, kCutoff, true, 9LL * 1024 * 1024 * 1024),
        make_movie("Small", 2026, kCutoff, true, 700LL * 1024 * 1024),
    };

    const auto view = mbu::build_library_view(library, F::All, S::Size,
                                             kCutoff, true);
    REQUIRE(titles(view) == std::vector<std::string>{"Huge", "Medium",
                                                     "Small", "Empty"});
}

TEST_CASE("Size sorts past the 32-bit boundary",
          "[library_view][sort][size]") {
    // file_size_bytes is int64_t and real movie files exceed 2^31 bytes; a
    // comparator that truncated to int would order these wrong.
    std::vector<Movie> library{
        make_movie("FourGig", 2026, kCutoff, true, 4LL * 1024 * 1024 * 1024),
        make_movie("TwoGigPlus", 2026, kCutoff, true, 2147483649LL),
        make_movie("JustUnder", 2026, kCutoff, true, 2147483647LL),
    };

    const auto view = mbu::build_library_view(library, F::All, S::Size,
                                             kCutoff, true);
    REQUIRE(titles(view) == std::vector<std::string>{"FourGig", "TwoGigPlus",
                                                     "JustUnder"});
}

// =====================================================================
// Filter and sort compose
// =====================================================================

TEST_CASE("The filter runs before the sort", "[library_view][compose]") {
    // If the sort ran first and the filter second the result would be the
    // same set, so this asserts the SET and the ORDER together: the two
    // surviving rows must come back newest-first even though the dropped
    // rows would have sorted between them.
    std::vector<Movie> library{
        make_movie("KeptOld", 2026, "2026-07-01T00:00:00Z", false, 1),
        make_movie("DroppedNewest", 2026, "2026-07-28T00:00:00Z", true, 2),
        make_movie("KeptNew", 2026, "2026-07-20T00:00:00Z", false, 3),
        make_movie("DroppedMiddle", 2026, "2026-07-10T00:00:00Z", true, 4),
    };

    const auto view = mbu::build_library_view(library, F::MissingFiles,
                                             S::Recent, kCutoff, true);
    REQUIRE(titles(view) == std::vector<std::string>{"KeptNew", "KeptOld"});
}

// =====================================================================
// Empty library
// =====================================================================

TEST_CASE("An empty library yields an empty view for every filter/sort pair",
          "[library_view][empty]") {
    const std::vector<Movie> empty;
    const F filters[] = {F::All, F::Unwatched, F::MissingFiles,
                         F::RecentlyAdded};
    const S sorts[]   = {S::Recent, S::Title, S::Year, S::Size};

    for (F f : filters) {
        for (S s : sorts) {
            // Both cutoff states, so the show-all fallback is exercised on an
            // empty input too.
            REQUIRE(mbu::build_library_view(empty, f, s, kCutoff, true).empty());
            REQUIRE(mbu::build_library_view(empty, f, s, "", false).empty());
        }
    }
}

// =====================================================================
// The returned pointers are borrowed from the caller's vector
// =====================================================================

TEST_CASE("The view holds pointers into the caller's own elements",
          "[library_view][contract]") {
    std::vector<Movie> library{
        make_movie("Zeta", 1990, "1990-01-01T00:00:00Z", true, 1),
        make_movie("Alpha", 2000, "2000-01-01T00:00:00Z", true, 2),
    };

    const auto view = mbu::build_library_view(library, F::All, S::Title,
                                             kCutoff, true);
    REQUIRE(view.size() == 2);
    // Title order is Alpha, Zeta — i.e. reversed from the input — so these
    // address comparisons also prove the sort permuted pointers rather than
    // copying values.
    REQUIRE(view[0] == &library[1]);
    REQUIRE(view[1] == &library[0]);
}

TEST_CASE("Mutating the source library is visible through the view",
          "[library_view][contract]") {
    std::vector<Movie> library{
        make_movie("Only", 2026, kCutoff, true, 1),
    };

    const auto view = mbu::build_library_view(library, F::All, S::Title,
                                             kCutoff, true);
    REQUIRE(view.size() == 1);
    library[0].title = "Renamed";
    REQUIRE(view[0]->title == "Renamed");
}

// =====================================================================
// Reserve
// =====================================================================

TEST_CASE("The view reserves the full library size up front",
          "[library_view][reserve]") {
    // rebuild_view() did view_.reserve(library_.size()) before filtering, so
    // a heavily-filtering pass allocates once instead of growing. Asserted
    // rather than assumed: with only one row surviving out of eight, a
    // capacity of 8 can only come from the reserve.
    std::vector<Movie> library;
    for (int i = 0; i < 8; ++i) {
        library.push_back(make_movie("Movie" + std::to_string(i), 2000 + i,
                                     kCutoff, i != 3, 100));
    }

    const auto view = mbu::build_library_view(library, F::MissingFiles,
                                             S::Title, kCutoff, true);
    REQUIRE(view.size() == 1);
    REQUIRE(view.capacity() >= library.size());
}
