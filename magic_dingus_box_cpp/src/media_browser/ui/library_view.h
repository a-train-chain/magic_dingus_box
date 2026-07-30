#pragma once

// The Library grid's filter + sort decision logic, as pure functions.
//
// This is the tested half of what LibraryScreen::rebuild_view() used to do
// inline. The split is along the impurity line, not along a feature line:
//
//   here          — which rows survive a filter, and in what order they sit
//   library_screen — reading the clock for the "recently added" cutoff, the
//                    latched spdlog::warn when that read fails, assigning the
//                    result to view_, and clamping grid_cursor_/scroll_row_
//
// Nothing here names ::ui::Renderer, and that is the entire point. Renderer
// pulls in GLES, which exists in no test target and not at all on the mac dev
// box, so library_screen.cpp compiles only into the kiosk binary — which is why
// this logic had zero automated coverage and why the "recently added" cutoff
// bug shipped. A single Renderer include would put it back out of reach.
//
// Movie comes from radarr_types.h rather than radarr_client.h: that is where
// the struct is actually defined, and it costs the test binary only <set>,
// <string>, <vector> and <cstdint> instead of jsoncpp.

#include <string>
#include <vector>

#include "app/app_state.h"                     // MbLibraryFilter / MbLibrarySort
#include "media_browser/radarr/radarr_types.h"  // Movie

namespace media_browser::ui {

// Whether one movie survives `filter`.
//
// `recent_cutoff_iso` / `recent_cutoff_valid` are only consulted for
// MbLibraryFilter::RecentlyAdded; every other filter ignores them. They are
// arguments rather than something computed in here because the clock read is
// the impure part, and because the invalid-cutoff branch is otherwise only
// reachable on a machine whose gmtime_r has already failed.
//
// Filter semantics, all preserved verbatim from rebuild_view():
//   All            — keeps everything.
//   Unwatched      — ALSO keeps everything. A deliberate placeholder: the kiosk
//                    tracks no watched-history yet, and an empty grid would
//                    read as "nothing is here" rather than "not implemented".
//                    Becomes `!m.watched` once Movie gains the field.
//   MissingFiles   — keeps `!m.has_file`.
//   RecentlyAdded  — keeps a row when
//                      !recent_cutoff_valid ||
//                      (!m.added_at.empty() && m.added_at >= recent_cutoff_iso)
//                    Movie::added_at is a Radarr ISO-8601 string and the cutoff
//                    is utils::iso8601_utc output; both are fixed-width, so the
//                    lexicographic compare IS a chronological compare and no
//                    date parsing is involved. Empty added_at is dropped rather
//                    than compared, since "" is below every real date.
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
                      const Movie& m,
                      const std::string& recent_cutoff_iso,
                      bool recent_cutoff_valid);

// Filter `library` by `filter`, then order the survivors by `sort`.
//
// Sort semantics, all preserved verbatim from rebuild_view():
//   Recent — added_at descending, as a plain string compare (ISO-8601, so
//            newest first). An empty added_at sorts last.
//   Title  — strcasecmp ascending, i.e. case-INSENSITIVE. "apple" precedes
//            "Banana", which a byte-wise `<` would get backwards.
//   Year   — year descending, with strcasecmp on the title as the tiebreak.
//   Size   — file_size_bytes descending.
//
// The sort is std::sort, so it is NOT stable: rows that compare equal under the
// chosen comparator (same added_at, case-insensitively equal titles, same year
// AND title, same byte count) come back in an unspecified relative order. That
// matches what the kiosk has always done.
//
// Returns pointers INTO `library`. The caller owns the elements and must
// outlive the result; any reallocation of `library` invalidates every pointer
// returned here. LibraryScreen satisfies this by rebuilding the view on the
// same thread that swaps the library, immediately after the swap.
std::vector<const Movie*> build_library_view(
    const std::vector<Movie>& library,
    ::app::AppState::DisplaySettings::MbLibraryFilter filter,
    ::app::AppState::DisplaySettings::MbLibrarySort sort,
    const std::string& recent_cutoff_iso,
    bool recent_cutoff_valid);

}  // namespace media_browser::ui
