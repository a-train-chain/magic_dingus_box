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
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace media_browser {

class ArtworkCache {
public:
    // max_bytes: LRU eviction threshold; default 256MB (conservative on
    // the Pi 4B with 2GB RAM — TMDB w500 posters are ~50-150KB each so
    // 256MB holds ~2-3k posters uncompressed in GPU memory).
    explicit ArtworkCache(std::size_t max_bytes = 256u * 1024u * 1024u);
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

    // Diagnostics
    std::size_t entries_count() const;
    std::size_t bytes_in_use() const;
    std::size_t bytes_waiting_upload() const;

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

    // --- Main-thread owned state ---
    std::size_t max_bytes_;
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
