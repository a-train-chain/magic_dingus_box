#pragma once

#include <vector>

#include "media_browser/radarr/radarr_types.h"
#include "media_browser/ui/mb_screen.h"

namespace media_browser { class RadarrClient; }

namespace media_browser::ui {

// Task 18: the Browse screen. Landing UI for the Movies feature.
//
// Layout:
//   - Top strip (~72px): 5 category labels (Popular, Now Playing, Top
//     Rated, Discover, Search). Active category is drawn in the theme
//     accent color; others are dimmed.
//   - Main area: 4-column poster grid. Each cell has a colored-quad
//     placeholder (a per-tmdb_id deterministic tint) + title + year. The
//     focused cell gets an accent-colored outline.
//   - Bottom bar (~40px): control hints.
//
// Navigation:
//   - DPad / ROTATE_VERTICAL up/down: jump between the category strip and
//     the poster grid, and move between rows inside the grid.
//   - DPad / ROTATE left/right: step through the grid (wraps to previous /
//     next row) or through the category labels when the strip has focus.
//   - SELECT (A button / rotary click): activate the focused item —
//     categories switch the loaded list (Search category transitions to
//     Screen::Search); posters remember the tmdb_id and transition to
//     Screen::Detail.
//   - SETTINGS_MENU (BTN4 / Menu): return to the kiosk main menu.
//
// Category -> Radarr mapping:
//   Popular, Now Playing, Top Rated, Discover each call RadarrClient::lookup()
//   with a representative query term. Radarr does not expose TMDB's real
//   "popular" / "top rated" endpoints, so these are approximations that
//   produce reasonable result sets for the first cut of the UI. The exact
//   queries live in query_for_category() in the .cpp.
class BrowseScreen : public MbScreen {
public:
    explicit BrowseScreen(RadarrClient& radarr);

    void enter() override;
    Screen handle_input(const std::vector<platform::InputEvent>& events) override;
    void render(::ui::Renderer& r, int screen_w, int screen_h) override;

    // tmdb_id of the poster most recently selected by the user. Consumed by
    // the dispatcher in main.cpp to forward to DetailScreen on transition.
    int selected_tmdb_id() const { return selected_tmdb_id_; }

private:
    enum class Category {
        Popular = 0,
        NowPlaying = 1,
        TopRated = 2,
        Discover = 3,
        Search = 4,
    };
    enum class Focus { CategoryStrip, PosterGrid };

    static constexpr int kNumCategories = 5;
    static constexpr int kGridCols = 4;

    void load_category(Category cat);
    static const char* label_for_category(Category cat);
    static const char* query_for_category(Category cat);

    RadarrClient& radarr_;

    Category category_ = Category::Popular;
    Focus focus_ = Focus::PosterGrid;
    int category_cursor_ = 0;   // Index into the top strip when Focus::CategoryStrip.
    int grid_cursor_ = 0;       // Flat index into movies_ when Focus::PosterGrid.
    int scroll_row_ = 0;        // Topmost visible row index.

    std::vector<MovieSearchHit> movies_;
    bool loaded_ = false;

    int selected_tmdb_id_ = 0;
    // Set when handle_input() wants the dispatcher to transition to Search
    // (via the top-strip Search category). Cleared on the next enter().
    bool want_search_screen_ = false;
};

}  // namespace media_browser::ui
