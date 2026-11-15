#pragma once

// The Channel Guide's clock: it prints local time = UTC + the zone's offset,
// and asks the zone for that offset as rarely as it can. A zone's offset
// only changes on a UTC minute boundary (whole- and half-hour zones and DST
// switches alike), so it is looked up again only when "now" is in a new UTC
// minute, or after invalidate() — main.cpp's timezone_reload_request poke
// calls tzset() and then invalidate(). The lookup is passed in (the kiosk's
// localtime_r tm_gmtoff, a fake in tests/app/test_guide_clock.cpp), so this
// is pure and header-only. Render thread only.

#include <cmath>
#include <cstdint>
#include <limits>

namespace app {

class GuideClock {
public:
    // The offset for now_utc (seconds east of UTC), calling lookup(now_utc)
    // only when now_utc is in another UTC minute than the last lookup, or
    // after invalidate().
    template <class Lookup>
    int offset(double now_utc, Lookup&& lookup) {
        const auto minute = static_cast<int64_t>(std::floor(now_utc / 60.0));
        if (minute != minute_) {
            offset_s_ = lookup(now_utc);
            minute_ = minute;
        }
        return offset_s_;
    }

    // The zone may have changed (tzset() ran): the next offset() looks it up.
    void invalidate() { minute_ = kNone; }

    // The local minute (UTC seconds + offset, floored to minutes): what the
    // guide's clock shows changes exactly when this does.
    static int64_t local_minute(double now_utc, int offset_s) {
        return static_cast<int64_t>(std::floor((now_utc + offset_s) / 60.0));
    }

private:
    static constexpr int64_t kNone = std::numeric_limits<int64_t>::min();
    int offset_s_ = 0;
    int64_t minute_ = kNone;  // the UTC minute offset_s_ was looked up for
};

}  // namespace app
