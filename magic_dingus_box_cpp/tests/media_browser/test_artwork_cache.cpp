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
#include "media_browser/artwork/artwork_sizing.h"

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
    REQUIRE(cache.bytes_in_use() == ArtworkCache::texture_bytes(20, 30));
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
    const int dim = 16;  // 16*16*4 = 1024 base bytes; 1364 with mips.
    const std::size_t one = ArtworkCache::texture_bytes(dim, dim);
    ArtworkCache cache(one);

    cache.test_inject_ready_upload(make_upload("a", dim, dim));
    REQUIRE(cache.pump_for_tests() == 1);
    REQUIRE(cache.entries_count() == 1);
    REQUIRE(cache.bytes_in_use() == one);

    // A very short sleep ensures the second entry's last_access is
    // strictly later than the first's — the LRU picker uses <, so
    // equal-timestamp ties aren't guaranteed to evict `a` first.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));

    cache.test_inject_ready_upload(make_upload("b", dim, dim));
    REQUIRE(cache.pump_for_tests() == 1);

    // Still exactly one entry in the cache — "a" must have been evicted.
    REQUIRE(cache.entries_count() == 1);
    REQUIRE(cache.bytes_in_use() == one);
    REQUIRE(cache.get_or_fetch("a") == 0);  // evicted, not present
    REQUIRE(cache.get_or_fetch("b") != 0);  // still there
}

TEST_CASE("LRU: touching the older entry keeps it alive across a new insert",
          "[artwork]") {
    const int dim = 16;
    // Budget holds 2 entries exactly. Inserting a 3rd evicts one.
    ArtworkCache cache(2 * ArtworkCache::texture_bytes(dim, dim));

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

TEST_CASE("clear_textures drops entries, queued uploads, and their "
          "in-flight markers", "[artwork]") {
    ArtworkCache cache;

    // One fully-uploaded entry.
    REQUIRE(cache.get_or_fetch("https://example.com/p1.jpg") == 0);
    cache.test_inject_ready_upload(make_upload("https://example.com/p1.jpg", 8, 8));
    REQUIRE(cache.pump_for_tests() == 1);
    REQUIRE(cache.entries_count() == 1);
    REQUIRE(cache.bytes_in_use() > 0);

    // One upload left waiting (not pumped). Its in-flight marker is only
    // dropped at upload time, so clear_textures() must erase it too —
    // otherwise get_or_fetch() would never re-enqueue this poster.
    REQUIRE(cache.get_or_fetch("https://example.com/p2.jpg") == 0);
    cache.test_inject_ready_upload(make_upload("https://example.com/p2.jpg", 8, 8));
    REQUIRE(cache.bytes_waiting_upload() > 0);
    REQUIRE(cache.test_is_in_flight("https://example.com/p2.jpg"));

    cache.clear_textures();

    REQUIRE(cache.entries_count() == 0);
    REQUIRE(cache.bytes_in_use() == 0);
    REQUIRE(cache.bytes_waiting_upload() == 0);
    REQUIRE(cache.pump_for_tests() == 0);
    REQUIRE_FALSE(cache.test_is_in_flight("https://example.com/p2.jpg"));

    // The cache must keep working after a clear.
    REQUIRE(cache.get_or_fetch("https://example.com/p3.jpg") == 0);
    cache.test_inject_ready_upload(make_upload("https://example.com/p3.jpg", 8, 8));
    REQUIRE(cache.pump_for_tests() == 1);
    REQUIRE(cache.entries_count() == 1);
}

TEST_CASE("texture_bytes counts the full mipmap chain", "[artwork]") {
    // 16x16: 1024 + 256 + 64 + 16 + 4 = 1364 (~+33%).
    REQUIRE(ArtworkCache::texture_bytes(16, 16) == 1364u);
    // Non-square: 4x1 -> 4x1, 2x1, 1x1 = (4+2+1)*4.
    REQUIRE(ArtworkCache::texture_bytes(4, 1) == 28u);
    REQUIRE(ArtworkCache::texture_bytes(1, 1) == 4u);
    REQUIRE(ArtworkCache::texture_bytes(0, 10) == 0u);
    // A w500 poster: ~1.33x the base level.
    const std::size_t base = 500u * 750u * 4u;
    const std::size_t full = ArtworkCache::texture_bytes(500, 750);
    REQUIRE(full > base + base / 4);
    REQUIRE(full < base + base / 2);
}

TEST_CASE("trim_textures_to evicts LRU down to the target and keeps the "
          "most recent", "[artwork]") {
    const int dim = 16;
    const std::size_t one = ArtworkCache::texture_bytes(dim, dim);
    ArtworkCache cache(10 * one);
    for (const char* u : {"a", "b", "c", "d"}) {
        cache.test_inject_ready_upload(make_upload(u, dim, dim));
        cache.pump_for_tests();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    cache.test_touch("a");  // a becomes MRU
    REQUIRE(cache.entries_count() == 4);

    REQUIRE(cache.trim_textures_to(2 * one) == 2);
    REQUIRE(cache.entries_count() == 2);
    REQUIRE(cache.bytes_in_use() == 2 * one);
    REQUIRE(cache.get_or_fetch("a") != 0);  // MRU kept
    REQUIRE(cache.get_or_fetch("d") != 0);  // newest kept

    // Already under target: no-op.
    REQUIRE(cache.trim_textures_to(10 * one) == 0);
    REQUIRE(cache.trim_textures_to(0) == 2);
    REQUIRE(cache.bytes_in_use() == 0);
}

// ---------------------------------------------------------------------------
// Card variants + no-thrash eviction (RC finding: TV-heavy Library pages on
// a Pi 4B uploaded and evicted posters every frame).
// ---------------------------------------------------------------------------

TEST_CASE("Card and Full requests for one URL are independent entries",
          "[artwork][variant]") {
    using media_browser::ArtworkVariant;
    using media_browser::artwork_cache_key;
    ArtworkCache cache;
    const std::string u = "https://artworks.thetvdb.com/banners/p.jpg";

    REQUIRE(cache.get_or_fetch(u, ArtworkVariant::Card) == 0);
    REQUIRE(cache.test_is_in_flight(artwork_cache_key(u, ArtworkVariant::Card)));
    REQUIRE_FALSE(cache.test_is_in_flight(u));
    REQUIRE(cache.get_or_fetch(u) == 0);  // Full: its own fetch
    REQUIRE(cache.test_is_in_flight(u));

    cache.test_inject_ready_upload(
        make_upload(artwork_cache_key(u, ArtworkVariant::Card), 37, 54));
    cache.test_inject_ready_upload(make_upload(u, 68, 100));
    REQUIRE(cache.pump_for_tests() == 2);

    const auto card = cache.get_dims(u, ArtworkVariant::Card);
    const auto full = cache.get_dims(u);
    REQUIRE(card);
    REQUIRE(full);
    CHECK(card->w == 37);
    CHECK(full->w == 68);
    CHECK(cache.get_or_fetch(u, ArtworkVariant::Card) !=
          cache.get_or_fetch(u, ArtworkVariant::Full));
}

TEST_CASE("Eviction never drops a texture drawn this or last frame",
          "[artwork][thrash]") {
    const int dim = 16;
    const std::size_t one = ArtworkCache::texture_bytes(dim, dim);
    ArtworkCache cache(2 * one);  // budget: 2 posters

    // Frame 1 draws a and b.
    cache.begin_frame();
    cache.test_inject_ready_upload(make_upload("a", dim, dim));
    cache.test_inject_ready_upload(make_upload("b", dim, dim));
    cache.pump_for_tests();
    REQUIRE(cache.get_or_fetch("a") != 0);
    REQUIRE(cache.get_or_fetch("b") != 0);

    // c arrives while a and b are on screen: nothing may go, the cache
    // overshoots instead (and says so once).
    cache.test_inject_ready_upload(make_upload("c", dim, dim));
    cache.pump_for_tests();
    CHECK(cache.entries_count() == 3);
    CHECK(cache.bytes_in_use() == 3 * one);
    CHECK(cache.budget_overshoots() == 1);

    // Frame 2 draws a and c only; b (drawn in frame 1) is still protected.
    cache.begin_frame();
    REQUIRE(cache.get_or_fetch("a") != 0);
    REQUIRE(cache.get_or_fetch("c") != 0);
    CHECK(cache.entries_count() == 3);

    // Frame 3: b is two drawn frames stale, so the overshoot heals by
    // evicting exactly b.
    cache.begin_frame();
    CHECK(cache.entries_count() == 2);
    CHECK(cache.bytes_in_use() == 2 * one);
    CHECK(cache.get_or_fetch("b") == 0);
    CHECK(cache.get_or_fetch("a") != 0);
    CHECK(cache.get_or_fetch("c") != 0);
    CHECK(cache.budget_overshoots() == 1);  // one episode, logged once
}

TEST_CASE("A visible set larger than the budget does not thrash",
          "[artwork][thrash]") {
    const int dim = 16;
    const std::size_t one = ArtworkCache::texture_bytes(dim, dim);
    ArtworkCache cache(2 * one);
    const std::vector<std::string> visible = {"p0", "p1", "p2", "p3"};

    std::size_t uploads = 0;
    for (int frame = 0; frame < 20; ++frame) {
        cache.begin_frame();
        // Simulate the fetcher completing whatever the last frame asked for.
        for (const auto& u : visible) {
            if (cache.test_is_in_flight(u)) {
                cache.test_inject_ready_upload(make_upload(u, dim, dim));
            }
        }
        uploads += cache.pump_for_tests();
        for (const auto& u : visible) cache.get_or_fetch(u);  // draw
    }
    // Each poster uploaded exactly once; none was ever evicted and refetched.
    CHECK(uploads == visible.size());
    CHECK(cache.entries_count() == visible.size());
    for (const auto& u : visible) CHECK(cache.get_or_fetch(u) != 0);
}

TEST_CASE("Newly uploaded textures survive later uploads in the same pump",
          "[artwork][thrash]") {
    const int dim = 16;
    const std::size_t one = ArtworkCache::texture_bytes(dim, dim);
    ArtworkCache cache(one);
    cache.begin_frame();
    cache.test_inject_ready_upload(make_upload("x", dim, dim));
    cache.test_inject_ready_upload(make_upload("y", dim, dim));
    cache.pump_for_tests();
    // Both just landed for the frame about to draw them: neither may be
    // dropped before it is ever shown.
    CHECK(cache.entries_count() == 2);
}

TEST_CASE("trim_textures_to is forced: it ignores the on-screen protection",
          "[artwork][thrash]") {
    const int dim = 16;
    const std::size_t one = ArtworkCache::texture_bytes(dim, dim);
    ArtworkCache cache(10 * one);
    cache.begin_frame();
    for (const char* u : {"a", "b", "c"}) {
        cache.test_inject_ready_upload(make_upload(u, dim, dim));
    }
    cache.pump_for_tests();
    for (const char* u : {"a", "b", "c"}) REQUIRE(cache.get_or_fetch(u) != 0);
    // Movie playback start must still be able to free memory.
    CHECK(cache.trim_textures_to(one) == 2);
    CHECK(cache.entries_count() == 1);
}
