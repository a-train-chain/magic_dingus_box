// PerSourceLogLimit: GstPlayer's GStreamer WARNING lines, at most one per
// source element per interval, so a misbehaving element cannot flood the
// journal (the SD card) while the next frozen picture stays diagnosable.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "video/log_rate_limit.h"

using video::PerSourceLogLimit;
using Clock = PerSourceLogLimit::Clock;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {
const Clock::time_point kT0{std::chrono::hours(1000)};
}

TEST_CASE("one line per source per interval, with the count it held back",
          "[video][log_limit]") {
    PerSourceLogLimit limit(seconds(5));
    auto v = limit.admit("qtdemux0", kT0);
    CHECK(v.log);
    CHECK(v.suppressed == 0);
    CHECK_FALSE(limit.admit("qtdemux0", kT0 + milliseconds(1)).log);
    CHECK_FALSE(limit.admit("qtdemux0", kT0 + milliseconds(4999)).log);
    v = limit.admit("qtdemux0", kT0 + seconds(5));
    CHECK(v.log);
    CHECK(v.suppressed == 2);
    v = limit.admit("qtdemux0", kT0 + seconds(11));
    CHECK(v.log);
    CHECK(v.suppressed == 0);
}

TEST_CASE("sources are limited independently", "[video][log_limit]") {
    PerSourceLogLimit limit(seconds(5));
    CHECK(limit.admit("qtdemux0", kT0).log);
    CHECK(limit.admit("avdec_h264-0", kT0 + milliseconds(10)).log);
    CHECK_FALSE(limit.admit("qtdemux0", kT0 + milliseconds(20)).log);
    CHECK_FALSE(limit.admit("avdec_h264-0", kT0 + milliseconds(30)).log);
}

TEST_CASE("the set of sources is bounded; the longest-quiet one goes first",
          "[video][log_limit]") {
    PerSourceLogLimit limit(seconds(5), /*max_sources=*/3);
    CHECK(limit.admit("a", kT0).log);
    CHECK(limit.admit("b", kT0 + milliseconds(1)).log);
    CHECK(limit.admit("c", kT0 + milliseconds(2)).log);
    CHECK(limit.sources() == 3);
    CHECK(limit.admit("d", kT0 + milliseconds(3)).log);  // evicts a
    CHECK(limit.sources() == 3);
    CHECK_FALSE(limit.admit("b", kT0 + milliseconds(4)).log);  // still held
    CHECK(limit.admit("a", kT0 + milliseconds(5)).log);        // forgotten: new again
    CHECK(limit.sources() == 3);
}

TEST_CASE("clear forgets every source (a new stream)", "[video][log_limit]") {
    PerSourceLogLimit limit(seconds(5));
    CHECK(limit.admit("qtdemux0", kT0).log);
    limit.clear();
    CHECK(limit.sources() == 0);
    CHECK(limit.admit("qtdemux0", kT0 + milliseconds(1)).log);
}
