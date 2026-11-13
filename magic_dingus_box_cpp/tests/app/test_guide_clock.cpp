// Tests for app/guide_clock.h: the Channel Guide's UTC offset is looked up
// at most once a UTC minute, or again after a tzset() (invalidate()).

#include <catch2/catch_test_macros.hpp>

#include "app/guide_clock.h"

namespace {

struct FakeZone {
    int offset = 0;
    int lookups = 0;
    int operator()(double) {
        ++lookups;
        return offset;
    }
};

}  // namespace

TEST_CASE("the offset is looked up once per UTC minute", "[guide_clock]") {
    app::GuideClock clock;
    FakeZone zone{-5 * 3600};
    const double t0 = 1767225600.0 + 10 * 60;  // a minute boundary
    CHECK(clock.offset(t0 + 0.5, zone) == -5 * 3600);
    CHECK(zone.lookups == 1);
    // Every frame of that minute: no lookup.
    CHECK(clock.offset(t0 + 1.0, zone) == -5 * 3600);
    CHECK(clock.offset(t0 + 59.99, zone) == -5 * 3600);
    CHECK(zone.lookups == 1);
    // The next minute: one lookup, which picks up a DST switch.
    zone.offset = -4 * 3600;
    CHECK(clock.offset(t0 + 60.0, zone) == -4 * 3600);
    CHECK(zone.lookups == 2);
    CHECK(clock.offset(t0 + 61.0, zone) == -4 * 3600);
    CHECK(zone.lookups == 2);
}

TEST_CASE("after tzset (invalidate) the next offset is looked up again", "[guide_clock]") {
    app::GuideClock clock;
    FakeZone zone{0};
    const double t = 1767225600.0 + 30.0;
    CHECK(clock.offset(t, zone) == 0);
    zone.offset = 9 * 3600;  // the zone changed mid-minute
    CHECK(clock.offset(t + 1.0, zone) == 0);  // not looked up yet
    clock.invalidate();
    CHECK(clock.offset(t + 2.0, zone) == 9 * 3600);
    CHECK(zone.lookups == 2);
    CHECK(clock.offset(t + 3.0, zone) == 9 * 3600);
    CHECK(zone.lookups == 2);
}

TEST_CASE("before the epoch and across zero the minute still floors", "[guide_clock]") {
    app::GuideClock clock;
    FakeZone zone{0};
    clock.offset(-0.5, zone);  // minute -1
    clock.offset(-59.9, zone);
    CHECK(zone.lookups == 1);
    clock.offset(0.0, zone);  // minute 0
    CHECK(zone.lookups == 2);
}

TEST_CASE("the local minute follows the offset", "[guide_clock]") {
    // 00:00:59.5 UTC: minute 0 in UTC, 1:00 local an hour east, and a
    // half-hour zone moves it by 30.
    CHECK(app::GuideClock::local_minute(59.5, 0) == 0);
    CHECK(app::GuideClock::local_minute(59.5, 3600) == 60);
    CHECK(app::GuideClock::local_minute(59.5, 5 * 3600 + 1800) == 330);
    CHECK(app::GuideClock::local_minute(30.0, -3600) == -60);
}
