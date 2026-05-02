// Unit tests for media_browser::ArtworkCache.
//
// Compiled with -DARTWORK_CACHE_TEST_MODE which:
//   - skips the libcurl + background-thread work (no real HTTP in tests)
//   - skips GL texture upload calls (no EGL context in the test binary)
//   - exposes test_inject_ready_upload() / pump_for_tests() / test_touch()
//     so we can simulate a "download complete" and exercise the
//     bookkeeping path end-to-end.
//
// The full GL upload path is exercised only on a Pi with a live EGL
// context — accepted as integration testing.

#include <catch2/catch_test_macros.hpp>

#include "media_browser/artwork/artwork_cache.h"

#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>

using media_browser::ArtworkCache;

namespace {

// Build a fake pixel buffer of a given size so we can feed
// test_inject_ready_upload() without needing a real image.
ArtworkCache::TestPendingUpload make_upload(const std::string& url,
                                            int w, int h) {
    ArtworkCache::TestPendingUpload u;
    u.url = url;
    u.width = w;
    u.height = h;
    u.pixels_rgba.assign(static_cast<std::size_t>(w * h * 4), 0x55u);
    return u;
}

}  // namespace

TEST_CASE("ArtworkCache constructs and destructs cleanly", "[artwork]") {
    // Construction + destruction with no usage — in TEST_MODE the
    // background thread isn't spawned so this just exercises the
    // default state + join-safety code paths.
    ArtworkCache cache(16 * 1024);
    REQUIRE(cache.entries_count() == 0);
    REQUIRE(cache.bytes_in_use() == 0);
    REQUIRE(cache.bytes_waiting_upload() == 0);
}

TEST_CASE("get_or_fetch returns 0 for a brand-new URL", "[artwork]") {
    ArtworkCache cache;
    REQUIRE(cache.get_or_fetch("https://example.com/a.jpg") == 0);
    // Empty URL is also a 0 (but shouldn't enqueue anything).
    REQUIRE(cache.get_or_fetch("") == 0);
    REQUIRE(cache.entries_count() == 0);
}

TEST_CASE("Injected upload is promoted to an entry by pump_for_tests", "[artwork]") {
    ArtworkCache cache;
    const std::string url = "https://example.com/a.jpg";
    REQUIRE(cache.get_or_fetch(url) == 0);

    cache.test_inject_ready_upload(make_upload(url, 20, 30));
    REQUIRE(cache.bytes_waiting_upload() == 20u * 30u * 4u);

    std::size_t uploaded = cache.pump_for_tests();
    REQUIRE(uploaded == 1);
    REQUIRE(cache.entries_count() == 1);
    REQUIRE(cache.bytes_in_use() == 20u * 30u * 4u);
    REQUIRE(cache.bytes_waiting_upload() == 0);

    // Subsequent get_or_fetch returns the synthetic non-zero texture id.
    REQUIRE(cache.get_or_fetch(url) != 0);
}

TEST_CASE("Corrupt upload (empty pixels) is dropped by pump", "[artwork]") {
    ArtworkCache cache;
    ArtworkCache::TestPendingUpload bad;
    bad.url = "https://example.com/corrupt.jpg";
    bad.width = 0;
    bad.height = 0;
    // empty pixels_rgba
    cache.test_inject_ready_upload(std::move(bad));
    std::size_t uploaded = cache.pump_for_tests();
    REQUIRE(uploaded == 1);  // the pending item is processed...
    REQUIRE(cache.entries_count() == 0);  // ...but no entry is stored.
    REQUIRE(cache.bytes_in_use() == 0);
}

TEST_CASE("LRU eviction drops oldest entries when over budget", "[artwork]") {
    // Budget of 1KB. Each "poster" is 32x32x4 = 4096 bytes — exactly 4KB,
    // so inserting the 2nd one must evict the 1st. Adjust to a size
    // where exactly one poster fits at a time.
    const int dim = 16;  // 16*16*4 = 1024 bytes per entry.
    ArtworkCache cache(1024);

    cache.test_inject_ready_upload(make_upload("a", dim, dim));
    REQUIRE(cache.pump_for_tests() == 1);
    REQUIRE(cache.entries_count() == 1);
    REQUIRE(cache.bytes_in_use() == 1024);

    // A very short sleep ensures the second entry's last_access is
    // strictly later than the first's — the LRU picker uses <, so
    // equal-timestamp ties aren't guaranteed to evict `a` first.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));

    cache.test_inject_ready_upload(make_upload("b", dim, dim));
    REQUIRE(cache.pump_for_tests() == 1);

    // Still exactly one entry in the cache — "a" must have been evicted.
    REQUIRE(cache.entries_count() == 1);
    REQUIRE(cache.bytes_in_use() == 1024);
    REQUIRE(cache.get_or_fetch("a") == 0);  // evicted, not present
    REQUIRE(cache.get_or_fetch("b") != 0);  // still there
}

TEST_CASE("LRU: touching the older entry keeps it alive across a new insert",
          "[artwork]") {
    const int dim = 16;  // 1024 bytes each.
    // Budget holds 2 entries exactly. Inserting a 3rd evicts one.
    ArtworkCache cache(2048);

    cache.test_inject_ready_upload(make_upload("a", dim, dim));
    cache.test_inject_ready_upload(make_upload("b", dim, dim));
    cache.pump_for_tests();
    REQUIRE(cache.entries_count() == 2);

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    // Touching "a" promotes it to MRU so "b" becomes the oldest.
    cache.test_touch("a");

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    cache.test_inject_ready_upload(make_upload("c", dim, dim));
    cache.pump_for_tests();

    REQUIRE(cache.entries_count() == 2);
    REQUIRE(cache.get_or_fetch("a") != 0);  // alive
    REQUIRE(cache.get_or_fetch("b") == 0);  // evicted
    REQUIRE(cache.get_or_fetch("c") != 0);  // alive
}

TEST_CASE("Pause/resume is idempotent and does not throw", "[artwork][pause]") {
    ArtworkCache c;
    REQUIRE_FALSE(c.is_paused());
    c.pause();
    REQUIRE(c.is_paused());
    c.pause();  // double-pause is a no-op
    REQUIRE(c.is_paused());
    c.resume();
    REQUIRE_FALSE(c.is_paused());
    c.resume();  // double-resume is a no-op
    REQUIRE_FALSE(c.is_paused());
}

TEST_CASE("Disk cache stats start at zero", "[artwork][disk]") {
    // Constructed without a disk_cache_dir — disk cache disabled,
    // counters should remain at zero.
    ArtworkCache c;
    REQUIRE(c.disk_cache_hits()   == 0);
    REQUIRE(c.disk_cache_misses() == 0);
    REQUIRE(c.disk_cache_writes() == 0);
}

TEST_CASE("Disk cache dir is created at construction", "[artwork][disk]") {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() /
                         ("mdb_artwork_test_" + std::to_string(std::rand()));
    fs::remove_all(tmp);
    REQUIRE_FALSE(fs::exists(tmp));
    {
        ArtworkCache c(64u * 1024u * 1024u, tmp.string());
        REQUIRE(fs::exists(tmp));
        REQUIRE(fs::is_directory(tmp));
    }
    fs::remove_all(tmp);
}

TEST_CASE("Repeated get_or_fetch for in-flight URL does not re-enqueue",
          "[artwork]") {
    // Can't directly inspect the work queue from outside, but the
    // contract here is just "no crash / no double-fetch observable".
    // With TEST_MODE the background thread never runs, so the URL
    // stays "in-flight" forever until an inject+pump simulates the
    // fetcher.
    ArtworkCache cache;
    const std::string url = "https://example.com/dupe.jpg";
    for (int i = 0; i < 10; ++i) {
        REQUIRE(cache.get_or_fetch(url) == 0);
    }
    cache.test_inject_ready_upload(make_upload(url, 4, 4));
    REQUIRE(cache.pump_for_tests() == 1);
    REQUIRE(cache.entries_count() == 1);
    // Now cached — further calls must return the texture id, not 0.
    for (int i = 0; i < 10; ++i) {
        REQUIRE(cache.get_or_fetch(url) != 0);
    }
}
