#pragma once

// Async poster/artwork cache for the Media Browser. Separates:
//   - a background thread that downloads + decodes image bytes over HTTP
//     via libcurl + stb_image
//   - a main-thread pump() that consumes completed decodes and uploads
//     them to OpenGL textures (GL calls only legal on the thread that
//     owns the EGL context)
//
// Callers should drive the cache by calling get_or_fetch(url) from the
// renderer each frame. On the first call for a URL the cache enqueues
// a background fetch and returns 0. Once the background thread finishes
// the download + decode, pump() uploads the pixels to a GL texture on
// the next frame, and subsequent get_or_fetch() calls return the
// non-zero texture id.
//
// LRU eviction: if total bytes_in_use() exceeds max_bytes_ after an
// upload, the least-recently-accessed textures are dropped until we are
// back under budget.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace media_browser {

class ArtworkCache {
public:
    // Pixel dimensions of a cached, uploaded texture. Returned by get_dims()
    // so the renderer can preserve native aspect ratio when fitting an image
    // into a slot of arbitrary aspect.
    struct TextureDims {
        int w = 0;
        int h = 0;
    };

    // max_bytes: LRU eviction threshold; default 256MB (conservative on
    // the Pi 4B with 2GB RAM — TMDB w500 posters are ~50-150KB each so
    // 256MB holds ~2-3k posters uncompressed in GPU memory).
    //
    // disk_cache_dir: optional path to a directory holding raw JPEG bytes
    // keyed by hashed URL. When set, the fetcher thread reads from disk
    // first before going to the network — turning a 7-15s TMDB round-trip
    // into a sub-millisecond local read for any URL the kiosk has seen
    // before. Empty string = disk caching disabled (network-only path,
    // identical to the original behavior). The directory is created on
    // construction if missing; if creation fails the cache silently falls
    // back to network-only.
    explicit ArtworkCache(std::size_t max_bytes = 256u * 1024u * 1024u,
                          std::string disk_cache_dir = "");
    ~ArtworkCache();

    ArtworkCache(const ArtworkCache&) = delete;
    ArtworkCache& operator=(const ArtworkCache&) = delete;

    // Main thread: request a texture for this URL. Returns 0 if the
    // texture is not yet uploaded; the caller should draw a placeholder
    // tint. Auto-enqueues a fetch on first call for a given URL. Safe to
    // call every frame for the same URL — it's cheap (hash-map lookup +
    // timestamp bump) once the URL is known.
    std::uint32_t get_or_fetch(const std::string& url);

    // Main thread only: called once per frame. Drains the "ready uploads"
    // queue filled by the background thread and performs the GL upload
    // (glGenTextures / glBindTexture / glTexImage2D / glGenerateMipmap).
    // Returns the number of textures uploaded this frame.
    std::size_t pump();

    // Main thread only: returns the pixel dimensions of the cached texture
    // for `url`, or std::nullopt if the URL is not yet uploaded. Used by
    // the renderer to preserve aspect ratio when letterboxing/pillarboxing
    // an image into an arbitrary slot.
    std::optional<TextureDims> get_dims(const std::string& url) const;

    // Pause/resume the background fetcher. Call pause() before video
    // playback starts to prevent the artwork worker from contending with
    // GStreamer for I/O bandwidth on the same drive that holds the
    // movie file. resume() restarts the worker. Mirrors the existing
    // qBittorrent pause_all() pattern. Both are idempotent.
    void pause();
    void resume();
    bool is_paused() const { return paused_.load(std::memory_order_acquire); }

    // Main thread only (owns the GL context). Deletes every uploaded
    // texture and discards queued-but-not-yet-uploaded pixel buffers,
    // including their in-flight dedup markers so the posters can
    // re-fetch later. Called before the DRM handoff to RetroArch so up
    // to max_bytes_ of poster textures don't sit in RAM/GPU memory for
    // the whole game session. Entries rebuild lazily via get_or_fetch()
    // on return — the disk cache makes that a local re-read, not a TMDB
    // round-trip.
    void clear_textures();

    // Diagnostics
    std::size_t entries_count() const;
    std::size_t bytes_in_use() const;
    std::size_t bytes_waiting_upload() const;
    std::size_t disk_cache_hits() const   { return disk_hits_.load(); }
    std::size_t disk_cache_misses() const { return disk_misses_.load(); }
    std::size_t disk_cache_writes() const { return disk_writes_.load(); }
    std::size_t disk_cache_bytes() const  { return disk_bytes_in_use_.load(); }
    std::size_t disk_cache_evictions() const { return disk_evictions_.load(); }
    std::size_t dead_url_skips() const    { return dead_url_skips_.load(); }

    // --- Test-only hooks (no GL). Lets the unit test simulate a
    // completed fetch by directly injecting a decoded pixel buffer, and
    // verify book-keeping without needing a real EGL context. ---
#ifdef ARTWORK_CACHE_TEST_MODE
    struct TestPendingUpload {
        std::string url;
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> pixels_rgba;
    };
    // Simulate a completed background fetch. The next pump_for_tests()
    // will process it using the same bookkeeping path as production.
    void test_inject_ready_upload(TestPendingUpload upload);
    // Like pump() but skips all GL calls — only updates the entries map
    // / LRU timestamps / byte counters.
    std::size_t pump_for_tests();
    // Manually mark a URL as accessed (for LRU testing).
    void test_touch(const std::string& url);
    // True if url currently has an in-flight fetch marker (dedup guard).
    bool test_is_in_flight(const std::string& url);
#endif

private:
    // What the fetcher thread pushes onto the "ready" queue when a
    // download + decode succeeds.
    struct PendingUpload {
        std::string url;
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> pixels_rgba;  // always 4-channel (RGBA8)
    };

    // One texture cached in GL. bytes tracks pixel byte size (w*h*4) so
    // LRU eviction can hit a byte budget. texture_id == 0 in test mode.
    struct Entry {
        std::uint32_t texture_id = 0;
        std::size_t bytes = 0;
        int width = 0;          // pixel width — used by get_dims() for aspect-fit
        int height = 0;         // pixel height
        std::chrono::steady_clock::time_point last_access;
    };

    // Background thread entry point.
    void fetcher_thread_main();

    // Evict LRU entries until bytes_in_use_ <= max_bytes_. Must be
    // called with entries_mutex_ held.
    void evict_lru_locked();

    // Actual GL upload of a pending item. Returns the new texture_id
    // and its byte size via out params. In TEST_MODE the GL portion is
    // skipped.
    void upload_one(PendingUpload&& p);

    // Map a URL to its on-disk cache filename. Returns empty when disk
    // caching is disabled. Uses std::hash<string> -> hex; collisions
    // are handled gracefully (cache miss falls back to network).
    std::string disk_cache_path(const std::string& url) const;

    // Read decoded RGBA pixels from the on-disk JPEG. Returns true on
    // hit. Out-params populated only on hit. Worker thread only.
    bool try_load_from_disk(const std::string& url,
                            int& w, int& h,
                            std::vector<std::uint8_t>& pixels_rgba);

    // Write the raw JPEG body to the on-disk cache (best-effort —
    // logged on failure but never throws). Worker thread only.
    void write_to_disk(const std::string& url,
                       const std::string& body);

    // Walk disk_cache_dir_ and tally total bytes into disk_bytes_in_use_.
    // Called once at construction so disk-LRU starts from a correct
    // baseline rather than only counting writes from this session.
    // Worker thread only (not yet started at construction time, so the
    // call is ordered safely on the constructing thread).
    void disk_recount_bytes();

    // If disk_bytes_in_use_ exceeds max_disk_bytes_, delete the oldest
    // (by mtime) cache files until we're under budget. Worker thread
    // only — production calls this from write_to_disk after a successful
    // write. Best-effort: filesystem errors are logged but not fatal.
    void disk_evict_lru_if_needed();

    // --- Main-thread owned state ---
    std::size_t max_bytes_;
    std::string disk_cache_dir_;        // empty == disk caching disabled

    // --- Disk LRU cap ---
    // Hard ceiling on total bytes the on-disk poster cache can occupy.
    // Default 2 GB — comfortable on a 16+ GB SD card while holding tens
    // of thousands of TMDB posters (avg ~100 KB each). Tunable via the
    // constructor for tests; not currently surfaced to callers.
    static constexpr std::size_t kDefaultMaxDiskBytes = 2ull * 1024ull * 1024ull * 1024ull;
    std::size_t max_disk_bytes_ = kDefaultMaxDiskBytes;
    std::atomic<std::size_t> disk_bytes_in_use_{0};
    std::atomic<std::size_t> disk_evictions_{0};

    // --- Pause/resume (UI thread sets, worker thread observes) ---
    std::atomic<bool>          paused_{false};
    std::mutex                 pause_mutex_;
    std::condition_variable    pause_cv_;

    // --- Disk cache stats (atomics — read from any thread) ---
    std::atomic<std::size_t> disk_hits_{0};
    std::atomic<std::size_t> disk_misses_{0};
    std::atomic<std::size_t> disk_writes_{0};

    // --- Poison-pill for chronically-broken URLs ---
    // After kMaxDecodeFailures consecutive decode failures for the same
    // URL, mark it permanently dead so we stop hammering TMDB and the
    // worker thread. Cleared on process restart (a fresh boot retries
    // dead URLs in case the upstream content was fixed).
    static constexpr int kMaxDecodeFailures = 3;
    std::unordered_map<std::string, int> failure_counts_;  // url -> count, guarded by entries_mutex_
    std::unordered_set<std::string> dead_urls_;            // guarded by entries_mutex_
    std::atomic<std::size_t> dead_url_skips_{0};           // diagnostic counter
    mutable std::mutex entries_mutex_;
    std::unordered_map<std::string, Entry> entries_;
    std::size_t bytes_in_use_ = 0;  // guarded by entries_mutex_

    // --- Work queue: main thread pushes URLs, background thread pops ---
    std::mutex work_mutex_;
    std::condition_variable work_cv_;
    std::deque<std::string> work_queue_;       // URLs to download
    std::unordered_set<std::string> in_flight_;  // URLs already enqueued
                                                 // (dedup guard)

    // --- Ready queue: background thread pushes, main thread (pump) pops ---
    std::mutex ready_mutex_;
    std::vector<PendingUpload> ready_uploads_;
    std::atomic<std::size_t> bytes_waiting_upload_{0};

    // --- Thread lifecycle ---
    std::atomic<bool> stop_{false};
    std::thread fetcher_thread_;
};

}  // namespace media_browser
