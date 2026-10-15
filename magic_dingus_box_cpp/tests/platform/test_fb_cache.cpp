// platform::FbCache — the GBM-buffer -> DRM-framebuffer cache behind
// FramePresenter. Pure bookkeeping; the drmModeRmFB call is injected so the
// eviction policy is checkable on the Mac.
//
// The bug these pin: the old cache trimmed by erasing unordered_map::begin()
// and skipped drmModeRmFB when that entry was the framebuffer on screen —
// the entry left the cache but the kernel object did not, one leaked FB per
// such eviction. RmFB'ing it instead is no better: removing the scanned-out
// FB makes the kernel disable the CRTC (a black screen). The only correct
// move is to evict a DIFFERENT entry, which always exists when trimming.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "platform/fb_cache.h"

using platform::FbCache;

namespace {

struct RmLog {
    std::vector<uint32_t> removed;
    FbCache::RemoveFn fn() {
        return [this](uint32_t fb) { removed.push_back(fb); };
    }
};

}  // namespace

TEST_CASE("FbCache: find returns the fb for a known bo handle", "[fb_cache]") {
    FbCache c;
    c.insert(/*bo=*/10, /*fb=*/100);
    REQUIRE(c.find(10).has_value());
    CHECK(*c.find(10) == 100);
    CHECK_FALSE(c.find(11).has_value());
    CHECK(c.size() == 1);
}

TEST_CASE("FbCache: trim evicts oldest first and RmFBs it", "[fb_cache]") {
    FbCache c;
    RmLog log;
    for (uint32_t i = 1; i <= 6; ++i) c.insert(i, 100 + i);
    c.trim(4, /*current_fb=*/106, log.fn());
    CHECK(c.size() == 4);
    CHECK(log.removed == std::vector<uint32_t>{101, 102});
    CHECK_FALSE(c.find(1).has_value());
    CHECK_FALSE(c.find(2).has_value());
    CHECK(c.find(3).has_value());
}

TEST_CASE("FbCache: trim never drops the scanned-out fb, and never leaks one",
          "[fb_cache]") {
    FbCache c;
    RmLog log;
    for (uint32_t i = 1; i <= 5; ++i) c.insert(i, 100 + i);
    // The OLDEST entry is the one on screen.
    c.trim(4, /*current_fb=*/101, log.fn());
    CHECK(c.size() == 4);
    // The on-screen fb survives in the cache (so it is still RmFB'd by a
    // later reset/clear), and the next-oldest is the one removed.
    CHECK(c.find(1).has_value());
    CHECK(log.removed == std::vector<uint32_t>{102});
    // Every fb that left the cache was RmFB'd.
    CHECK(c.size() + log.removed.size() == 5);
}

TEST_CASE("FbCache: trim is a no-op at or under the limit", "[fb_cache]") {
    FbCache c;
    RmLog log;
    for (uint32_t i = 1; i <= 4; ++i) c.insert(i, 100 + i);
    c.trim(4, 0, log.fn());
    CHECK(c.size() == 4);
    CHECK(log.removed.empty());
}

TEST_CASE("FbCache: drop_all_except keeps (and does not RmFB) the current fb",
          "[fb_cache]") {
    FbCache c;
    RmLog log;
    c.insert(1, 101);
    c.insert(2, 102);
    c.insert(3, 103);
    c.drop_all_except(/*current_fb=*/102, log.fn());
    CHECK(log.removed == std::vector<uint32_t>{101, 103});
    CHECK(c.size() == 1);
    REQUIRE(c.find(2).has_value());
    CHECK(*c.find(2) == 102);
}

TEST_CASE("FbCache: drop_all_except with no current fb drops everything",
          "[fb_cache]") {
    FbCache c;
    RmLog log;
    c.insert(1, 101);
    c.insert(2, 102);
    c.drop_all_except(0, log.fn());
    CHECK(c.size() == 0);
    CHECK(log.removed.size() == 2);
}

TEST_CASE("FbCache: clear RmFBs every entry including the current one",
          "[fb_cache]") {
    FbCache c;
    RmLog log;
    c.insert(1, 101);
    c.insert(2, 102);
    c.clear(log.fn());
    CHECK(c.size() == 0);
    CHECK(log.removed == std::vector<uint32_t>{101, 102});
}

TEST_CASE("FbCache: re-inserting a handle replaces and RmFBs the stale fb",
          "[fb_cache]") {
    // A handle can only be re-added after its entry was dropped, but if a
    // caller ever does it twice the older kernel FB must not leak.
    FbCache c;
    RmLog log;
    c.insert(1, 101);
    c.insert(1, 201, log.fn());
    CHECK(c.size() == 1);
    CHECK(*c.find(1) == 201);
    CHECK(log.removed == std::vector<uint32_t>{101});
}
