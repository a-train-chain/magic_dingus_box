#pragma once

#include <vector>

#include "media_browser/radarr/radarr_types.h"
#include "media_browser/tmdb_client.h"
#include "media_browser/ui/mb_screen.h"

namespace media_browser {
class RadarrClient;
class TmdbClient;
}

namespace media_browser::ui {

// Task 18 + Phase A/B (TMDB Discover overhaul): the Browse screen.
//
// Layout:
//   - Top strip (~72px): 9 chips split into two groups.
//       - Content chips (5): Popular, Now Playing, Top Rated, Upcoming,
//         Filter. Activating one reloads the poster grid. Active chip is
//         drawn in the theme accent color; inactive content chips are
//         dim.
//       - A thin vertical divider separates the groups.
//       - Nav chips (4): Search, Library, Queue, Settings. Activating
//         any of them transitions to the corresponding Screen instead of
//         reloading the grid. Nav chips are rendered in action/accent2
//         color when unfocused to set them apart from content chips.
//   - Filter panel (Phase B): rendered below the strip when Filter is
//     the active content category. Exposes Genre, Year, Sort-By. Any
//     change re-queries /discover/movie.
//   - Main area: 4-column poster grid.
//   - Bottom bar (~40px): control hints.
//
// Data source (Phase A):
//   TMDB Discover / category endpoints directly — not Radarr's
//   /movie/lookup search. Radarr stays responsible for library,
//   add-movie, queue. Clean separation.
class BrowseScreen : public MbScreen {
public:
    BrowseScreen(RadarrClient& radarr, TmdbClient& tmdb);

    void enter() override;
    Screen handle_input(const std::vector<platform::InputEvent>& events) override;
    void render(::ui::Renderer& r, int screen_w, int screen_h) override;

    // tmdb_id of the poster most recently selected by the user. Consumed by
    // the dispatcher in main.cpp to forward to DetailScreen on transition.
    int selected_tmdb_id() const { return selected_tmdb_id_; }

private:
    // Top-strip chip order. Content chips load a grid; nav chips transition.
    enum class Category {
        Popular = 0,
        NowPlaying = 1,
        TopRated = 2,
        Upcoming = 3,
        Filter = 4,
        Search = 5,
        Library = 6,
        Queue = 7,
        Settings = 8,
    };
    enum class Focus {
        CategoryStrip,
        FilterPanel,   // Phase B — only reachable when Category::Filter is active.
        PosterGrid,
    };

    // Filter panel row / control identifiers.
    enum class FilterRow { Genre = 0, Year = 1, SortBy = 2, Count = 3 };

    static constexpr int kNumContentCategories = 5;  // Popular..Filter
    static constexpr int kNumCategories = 9;
    static constexpr int kGridCols = 4;

    static bool is_nav_chip(Category cat) {
        return static_cast<int>(cat) >= kNumContentCategories;
    }

    void load_category(Category cat);
    void reload_filter_results();
    // Lazily fetches /genre/movie/list on first entry to the Filter category.
    void ensure_genres_loaded();
    // Cycle the current filter_row_'s value by `delta` (+1 / -1 typical).
    void cycle_filter_value(int delta);

    static const char* label_for_category(Category cat);

    RadarrClient& radarr_;
    TmdbClient& tmdb_;

    Category category_ = Category::Popular;
    Focus focus_ = Focus::PosterGrid;
    int category_cursor_ = 0;   // Index into the top strip when Focus::CategoryStrip.
    int grid_cursor_ = 0;       // Flat index into movies_ when Focus::PosterGrid.
    int scroll_row_ = 0;        // Topmost visible row index.

    // Result set for the active category. Uses TmdbSearchHit directly —
    // the only shape the renderer needs is tmdb_id/title/year/poster URL.
    std::vector<TmdbSearchHit> movies_;
    bool loaded_ = false;
    // True while load_category() is in-flight. Drives the "Loading..." state.
    bool loading_ = false;
    // Snapshot of radarr_.is_reachable() at enter() time. Drives the
    // "Radarr service offline" banner.
    bool services_ok_ = true;

    int selected_tmdb_id_ = 0;
    // Set when handle_input() wants the dispatcher to transition to Search
    // (via the top-strip Search category). Cleared on the next enter().
    bool want_search_screen_ = false;

    // --- Phase B: filter state -------------------------------------
    DiscoverFilter current_filter_;
    std::vector<Genre> genres_;
    bool genres_loaded_ = false;
    FilterRow filter_row_ = FilterRow::Genre;  // Focused row in the panel.

    // Available sort-by strings (paired with display labels in the .cpp).
    // current_sort_index_ is the index into a static array in the .cpp.
    int current_sort_index_ = 0;
};

}  // namespace media_browser::ui
