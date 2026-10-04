// video::QueryCache — the per-frame memo behind GstPlayer's position /
// duration getters.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "video/query_cache.h"

using video::QueryCache;

TEST_CASE("QueryCache starts stale", "[query_cache]") {
    QueryCache c;
    CHECK_FALSE(c.fresh(0));
    CHECK_FALSE(c.fresh(QueryCache::now_ns()));
}

TEST_CASE("QueryCache serves the rest of the frame, never the next one",
          "[query_cache]") {
    QueryCache c;
    const std::int64_t t0 = 1'000'000'000;
    c.store(t0);
    CHECK(c.fresh(t0));
    CHECK(c.fresh(t0 + 5'000'000));                    // later in the frame
    CHECK(c.fresh(t0 + QueryCache::kTtlNs - 1));
    CHECK_FALSE(c.fresh(t0 + QueryCache::kTtlNs));     // TTL boundary
    // The next 60 Hz frame (16.7 ms later) always re-queries.
    CHECK_FALSE(c.fresh(t0 + 16'666'667));
    // A clock read from before the stamp is never trusted.
    CHECK_FALSE(c.fresh(t0 - 1));
}

TEST_CASE("QueryCache TTL is under half a 60 Hz frame", "[query_cache]") {
    CHECK(QueryCache::kTtlNs * 2 <= 16'666'667);
}

TEST_CASE("QueryCache invalidate forces a re-query (seek / stop)",
          "[query_cache]") {
    QueryCache c;
    const std::int64_t t0 = 42;
    c.store(t0);
    REQUIRE(c.fresh(t0 + 1));
    c.invalidate();
    CHECK_FALSE(c.fresh(t0 + 1));
    c.store(t0 + 2);
    CHECK(c.fresh(t0 + 3));
}
