#pragma once

#include <chrono>
#include <string>
#include <unordered_set>
#include <vector>

#include "media_browser/radarr/radarr_types.h"
#include "media_browser/ui/mb_screen.h"
#include "ui/virtual_keyboard.h"

namespace media_browser { class RadarrClient; }

namespace media_browser::ui {

// Task 19: the Search screen. Replaces the Task 17 stub.
//
// Layout (rough):
//   - Top ~40% of screen:
//       * Query box (shows the current text buffer).
//       * Virtual-keyboard widget (reuses ui::VirtualKeyboard from the
//         kiosk main UI — we do NOT invent a new one).
//   - Bottom ~60% of screen:
//       * Live Radarr search results, 3-column poster grid (smaller
//         cells than BrowseScreen because the keyboard eats vertical
//         real estate).
//
// Debounce: every time the query changes, we wait ~400ms of typing idle
// before calling RadarrClient::lookup(query). That way holding down a
// direction or typing fast doesn't spam the Radarr API on every
// keystroke. A small "searching..." indicator shows while a lookup is
// in flight.
//
// Focus model:
//   - Two focus regions: Keyboard (top) and Results (bottom).
//   - Starts in Keyboard with the virtual keyboard internally focused on
//     its first row.
//   - LEFT/RIGHT always go to VirtualKeyboard::navigate_left/right.
//   - UP/DOWN inside the keyboard call navigate_up/down — except that
//     pressing DOWN while on the keyboard's bottom row AND at least one
//     result is visible transitions focus to the Results grid. Pressing
//     UP from the top row of the Results grid returns focus to the
//     keyboard.
//   - SELECT on the keyboard is VirtualKeyboard::select() (types a
//     character, backspaces, toggles caps, etc.). SELECT on a result
//     stores its tmdb_id and transitions to Screen::Detail.
//   - SETTINGS_MENU returns to Screen::Browse (back stack — not the
//     kiosk main menu, unlike BrowseScreen which returns to Exit).
class SearchScreen : public MbScreen {
public:
    explicit SearchScreen(RadarrClient& radarr);

    void enter() override;
    Screen handle_input(const std::vector<platform::InputEvent>& events) override;
    void update() override;
    void render(::ui::Renderer& r, int screen_w, int screen_h) override;

    // tmdb_id of the most recently selected result poster. The dispatcher
    // in main.cpp reads this to forward to DetailScreen on transition
    // (same handoff pattern BrowseScreen uses).
    int selected_tmdb_id() const { return selected_tmdb_id_; }

private:
    enum class Focus { Keyboard, Results };

    static constexpr int kGridCols = 3;
    static constexpr int kDebounceMs = 400;

    void run_lookup_if_due();
    // BTN2 quick-add: only fires when Focus::Results — adds the focused
    // result to the Radarr library (same behavior as BrowseScreen).
    void quick_add_focused();

    RadarrClient& radarr_;
    ::ui::VirtualKeyboard keyboard_;

    Focus focus_ = Focus::Keyboard;

    // Query state / debounce tracking.
    std::string query_;
    std::string last_queried_;
    std::chrono::steady_clock::time_point last_input_time_ =
        std::chrono::steady_clock::time_point::min();
    bool searching_ = false;

    std::vector<MovieSearchHit> results_;

    int grid_cursor_ = 0;
    int scroll_row_ = 0;

    int selected_tmdb_id_ = 0;

    // --- BTN2 quick-add cache (same shape as BrowseScreen) ------------
    std::unordered_set<int> library_tmdb_ids_;
    std::vector<QualityProfile> quality_profiles_;
    bool library_cached_ = false;
};

}  // namespace media_browser::ui
