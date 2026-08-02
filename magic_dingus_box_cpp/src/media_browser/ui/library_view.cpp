#include "media_browser/ui/library_view.h"

#include <algorithm>
#include <cstdint>
#include <strings.h>  // strcasecmp

namespace media_browser::ui {

namespace {

using F = ::app::AppState::DisplaySettings::MbLibraryFilter;
using S = ::app::AppState::DisplaySettings::MbLibrarySort;

// The Size sort's byte count, through the entry's back-pointers. Exactly one
// of the two pointers is non-null by construction; the 0 fallback keeps a
// malformed entry sorting last instead of reading through null.
int64_t entry_size_bytes(const LibraryEntry* e) {
    if (e->movie)  return e->movie->file_size_bytes;
    if (e->series) return e->series->size_on_disk_bytes;
    return 0;
}

}  // namespace

std::vector<LibraryEntry> build_library_entries(
    const std::vector<Movie>& movies,
    const std::vector<Series>& tv,
    const std::unordered_set<int>& watched_movie_ids,
    const std::unordered_map<int, int>& tv_watched_counts,
    const std::unordered_set<MediaRef>& downloading_refs) {
    std::vector<LibraryEntry> entries;
    entries.reserve(movies.size() + tv.size());

    // ---- Movies: one entry each, no inclusion rule ----
    for (const Movie& m : movies) {
        LibraryEntry e;
        e.ref         = MediaRef{MediaKind::Movie, m.tmdb_id};
        e.title       = m.title;
        e.year        = m.year;
        e.poster_url  = m.poster_url;
        e.added_at    = m.added_at;
        e.file_count  = m.has_file ? 1 : 0;
        e.total_count = 1;
        e.downloading = downloading_refs.count(e.ref) > 0;
        e.watched     = watched_movie_ids.count(m.tmdb_id) > 0;
        e.movie       = &m;
        entries.push_back(std::move(e));
    }

    // ---- TV: included when the series-level stat says any file exists OR a
    // download is active. The series-level statistics.episodeFileCount is
    // correct HERE (a specials-only series still owns real disk content) and
    // ONLY here — the counts and the watched math below use the
    // season-0-excluded sums, matching WatchStore::tv_watched_counts. ----
    for (const Series& s : tv) {
        const MediaRef ref{MediaKind::Tv, s.tmdb_id};
        const bool is_downloading = downloading_refs.count(ref) > 0;
        if (s.episode_file_count <= 0 && !is_downloading) continue;

        int file_count = 0;
        int total_count = 0;
        for (const Season& season : s.seasons) {
            if (season.season_number <= 0) continue;  // S0 excluded everywhere
            file_count  += season.episode_file_count;
            total_count += season.episode_count;
        }

        int watched_count = 0;
        if (auto it = tv_watched_counts.find(s.tmdb_id);
            it != tv_watched_counts.end()) {
            watched_count = it->second;
        }

        LibraryEntry e;
        e.ref         = ref;
        e.title       = s.title;
        e.year        = s.year;
        e.poster_url  = s.poster_url;
        e.added_at    = s.added_at;
        e.file_count  = file_count;
        e.total_count = total_count;
        e.downloading = is_downloading;
        // >= not ==: watch rows never GC, so a re-sourced series can hold a
        // count above its CURRENT files (Task 3 accepted v1 note). The
        // file_count > 0 guard keeps a 0-file series un-watched, which is
        // what makes the Unwatched filter keep its DOWNLOADING tile.
        e.watched     = file_count > 0 && watched_count >= file_count;
        e.series      = &s;
        entries.push_back(std::move(e));
    }

    return entries;
}

bool library_row_kept(F filter, const LibraryEntry& e,
                      const std::string& recent_cutoff_iso,
                      bool recent_cutoff_valid) {
    bool keep = true;
    switch (filter) {
        case F::All:
            keep = true;
            break;
        case F::Unwatched:
            // Real since Phase 3: one line for BOTH kinds via the
            // precomputed entry.watched (movies: watched-set membership;
            // tv: watched_count >= s0-excluded file_count && files > 0).
            // A 0-file downloading series is !watched, so it IS kept.
            keep = !e.watched;
            break;
        case F::MissingFiles:
            // Movies: 0 < 1 iff !has_file — the pre-TV meaning, preserved.
            // TV: gaps in S1+ (both counts are season-0-excluded, so
            // missing specials are not missing files).
            keep = e.file_count < e.total_count;
            break;
        case F::RecentlyAdded:
            // added_at is a Radarr/Sonarr ISO-8601 string. Empty strings
            // (which neither service should emit but we guard anyway)
            // compare less-than the cutoff and are dropped. No usable
            // cutoff -> show all (see the header, and the latched warn in
            // LibraryScreen::rebuild_view()).
            keep = !recent_cutoff_valid ||
                   (!e.added_at.empty() && e.added_at >= recent_cutoff_iso);
            break;
    }
    return keep;
}

std::vector<const LibraryEntry*> build_library_view(
    const std::vector<LibraryEntry>& entries, F filter, S sort,
    const std::string& recent_cutoff_iso, bool recent_cutoff_valid) {
    std::vector<const LibraryEntry*> view;
    view.reserve(entries.size());

    // ---- Filter pass ----
    for (const LibraryEntry& e : entries) {
        if (library_row_kept(filter, e, recent_cutoff_iso, recent_cutoff_valid)) {
            view.push_back(&e);
        }
    }

    // ---- Sort pass ----
    auto cmp_recent = [](const LibraryEntry* a, const LibraryEntry* b) {
        return a->added_at > b->added_at;  // newest first (ISO-8601 lex sort)
    };
    auto cmp_title = [](const LibraryEntry* a, const LibraryEntry* b) {
        return ::strcasecmp(a->title.c_str(), b->title.c_str()) < 0;
    };
    auto cmp_year = [](const LibraryEntry* a, const LibraryEntry* b) {
        if (a->year != b->year) return a->year > b->year;  // newest first
        return ::strcasecmp(a->title.c_str(), b->title.c_str()) < 0;
    };
    auto cmp_size = [](const LibraryEntry* a, const LibraryEntry* b) {
        return entry_size_bytes(a) > entry_size_bytes(b);  // largest first
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
