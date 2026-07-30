#include "media_browser/ui/library_view.h"

#include <algorithm>
#include <strings.h>  // strcasecmp

namespace media_browser::ui {

namespace {

using F = ::app::AppState::DisplaySettings::MbLibraryFilter;
using S = ::app::AppState::DisplaySettings::MbLibrarySort;

}  // namespace

bool library_row_kept(F filter, const Movie& m,
                      const std::string& recent_cutoff_iso,
                      bool recent_cutoff_valid) {
    bool keep = true;
    switch (filter) {
        case F::All:
            keep = true;
            break;
        case F::Unwatched:
            // Placeholder: kiosk doesn't track watched-history yet.
            // Accept all rows so the operator sees something while
            // the (soon) feature is in development. Will switch to
            // `keep = !m.watched;` once Movie.watched lands.
            keep = true;
            break;
        case F::MissingFiles:
            keep = !m.has_file;
            break;
        case F::RecentlyAdded:
            // Movie.added_at is a Radarr ISO-8601 string. Empty
            // strings (which Radarr should never emit but we guard
            // anyway) compare less-than the cutoff and are dropped.
            // No usable cutoff -> show all (see the header, and the
            // latched warn in LibraryScreen::rebuild_view()).
            keep = !recent_cutoff_valid ||
                   (!m.added_at.empty() && m.added_at >= recent_cutoff_iso);
            break;
    }
    return keep;
}

std::vector<const Movie*> build_library_view(
    const std::vector<Movie>& library, F filter, S sort,
    const std::string& recent_cutoff_iso, bool recent_cutoff_valid) {
    std::vector<const Movie*> view;
    view.reserve(library.size());

    // ---- Filter pass ----
    for (const Movie& m : library) {
        if (library_row_kept(filter, m, recent_cutoff_iso, recent_cutoff_valid)) {
            view.push_back(&m);
        }
    }

    // ---- Sort pass ----
    auto cmp_recent = [](const Movie* a, const Movie* b) {
        return a->added_at > b->added_at;  // newest first (ISO-8601 lex sort)
    };
    auto cmp_title = [](const Movie* a, const Movie* b) {
        return ::strcasecmp(a->title.c_str(), b->title.c_str()) < 0;
    };
    auto cmp_year = [](const Movie* a, const Movie* b) {
        if (a->year != b->year) return a->year > b->year;  // newest first
        return ::strcasecmp(a->title.c_str(), b->title.c_str()) < 0;
    };
    auto cmp_size = [](const Movie* a, const Movie* b) {
        return a->file_size_bytes > b->file_size_bytes;  // largest first
    };
    switch (sort) {
        case S::Recent: std::sort(view.begin(), view.end(), cmp_recent); break;
        case S::Title:  std::sort(view.begin(), view.end(), cmp_title);  break;
        case S::Year:   std::sort(view.begin(), view.end(), cmp_year);   break;
        case S::Size:   std::sort(view.begin(), view.end(), cmp_size);   break;
    }

    return view;
}

}  // namespace media_browser::ui
