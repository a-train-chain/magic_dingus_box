#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ui { class Renderer; }

namespace media_browser::ui {

// Identifies which underlying tab's filter state this overlay edits.
enum class FilterTabKind {
    Popular,
    TopRated,
    ForYou,   // SHUFFLE-only overlay in Phase 1 (spec 1c)
};

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

// Returns TMDB genre IDs in bit-position order. Index i corresponds to
// bit i in FilterState::genre_mask. Used by callers to translate the mask
// into a TMDB DiscoverFilter::genre_ids vector.
const std::vector<int>& filter_overlay_genre_ids();

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
    void open(FilterTabKind tab, const FilterState& current);

    // Begin closing the overlay. No-op if already closed or sliding out.
    void close();

    bool is_visible() const { return state_ != State::Closed; }
    bool is_input_active() const { return state_ == State::Open; }

    // Per-frame animation tick. Call from screen's tick() / before render().
    void tick();

    // Input handlers. Return true if the input was consumed.
    //   on_rotate: negative = up/prev, positive = down/next.
    //     Mode::RowSelect  → moves cursor through filter rows.
    //     Mode::ValueSelect → cycles the focused row's value in working_.
    //   on_select: rotary press.
    //     Mode::RowSelect + value row → enter ValueSelect (no commit fired).
    //     Mode::RowSelect + RESET ALL → reset working_ to defaults (no commit fired).
    //     Mode::ValueSelect → save value to working_, exit back to RowSelect (no commit fired).
    //   on_btn4_close:
    //     Mode::ValueSelect → exit to RowSelect (no commit fired).
    //     Mode::RowSelect   → fire commit callback ONCE, then close overlay.
    bool on_rotate(int delta);
    bool on_select();
    bool on_btn4_close();

    // Commits go through this callback. The caller's commit handler is
    // responsible for persisting the new state and triggering a refetch.
    // Fires exactly ONCE per overlay session (on BTN4 close from RowSelect).
    using CommitCallback = std::function<void(const FilterState&, FilterTabKind)>;
    void set_on_commit(CommitCallback cb) { on_commit_ = std::move(cb); }

    // SHUFFLE row callback (spec 1b). The overlay closes itself via the
    // commit-free close() before firing; the handler persists any staged
    // edits and performs exactly one shuffle load.
    using ShuffleCallback = std::function<void(const FilterState&, FilterTabKind)>;
    void set_on_shuffle(ShuffleCallback cb) { on_shuffle_ = std::move(cb); }

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

    // Per-tab row model (spec 1b): the row count and roles vary by tab kind.
    // Popular/TopRated: rows 0-5 = filter/sort values, 6 = RESET ALL,
    // 7 = SHUFFLE. ForYou: single SHUFFLE row.
    enum class RowRole { Value, Reset, Shuffle };
    bool has_filter_rows() const { return tab_ != FilterTabKind::ForYou; }
    int  row_count() const { return has_filter_rows() ? 8 : 1; }
    RowRole role_for_row(int row) const {
        if (!has_filter_rows()) return RowRole::Shuffle;
        if (row == 6) return RowRole::Reset;
        if (row == 7) return RowRole::Shuffle;
        return RowRole::Value;
    }
    FilterState opened_;          // snapshot at open() — commit skipped when unchanged
    ShuffleCallback on_shuffle_;
    void render_shuffle_row(::ui::Renderer& r, int panel_x, int x, int y,
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
