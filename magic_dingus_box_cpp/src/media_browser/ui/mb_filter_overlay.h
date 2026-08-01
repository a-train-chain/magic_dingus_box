#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "app/app_state.h"

namespace ui { class Renderer; }

namespace media_browser::ui {

// Identifies which underlying tab's filter state this overlay edits.
enum class FilterTabKind {
    Popular,
    TopRated,
    ForYou,   // SHUFFLE-only overlay in Phase 1 (spec 1c)
};

// The persisted Movies/TV mode, as the overlay sees it. mb_filter_state.h
// declares the identical alias; both are redeclarations of the same type.
using MbMode = ::app::AppState::DisplaySettings::MbMode;

// The overlay's row model, pure and free-standing so it can be asserted on
// without a GL Renderer (mb_filter_overlay.cpp cannot be linked into the test
// binary; this header can be included by it).
//
// Chart tabs (Popular / Top Rated), top to bottom:
//   0        MODE        action — toggles Movies/TV and applies immediately
//   1..6     values      GENRE, DECADE, MIN RATING, RUNTIME, LANGUAGE, ORDER
//   7        RESET ALL   action
//   8        SHUFFLE     action
// For You keeps no persisted filter state in either mode, so it gets exactly
//   0 MODE, 1 SHUFFLE.
enum class FilterRowRole { Mode, Value, Reset, Shuffle };

inline int filter_row_count(FilterTabKind tab) {
    return (tab == FilterTabKind::ForYou) ? 2 : 9;
}

inline FilterRowRole filter_row_role(FilterTabKind tab, int row) {
    if (tab == FilterTabKind::ForYou) {
        return (row == 0) ? FilterRowRole::Mode : FilterRowRole::Shuffle;
    }
    if (row == 0) return FilterRowRole::Mode;
    if (row == 7) return FilterRowRole::Reset;
    if (row == 8) return FilterRowRole::Shuffle;
    return FilterRowRole::Value;
}

// 0-5 (the index into the GENRE/DECADE/MIN RATING/RUNTIME/LANGUAGE/ORDER
// value list) for a Value row; -1 for every other row, including
// out-of-range ones. Decoupling the value index from the panel's row number
// is what lets MODE sit at the top without renumbering the value switch.
inline int filter_value_index(FilterTabKind tab, int row) {
    if (row < 0 || row >= filter_row_count(tab)) return -1;
    return (filter_row_role(tab, row) == FilterRowRole::Value) ? row - 1 : -1;
}

// View-level snapshot of the per-tab filter state. Consumers (BrowseScreen)
// own the storage in DisplaySettings; this struct is a transient view that
// the overlay reads from and writes back to on commit.
struct FilterState {
    uint32_t genre_mask = 0;
    int decade = 0;       // matches MbDecade enum value
    int min_rating = 0;   // matches MbMinRating enum value
    int runtime = 0;      // matches MbRuntime enum value
    int language = 0;     // matches MbLanguage enum value
    int sort = 0;         // matches MbDiscoverSort enum value
};

inline bool operator==(const FilterState& a, const FilterState& b) {
    return a.genre_mask == b.genre_mask && a.decade == b.decade &&
           a.min_rating == b.min_rating && a.runtime == b.runtime &&
           a.language == b.language && a.sort == b.sort;
}
inline bool operator!=(const FilterState& a, const FilterState& b) { return !(a == b); }

class FilterOverlay {
public:
    enum class State {
        Closed,
        SlidingIn,
        Open,
        SlidingOut,
    };

    FilterOverlay();

    // Begin opening the overlay. No-op if already open or sliding in.
    // `mode` is the caller's currently persisted Movies/TV mode; the MODE row
    // renders and toggles it.
    void open(FilterTabKind tab, MbMode mode, const FilterState& current);

    // Begin closing the overlay. No-op if already closed or sliding out.
    void close();

    bool is_visible() const { return state_ != State::Closed; }
    bool is_input_active() const { return state_ == State::Open; }

    // Per-frame animation tick. Call from screen's tick() / before render().
    void tick();

    // Input handlers. Return true if the input was consumed. Only accepted
    // while fully Open (is_input_active()) — on_rotate/on_select/
    // on_btn4_close all reject input during the SlidingIn/SlidingOut
    // animation windows.
    //   on_rotate: negative = up/prev, positive = down/next.
    //     Mode::RowSelect  → moves cursor through the tab's rows
    //       (filter_row_count()/filter_row_role() — Popular/TopRated:
    //       0 MODE, 1-6 value rows, 7 RESET ALL, 8 SHUFFLE; ForYou:
    //       0 MODE, 1 SHUFFLE).
    //     Mode::ValueSelect → cycles the focused row's value in working_.
    //   on_select: rotary press.
    //     RowRole::Value   → enter ValueSelect (no commit fired).
    //     RowRole::Reset   → reset working_ to defaults, stay in RowSelect
    //       (no commit fired).
    //     FilterRowRole::Mode → toggle Movies/TV via on_mode_toggle_, re-stage
    //       the new mode's persisted filters, stay Open (no commit fired).
    //     RowRole::Shuffle → fire the shuffle callback with the staged
    //       state, then close via the commit-free close() path (on_commit_
    //       is never touched).
    //     Mode::ValueSelect → save value to working_, exit back to RowSelect (no commit fired).
    //   on_btn4_close:
    //     Mode::ValueSelect → exit to RowSelect (no commit fired).
    //     Mode::RowSelect   → fire on_commit_ only when working_ differs
    //       from the open()-time snapshot, then close overlay either way.
    bool on_rotate(int delta);
    bool on_select();
    bool on_btn4_close();

    // Commits go through this callback. The caller's commit handler is
    // responsible for persisting the new state and triggering a refetch.
    // Fires at most once per overlay session, only when the staged state
    // differs from the open()-time snapshot.
    using CommitCallback = std::function<void(const FilterState&, FilterTabKind)>;
    void set_on_commit(CommitCallback cb) { on_commit_ = std::move(cb); }

    // SHUFFLE row callback (spec 1b). Fires synchronously from on_select()
    // while the overlay is still Open, with the staged working_ state; the
    // handler persists it and performs exactly one shuffle load. The
    // overlay then closes itself via the commit-free close() path — on_commit_
    // is never invoked for this session.
    using ShuffleCallback = std::function<void(const FilterState&, FilterTabKind)>;
    void set_on_shuffle(ShuffleCallback cb) { on_shuffle_ = std::move(cb); }

    // MODE row callback. Fires synchronously from on_select() while the
    // overlay stays OPEN. Contract, in order:
    //   1. the handler persists `staged` into the mode the overlay was
    //      showing (the mode this callback is REPLACING) so no edit is lost;
    //   2. it persists `new_mode` and reloads the active content tab under it;
    //   3. it returns the filter state persisted for `new_mode` + `tab`,
    //      which the overlay re-stages as both working_ and the open()-time
    //      snapshot — so the subsequent BTN4 commit only fires for edits made
    //      AFTER the swap, and never re-applies pre-swap edits to the wrong
    //      mode's storage.
    using ModeToggleCallback =
        std::function<FilterState(MbMode new_mode, FilterTabKind tab,
                                  const FilterState& staged)>;
    void set_on_mode_toggle(ModeToggleCallback cb) { on_mode_toggle_ = std::move(cb); }

    // Render the overlay panel. Caller is responsible for screen geometry.
    void render(::ui::Renderer& r, int screen_w, int screen_h);

    // Compute current panel left X (for slide animation).
    int compute_panel_left_x() const;

private:
    // Two-level navigation mode.
    enum class Mode { RowSelect, ValueSelect };

    State state_ = State::Closed;
    Mode  mode_  = Mode::RowSelect;
    std::chrono::steady_clock::time_point anim_started_at_;

    FilterTabKind tab_ = FilterTabKind::Popular;
    FilterState working_;       // staging area — edits accumulate here until BTN4 close
    int focus_row_ = 0;         // 0..row_count()-1

    CommitCallback on_commit_;

    // Row model lives in the free functions above (filter_row_count /
    // filter_row_role / filter_value_index) so it is unit-testable without a
    // Renderer. Kept as thin private wrappers only where the call sites read
    // better for it.
    bool has_filter_rows() const { return tab_ != FilterTabKind::ForYou; }
    int  row_count() const { return filter_row_count(tab_); }
    FilterRowRole role_for_row(int row) const { return filter_row_role(tab_, row); }
    FilterState opened_;          // snapshot at open() — commit skipped when unchanged
    ShuffleCallback on_shuffle_;
    ModeToggleCallback on_mode_toggle_;
    // The persisted Movies/TV mode as of open() or the last MODE toggle.
    // NOTE: named media_mode_, not mode_ — mode_ is the RowSelect/ValueSelect
    // navigation state.
    MbMode media_mode_ = MbMode::Movies;
    void render_shuffle_row(::ui::Renderer& r, int panel_x, int x, int y,
                            bool focused);
    void render_mode_row(::ui::Renderer& r, int panel_x, int x, int y,
                         bool focused);

    static constexpr int kSlideInMs  = 200;
    static constexpr int kSlideOutMs = 150;
    static constexpr int kPanelWidthPx = 380;

    // Cycle the focused row's value forward/backward in working_.
    // direction > 0 = forward, < 0 = backward.
    void cycle_focused_value(int direction);

    void render_single_row(::ui::Renderer& r, int panel_x, int x, int y,
                           const char* label, const char* value,
                           bool focused, bool editing);
    void render_reset_row(::ui::Renderer& r, int panel_x, int x, int y,
                          bool focused);
};

}  // namespace media_browser::ui
