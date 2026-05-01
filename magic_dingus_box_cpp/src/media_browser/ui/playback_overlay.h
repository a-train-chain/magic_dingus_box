#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "media_browser/tmdb_client.h"

namespace ui { class Renderer; }

namespace media_browser::ui {

// Lightweight metadata view for the overlay's detail header.
// Populated from the movie that is currently playing; used to drive the
// "NOW PLAYING" panel and kick off the similar-films pre-fetch.
struct PlaybackOverlayMovieMeta {
    std::string title;
    int year = 0;
    int runtime_min = 0;
    std::string genres;      // pre-formatted, e.g. "Action · Drama · Crime"
    std::string synopsis;    // truncation done at render time
    std::string poster_url;  // full URL passed to artwork cache
    int tmdb_id = 0;         // 0 if local file with no TMDB binding
    // Cast and director populated from TmdbMovieDetail when available.
    std::vector<std::string> cast;       // top 3-5 actor names
    std::string director;                // primary director name (first of directors list)
};

// Bottom-1/3 translucent overlay drawn on top of the playback video.
//
// Opening: rotary press while video is playing → overlay_.open().
// Closing:  BTN4 while overlay is open → overlay_.close() (BTN4 when
//           overlay is closed returns to Detail as before — not handled here).
// Scrolling: rotary twist while open scrolls the similar-films carousel.
// Quick-add: Task 9 will wire SELECT (rotary press while open) to add the
//            focused film to the Radarr queue. Leave a TODO marker here.
//
// Threading: start_prefetch() spawns a single background thread that calls
// TmdbClient::get_similar(). The result vector is protected by similar_mu_.
// FetchState is an atomic so the render path can poll it without the lock.
class PlaybackOverlay {
public:
    PlaybackOverlay();
    ~PlaybackOverlay();

    // Begin pre-fetching similar films in the background. Idempotent: calling
    // twice with the same tmdb_id is a no-op once the fetch has started.
    void start_prefetch(::media_browser::TmdbClient& tmdb,
                        const PlaybackOverlayMovieMeta& meta);

    // Cancel any in-flight pre-fetch (called on playback stop).
    void cancel_prefetch();

    bool is_open() const { return open_.load(); }
    void open();
    void close();

    // Input — returns true if consumed (only when overlay is open).
    bool on_rotate(int delta);
    int focused_index() const { return cursor_; }

    // Snapshot the currently-focused similar film.
    // Returns nullopt when the overlay is closed, the list is empty, or
    // the cursor is out of range.
    // Task 9 uses this for quick-add.
    std::optional<::media_browser::TmdbSearchHit> focused_film() const;

    void render(::ui::Renderer& r, int screen_w, int screen_h);

    // Show a small toast next to the focused poster for ~2 seconds.
    void show_toast(const std::string& msg);

private:
    std::atomic<bool> open_{false};
    std::atomic<bool> cancel_requested_{false};
    int cursor_ = 0;
    PlaybackOverlayMovieMeta meta_;

    mutable std::mutex similar_mu_;
    std::vector<::media_browser::TmdbSearchHit> similar_;

    enum class FetchState { Idle, InFlight, Loaded, Failed };
    std::atomic<FetchState> fetch_state_{FetchState::Idle};
    int prefetched_tmdb_id_ = 0;

    std::thread fetch_thread_;

    std::string toast_msg_;
    std::chrono::steady_clock::time_point toast_started_at_;
    bool toast_active_ = false;

    // One-shot diagnostic log: emits snapshot size the first time the overlay
    // renders after being opened. Reset to true each time open() is called.
    bool render_log_once_ = false;
};

}  // namespace media_browser::ui
