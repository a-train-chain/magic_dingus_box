#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
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
// before kicking the (now async) Radarr lookup. That way holding down a
// direction or typing fast doesn't spam the Radarr API on every
// keystroke. A small "searching..." indicator shows while a lookup is
// in flight.
//
// Async fetch model (Task 6 of audit-improvements; mirrors BrowseScreen
// 8849b77 + DetailScreen aba7821):
//   Two INDEPENDENT pipelines, each with its own gen counter / mutex /
//   pending result / worker pool:
//     - lib_*  : one-shot library-cache fetch fired from enter() and
//                refreshed after a successful add. Drives the
//                "IN LIBRARY" overlay chips and gates BTN2 quick-add.
//     - lookup_*: bouncy lookup fetch fired from run_lookup_if_due()
//                whenever the typed query goes idle for kDebounceMs.
//                Each new keystroke past the debounce bumps the gen so
//                a stale in-flight lookup drops its result and the
//                latest query wins.
//   Both pipelines drain in update() via apply_pending_lib() /
//   apply_pending_lookup() — render() never blocks on HTTP. The
//   destructor invalidates both gens and joins all workers so a CURL
//   in-flight at shutdown can't outlive `this`.
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
    ~SearchScreen();

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

    // 9-column grid matches Browse / Library exactly — same cell width,
    // same poster aspect, same chrome::draw_poster_card chrome — so all
    // three Marquee grids read as one consistent vocabulary. The
    // keyboard still consumes the top ~50% of the screen, so typically
    // only one row of posters is visible at a time; the result list
    // scrolls vertically when there are more than kGridCols hits.
    static constexpr int kGridCols = 9;
    static constexpr int kDebounceMs = 400;

    void run_lookup_if_due();
    // BTN2 quick-add: only fires when Focus::Results — adds the focused
    // result to the Radarr library (same behavior as BrowseScreen).
    void quick_add_focused();

    // --- Async pipelines (lib_ + lookup_) -----------------------------
    // Library cache: one-shot fetch dispatched from enter() (and after a
    // successful add). Populates library_tmdb_ids_ + quality_profiles_.
    void start_lib_fetch();
    void run_lib_fetch(uint64_t gen);
    void apply_pending_lib();

    // Lookup: dispatched from run_lookup_if_due() each time the typed
    // query goes idle. The query is captured by value into the worker
    // so a mid-flight worker isn't racing the user's typing on `query_`.
    void start_lookup(std::string query);
    void run_lookup(uint64_t gen, std::string query);
    void apply_pending_lookup();

    // Bundled output of run_lib_fetch(). profiles_valid is separate
    // because we only re-fetch profiles on the FIRST library load —
    // subsequent refreshes (post-add) just refresh the in-library set.
    struct LibFetchResult {
        std::vector<Movie>          library;
        std::vector<QualityProfile> profiles;
        bool profiles_valid = false;
    };

    RadarrClient& radarr_;
    ::ui::VirtualKeyboard keyboard_;

    Focus focus_ = Focus::Keyboard;

    // Query state / debounce tracking.
    std::string query_;
    std::string last_queried_;
    std::chrono::steady_clock::time_point last_input_time_ =
        std::chrono::steady_clock::time_point::min();

    std::vector<MovieSearchHit> results_;

    int grid_cursor_ = 0;
    int scroll_row_ = 0;

    int selected_tmdb_id_ = 0;

    // --- BTN2 quick-add cache (same shape as BrowseScreen) ------------
    std::unordered_set<int> library_tmdb_ids_;
    std::vector<QualityProfile> quality_profiles_;
    bool library_cached_ = false;   // True once profiles_ have been fetched.
    bool lib_loaded_     = false;   // True once the in-library set has populated
                                    // at least once. Gates BTN2 quick-add.

    // --- Async lib_ pipeline ------------------------------------------
    // Library fetch was synchronous in enter() and blocked the render
    // thread for ~1s on every entry to the screen — visible as a stutter
    // when transitioning Browse -> Search. Worker thread does the HTTP
    // off the render thread; render() shows the chip-less grid until
    // apply_pending_lib() drains the result on a future update() tick.
    // Generation counter pattern same as BrowseScreen / DetailScreen.
    std::atomic<uint64_t>     lib_current_gen_{0};
    std::mutex                lib_result_mtx_;
    LibFetchResult            lib_pending_;
    std::atomic<bool>         lib_result_ready_{false};
    bool                      lib_loading_ = false;
    std::vector<std::thread>  lib_workers_;

    // --- Async lookup_ pipeline ---------------------------------------
    // The Radarr /movie/lookup call runs through Radarr's TMDB proxy and
    // typically takes 1-3s, occasionally 5+ on a cold-cache miss. Run
    // synchronously this froze the entire kiosk UI every time the user
    // paused typing. Each new lookup bumps lookup_current_gen_; the
    // worker captures gen at spawn time and only publishes its result
    // if lookup_current_gen_ still matches — so a rapid sequence of
    // typed pauses pre-empts stale workers without blocking on join.
    std::atomic<uint64_t>            lookup_current_gen_{0};
    std::mutex                       lookup_result_mtx_;
    std::vector<MovieSearchHit>      lookup_pending_;
    std::atomic<bool>                lookup_result_ready_{false};
    bool                             lookup_loading_ = false;
    std::vector<std::thread>         lookup_workers_;
};

}  // namespace media_browser::ui
