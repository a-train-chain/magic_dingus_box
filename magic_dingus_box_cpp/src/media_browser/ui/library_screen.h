#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include "app/app_state.h"
#include "media_browser/media_ref.h"
#include "media_browser/radarr/radarr_types.h"
#include "media_browser/sonarr/sonarr_types.h"
#include "media_browser/ui/library_view.h"
#include "media_browser/ui/mb_screen.h"

namespace media_browser {
class RadarrClient;
class SonarrClient;
}
namespace media_browser::library { class WatchStore; }

namespace media_browser::ui {

// Task 22: the Library screen — replaces the Task 17 stub.
// Phase 3 Task 7: mixed movie+TV. One grid holds every Radarr movie plus
// every Sonarr series that has files (or an active download — the inclusion
// rule lives in library_view.h's build_library_entries), keyed by MediaRef
// throughout because the TMDB movie/TV id spaces overlap completely. The
// Unwatched filter is real now, fed by WatchStore at apply_pending() time.
//
// Layout:
//   - Top bar (~56px): "Library" title + "(N movies)" count on the left,
//     filter chip row on the right (All / Unwatched / Missing Upgrades /
//     Recent). Active chip is drawn in the theme accent color; others are
//     dimmed. The focused chip (when focus_ == FilterStrip) gets a small
//     underline.
//   - Main area: 4-column poster grid. Each cell has a deterministic
//     colored-quad placeholder (like BrowseScreen), title + year below, and
//     a colored corner dot indicating file state:
//       * green  (highlight1):  has_file = true, quality looks like Bluray
//                               or WEB-DL 1080p+ ("best" state)
//       * orange (highlight3):  has_file = true but not 1080p+/Bluray —
//                               upgrade available (approximation: filters
//                               against a prefix match on "Bluray" / "WEBDL-1080p")
//       * red    (highlight2):  monitored but has_file = false — no file yet
//     The focused cell gets an accent-colored outline.
//   - Bottom bar (~40px): "Select: Open details   LEFT/RIGHT: Filter   "
//     BTN2 (red): back to Browse. BTN4 (black): opens the Library overlay
//     (sort + filter + stats — wired in Task 5 of the v1.6.x library-overlay plan).
//
// Navigation:
//   - LEFT / RIGHT (ROTATE) at top-strip level switches filter chip; in the
//     grid it steps horizontally through cells.
//   - UP from grid row 0 jumps focus to the filter strip.
//   - DOWN from strip jumps focus back to grid row 0.
//   - UP / DOWN (ROTATE_VERTICAL) inside the grid walks rows.
//   - SELECT / ROTARY_CLICK on a grid cell stores that entry's MediaRef and
//     transitions to Screen::Detail (movies) or Screen::SeriesDetail (TV) —
//     the KIND rides in the returned Screen value, mirroring BrowseScreen;
//     the dispatcher reads selected_ref() for the id.
//   - BTN2 (PLAY_PAUSE): returns to Screen::Browse. BTN4 (SETTINGS_MENU)
//     short-press: opens the Library slide-in overlay (Task 5 of v1.6.x);
//     long-press still exits MB → MainMenu.
class LibraryScreen : public MbScreen {
public:
    // `sonarr` is nullable, QueueScreen-precedent: a box with no Sonarr API
    // key holds a SonarrMockClient whose fixture library would render
    // phantom TV tiles on every keyless unit — main.cpp passes nullptr in
    // that case and this screen stays movie-only. `watch_store` is nullable
    // for the same defensive reason (a failed open degrades to ok()==false
    // anyway); null or degraded simply means nothing reads as watched.
    LibraryScreen(RadarrClient& radarr, SonarrClient* sonarr,
                  library::WatchStore* watch_store, ::app::AppState& state);
    ~LibraryScreen();  // joins the async refresh worker

    void enter() override;
    void update() override;  // drains async refresh + re-arms the poll cadence
    Screen handle_input(const std::vector<platform::InputEvent>& events) override;
    void render(::ui::Renderer& r, int screen_w, int screen_h) override;

    // MediaRef of the poster most recently selected by the user. Consumed
    // by the dispatcher in main.cpp on transition: Detail takes ref.id when
    // kind == Movie, SeriesDetail takes it when kind == Tv (the kind also
    // rides in the Screen value handle_input returned, Browse-style).
    MediaRef selected_ref() const { return selected_ref_; }

private:
    enum class Filter {
        All = 0,
        Unwatched = 1,       // MVP: alias for has_file == true (no watch tracking yet).
        MissingUpgrades = 2, // has_file == true but quality not 1080p-class.
        Recent = 3,          // All, sorted by added_at descending.
    };
    enum class Focus { FilterStrip, PosterGrid };

    static constexpr int kNumFilters = 4;
    // 9-column poster grid — matches BrowseScreen's density so the two
    // screens feel like the same UI. Was 5 columns originally, which
    // looked correct in isolation but visibly didn't match Browse when
    // operators flipped between the two. With 9 columns + 2 visible
    // rows, you see 18 posters at once, same as Browse. On a 1280-wide
    // target with kPaddingX=32 and kCellGapX=20, each cell ends up
    // ~119px wide (poster ~119x178 at 2:3) — small but still legible
    // as artwork.
    static constexpr int kGridCols = 9;

    // Categorises a library entry into the three dot-color buckets used
    // by the grid state indicator and the MissingUpgrades filter logic.
    enum class FileState {
        HasGoodFile,        // green   — 1080p-class file present
        UpgradeAvailable,   // orange  — has file but lower quality
        MissingFile,        // red     — monitored, no file yet
    };

    // Async refresh (mirrors QueueScreen). enter() and the periodic timer
    // call refresh_async() which spawns run_refresh() on a worker thread;
    // apply_pending() drains its result on the render thread and calls
    // rebuild_view(). reload() is retained as a thin synchronous wrapper
    // (run_refresh into pending + immediate apply) for any caller that
    // needs blocking semantics, but the live path is fully async so the
    // render thread never stalls on Radarr HTTP.
    void refresh_async();                 // non-blocking; spawns worker
    void run_refresh();                   // worker body (off render thread)
    void apply_pending();                 // drain on render thread
    void rebuild_view();                  // Apply current filter + sort to library_.
    static FileState classify(const Movie& m);
    static bool is_1080p_quality(const std::string& q);

    RadarrClient& radarr_;
    // Nullable — see the constructor comment. When null, run_refresh()
    // skips the Sonarr fetches entirely and sonarr_ok stays true (an empty
    // TV library is the genuine answer, not an outage).
    SonarrClient* sonarr_ = nullptr;
    // Nullable / possibly-degraded. MAIN-THREAD-ONLY by the store's own
    // contract: read exclusively in apply_pending() on the render thread,
    // never in run_refresh()'s worker.
    library::WatchStore* watch_store_ = nullptr;
    ::app::AppState& state_;

    Filter filter_ = Filter::All;
    Focus focus_ = Focus::PosterGrid;
    int filter_cursor_ = 0;   // Index into the top strip when Focus::FilterStrip.
    int grid_cursor_ = 0;     // Flat index into view_ when Focus::PosterGrid.
    int scroll_row_ = 0;      // Topmost visible row index.

    std::vector<Movie> library_;          // Movie library, as returned by Radarr.
    std::vector<Series> tv_library_;      // TV library, as returned by Sonarr.
    // Mixed entries built over library_/tv_library_ (borrow their storage),
    // and the filter-applied, possibly-sorted view of pointers into
    // entries_. All four containers are swapped/rebuilt together on the
    // render thread in apply_pending() — see rebuild_view()'s aliasing note.
    std::vector<LibraryEntry> entries_;
    std::vector<const LibraryEntry*> view_;
    bool loaded_ = false;
    // False when Sonarr was configured but did not answer the last
    // library fetch (checked-variant nullopt). TV entries are simply
    // absent that cycle; render() surfaces the queue_screen-precedent
    // "Sonarr offline" warning line so the gap is explained rather than
    // silently reading as "your shows vanished".
    bool sonarr_ok_ = true;
    // Latch so the "could not format the 30-day cutoff" warning logs once
    // rather than on every rebuild_view() — which runs ~every 2s while this
    // screen is open. See rebuild_view().
    bool warned_recent_cutoff_ = false;

    // Refs currently downloading: movies in the Radarr queue plus series
    // with rows in the Sonarr queue. MediaRef-keyed (never bare tmdb ints)
    // because the movie/TV id spaces overlap completely. Drives the
    // DOWNLOADING badge on poster cards (takes precedence over the
    // always-true in_library flag) and feeds build_library_entries'
    // TV inclusion rule.
    std::unordered_set<MediaRef> downloading_refs_;

    // Movie refs whose queue item is sitting in
    // trackedDownloadState=importBlocked — i.e. the torrent finished but
    // Radarr couldn't import a video file (typical bait release: trailer-
    // only torrents). Surfaces as a 'BAD RELEASE' badge instead of the
    // misleading 'DOWNLOADING' chip. (Movie-kind refs only today — the TV
    // queue mapping folds everything into downloading_refs_.)
    std::unordered_set<MediaRef> stuck_refs_;

    // Movie refs whose download finished and Radarr is copying the file
    // into the library (tracked_download_state importing/importPending).
    // Shows an IMPORTING badge (in-progress success color), distinct from
    // the red DOWNLOADING/BAD RELEASE chips. Transient — usually visible
    // for only a refresh cycle or two before the file lands and the badge
    // clears to plain IN LIBRARY.
    std::unordered_set<MediaRef> importing_refs_;

    MediaRef selected_ref_{};

    // --- Async refresh state (mirrors QueueScreen) ----------------------
    // run_refresh() builds a PendingResult on a worker thread; apply_pending()
    // swaps it into the live members on the render thread. Only pending_
    // needs the mutex — library_/view_/the id-sets are read by render() and
    // written by apply_pending(), both on the render thread.
    struct PendingResult {
        std::vector<Movie>  library;
        std::vector<Series> tv_library;   // empty when sonarr_ null or down
        std::unordered_set<MediaRef> downloading;  // movie + tv refs
        std::unordered_set<MediaRef> stuck;
        std::unordered_set<MediaRef> importing;
        bool sonarr_ok = false;
    };
    std::mutex            pending_mtx_;
    PendingResult         pending_;
    std::atomic<bool>     result_ready_{false};
    std::atomic<bool>     refresh_in_flight_{false};
    std::thread           refresh_worker_;
    std::chrono::steady_clock::time_point last_refresh_at_{};
    static constexpr int  kRefreshIntervalMs = 2000;  // library churns slowly

    // Header stats line cache ("N titles · X GB used · Y GB free").
    // Rebuilt at most every 5s in render() — computing it per frame cost a
    // statvfs syscall + O(library) sum + string concats at 60fps.
    std::string stats_line_;
    std::chrono::steady_clock::time_point last_stats_refresh_{};

    // Slide-in overlay state machine (v1.6.x). The overlay is a 480 px
    // panel that slides in from the right edge on BTN4 press, holding
    // stats + sort + filter controls. Closed = no overlay rendered;
    // SlidingIn = animating from x=1280 → x=760 (200 ms ease-out);
    // Open = stationary; SlidingOut = animating from x=760 → x=1280
    // (150 ms ease-in). Input is captured by the panel during
    // SlidingIn / Open / SlidingOut.
    enum class OverlayState {
        Closed     = 0,
        SlidingIn  = 1,
        Open       = 2,
        SlidingOut = 3,
    };
    OverlayState overlay_state_ = OverlayState::Closed;

    // Animation start time for the current slide. Used to compute the
    // panel's current x-position via ease curves in render().
    std::chrono::steady_clock::time_point overlay_anim_started_at_{};

    // Cursor position inside the panel. The 8 focusable rows are
    // indexed 0-7: Sort (Recent, Title, Year, Size) at 0-3, Filter
    // (All, Unwatched, MissingFiles, RecentlyAdded) at 4-7.
    int overlay_focus_row_ = 0;

    // Number of focusable rows in the panel — kept as a constant so
    // render and input-handling stay in lockstep.
    static constexpr int kOverlayFocusableRows = 8;

    // Slide-in / slide-out animation durations in ms. Used by both
    // tick_overlay_animation() (state-machine promotion) and render()
    // (panel x-position computation). Keep them adjacent here so a
    // future tweak to one location can't drift from the other.
    static constexpr int kOverlaySlideInMs  = 200;
    static constexpr int kOverlaySlideOutMs = 150;

    // Open / close transitions. start_open_overlay() snaps to
    // SlidingIn, sets the cursor to whichever row matches the
    // currently-active sort, and timestamps the animation start.
    // start_close_overlay() snaps to SlidingOut and timestamps;
    // tick_overlay_animation() promotes SlidingIn → Open and
    // SlidingOut → Closed when the animation duration has elapsed.
    void start_open_overlay();
    void start_close_overlay();
    void tick_overlay_animation();

    // Compute the panel's current left x-coord based on overlay state +
    // elapsed animation time. Returns kOverlayPanelClosedX when the
    // panel is in Closed state (rendered off-screen). Used by render()
    // to animate the panel's slide.
    static int compute_overlay_left_x(OverlayState state,
                                      std::chrono::steady_clock::time_point anim_started);
};

}  // namespace media_browser::ui
