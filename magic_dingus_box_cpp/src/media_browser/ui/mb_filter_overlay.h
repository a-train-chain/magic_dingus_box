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
    bool on_rotate(int delta);    // negative = up/prev, positive = down/next
    bool on_select();              // rotary press → cycle current row's value, fire commit
    bool on_btn4_close();          // BTN4 → start close animation

    // Commits go through this callback. The caller's commit handler is
    // responsible for persisting the new state and triggering a refetch.
    using CommitCallback = std::function<void(const FilterState&, FilterTabKind)>;
    void set_on_commit(CommitCallback cb) { on_commit_ = std::move(cb); }

    // Render the overlay panel. Caller is responsible for screen geometry.
    void render(::ui::Renderer& r, int screen_w, int screen_h);

    // Compute current panel left X (for slide animation).
    int compute_panel_left_x() const;

private:
    State state_ = State::Closed;
    std::chrono::steady_clock::time_point anim_started_at_;

    FilterTabKind tab_ = FilterTabKind::Popular;
    FilterState working_;       // edits in flight
    int focus_row_ = 0;         // 0..kFocusableRowCount-1

    CommitCallback on_commit_;

    static constexpr int kFocusableRowCount = 7;  // 6 filters + Reset
    static constexpr int kSlideInMs = 200;
    static constexpr int kSlideOutMs = 150;
    static constexpr int kPanelWidthPx = 380;

    void render_genre_row(::ui::Renderer& r, int x, int y);
    void render_value_row(::ui::Renderer& r, int x, int y, const std::string& label,
                          const std::vector<std::string>& values, int selected_index);
    void render_reset_row(::ui::Renderer& r, int x, int y);
};

}  // namespace media_browser::ui
