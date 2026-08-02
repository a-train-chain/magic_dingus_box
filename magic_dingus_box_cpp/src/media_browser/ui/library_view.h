#pragma once

// The mixed movie+TV Library grid's entry-building, filter and sort decision
// logic, as pure functions (Phase 3 Task 7 re-type of the movie-only layer).
//
// This is the tested half of what LibraryScreen does. The split is along the
// impurity line, not along a feature line:
//
//   here          — which entries exist, which survive a filter, and in what
//                    order they sit
//   library_screen — reading the clock for the "recently added" cutoff, the
//                    latched spdlog::warn when that read fails, reading
//                    WatchStore on the render thread, assigning the result to
//                    view_, and clamping grid_cursor_/scroll_row_
//
// Nothing here names ::ui::Renderer, and that is the entire point. Renderer
// pulls in GLES, which exists in no test target and not at all on the mac dev
// box, so library_screen.cpp compiles only into the kiosk binary — which is
// why this logic had zero automated coverage when it lived there, and why the
// "recently added" cutoff bug shipped. A single Renderer include would put it
// back out of reach.
//
// Movie comes from radarr_types.h and Series from sonarr_types.h — the files
// where the structs are actually defined — so the test binary pays for
// <set>/<string>/<vector>/<cstdint>, not jsoncpp.

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "app/app_state.h"                       // MbLibraryFilter / MbLibrarySort
#include "media_browser/media_ref.h"             // MediaRef / MediaKind
#include "media_browser/radarr/radarr_types.h"   // Movie
#include "media_browser/sonarr/sonarr_types.h"   // Series / Season

namespace media_browser::ui {

// One Library grid cell, either kind. Built by build_library_entries; the
// filter/sort layer below and LibraryScreen's render loop both consume it.
//
// The counts and the watched flag are the load-bearing part:
//
//   * Movies: file_count = has_file ? 1 : 0, total_count = 1, watched =
//     membership in WatchStore::watched_movie_ids().
//   * TV: file_count / total_count sum Season::episode_file_count /
//     episode_count over seasons with season_number > 0 — the SAME
//     season-0-excluded basis as WatchStore::tv_watched_counts (Task 3
//     reviewer adjudication). The series-level statistics.episodeFileCount
//     INCLUDES imported S0 specials; using it as the watched denominator
//     would leave such a series permanently un-watchable, because the S0
//     rows can never appear in the numerator. The series-level stat drives
//     the INCLUSION rule only (see build_library_entries).
//   * watched (tv) = watched_count >= file_count && file_count > 0. The `>=`
//     (not `==`) absorbs Task 3's accepted v1 note: watch rows never GC, so
//     a re-sourced series can hold a count larger than its CURRENT files.
//     The `file_count > 0` guard keeps a 0-file downloading series
//     un-watched — which is what makes the Unwatched filter keep it.
//
// `movie`/`series` point INTO the vectors handed to build_library_entries —
// exactly one is non-null, matching ref.kind. Same borrowing contract as the
// view below: the caller owns the storage and must keep it alive and unmoved.
struct LibraryEntry {
    MediaRef ref;
    std::string title;
    int year = 0;
    std::string poster_url;   // may be empty -> renderer draws the tint
    std::string added_at;     // ISO-8601, from Radarr/Sonarr verbatim
    int file_count = 0;
    int total_count = 0;
    bool downloading = false;
    bool watched = false;
    const Movie* movie = nullptr;
    const Series* series = nullptr;
};

// Builds the raw (unfiltered, unsorted) entry list: one entry per Movie, in
// input order, then one per INCLUDED Series, in input order. Sorting stays
// the caller's job (build_library_view).
//
// TV inclusion rule: a series produces an entry only when
//   series.episode_file_count > 0 || downloading_refs.count({Tv, tmdb_id})
// The first operand is the SERIES-LEVEL stat on purpose — a series whose only
// files are S0 specials still owns real disk content and must stay visible —
// and the second is the escape hatch for a freshly-started download whose
// first import hasn't landed (episodeFileCount == 0), without which no TV
// tile could ever show DOWNLOADING.
//
// `watched_movie_ids` / `tv_watched_counts` are WatchStore::watched_movie_ids
// / tv_watched_counts snapshots — read them on the render thread (WatchStore
// is main-thread-only) and pass them in; this function never touches the
// store. `downloading_refs` is MediaRef-keyed because the TMDB movie and TV
// id spaces overlap completely: a Tv-kind ref must never light up a movie
// sharing the integer id, and vice versa.
std::vector<LibraryEntry> build_library_entries(
    const std::vector<Movie>& movies,
    const std::vector<Series>& tv,
    const std::unordered_set<int>& watched_movie_ids,
    const std::unordered_map<int, int>& tv_watched_counts,
    const std::unordered_set<MediaRef>& downloading_refs);

// Whether one entry survives `filter`.
//
// `recent_cutoff_iso` / `recent_cutoff_valid` are only consulted for
// MbLibraryFilter::RecentlyAdded; every other filter ignores them. They are
// arguments rather than something computed in here because the clock read is
// the impure part, and because the invalid-cutoff branch is otherwise only
// reachable on a machine whose gmtime_r has already failed.
//
// Filter semantics, all pinned by tests:
//   All            — keeps everything.
//   Unwatched      — keeps `!e.watched`. ONE line for BOTH kinds via the
//                    precomputed entry.watched. A 0-file downloading series
//                    has watched == false so it IS kept — deliberately
//                    superseding the spec's `watched_episode_count <
//                    episodeFileCount` formula, which reads 0 < 0 and would
//                    drop the very tile whose DOWNLOADING badge is the
//                    point. Movies with no file: watched=false, kept.
//   MissingFiles   — keeps `file_count < total_count`. For movies this is
//                    exactly the old `!has_file` (0 < 1); for TV it means
//                    "gaps in S1+" — missing SPECIALS are not missing files,
//                    since both counts are season-0-excluded.
//   RecentlyAdded  — keeps an entry when
//                      !recent_cutoff_valid ||
//                      (!e.added_at.empty() && e.added_at >= recent_cutoff_iso)
//                    added_at is a Radarr/Sonarr ISO-8601 string and the
//                    cutoff is utils::iso8601_utc output; both are
//                    fixed-width, so the lexicographic compare IS a
//                    chronological compare and no date parsing is involved.
//                    Empty added_at is dropped rather than compared, since
//                    "" is below every real date.
//
// The `!recent_cutoff_valid ||` short-circuit is the show-all fallback for an
// unformattable cutoff and is load-bearing, not redundant. The flag is
// authoritative: do NOT reduce this to a comparison against an empty cutoff
// string on the theory that "" already passes everything. That reasoning is
// exactly how the original bug worked — the pre-iso8601_utc code ignored
// gmtime_r's return and formatted a zero tm into "1900-01-00T00:00:00Z", a
// well-formed string that every real date compared greater than, so the date
// filter became a pass-everything filter with no crash and nothing logged.
// Showing everything is the deliberate choice over showing nothing (an empty
// grid reads as "your library is empty", a scarier failure on an appliance),
// but it has to be a branch someone can see and test.
bool library_row_kept(::app::AppState::DisplaySettings::MbLibraryFilter filter,
                      const LibraryEntry& e,
                      const std::string& recent_cutoff_iso,
                      bool recent_cutoff_valid);

// Filter `entries` by `filter`, then order the survivors by `sort`.
//
// Sort semantics, preserved verbatim from the movie-only layer, now over
// mixed-kind entries:
//   Recent — added_at descending, as a plain string compare (ISO-8601, so
//            newest first). An empty added_at sorts last.
//   Title  — strcasecmp ascending, i.e. case-INSENSITIVE. "apple" precedes
//            "Banana", which a byte-wise `<` would get backwards.
//   Year   — year descending, with strcasecmp on the title as the tiebreak.
//   Size   — bytes descending, read through the entry's back-pointers:
//            movie->file_size_bytes for movies, series->size_on_disk_bytes
//            for TV.
//
// The sort is std::sort, so it is NOT stable: entries that compare equal
// under the chosen comparator come back in an unspecified relative order.
// That matches what the kiosk has always done.
//
// Returns pointers INTO `entries`. The caller owns the elements and must
// outlive the result; any reallocation of `entries` invalidates every pointer
// returned here. LibraryScreen satisfies this by rebuilding entries and view
// on the same thread that swaps the libraries, immediately after the swap.
std::vector<const LibraryEntry*> build_library_view(
    const std::vector<LibraryEntry>& entries,
    ::app::AppState::DisplaySettings::MbLibraryFilter filter,
    ::app::AppState::DisplaySettings::MbLibrarySort sort,
    const std::string& recent_cutoff_iso,
    bool recent_cutoff_valid);

}  // namespace media_browser::ui
