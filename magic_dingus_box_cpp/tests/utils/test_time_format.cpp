// utils::iso8601_utc tests.
//
// Moved here from tests/retroarch/test_controller_profile.cpp when the helper
// left retroarch/ for utils/: two callers now share it, one of them Media
// Browser code, and a media_browser -> retroarch dependency would have been
// the wrong shape.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <ctime>
#include <string>

#include "utils/time_format.h"

namespace {

// The well-known 10^9 instant: 2001-09-09T01:46:40Z. Chosen because every
// field is non-zero and no two are equal, so a swapped month/day or a dropped
// zero-pad shows up as a mismatch rather than an accidental pass.
constexpr std::time_t kNineZeros = 1000000000;

// Sets TZ for one test and puts it back afterwards, so the process-global
// mutation cannot leak into another case regardless of Catch2's ordering.
class TzGuard {
public:
    explicit TzGuard(const char* tz) {
        // Copy getenv's result BEFORE setenv can invalidate the pointer it
        // returned -- setenv is permitted to reallocate the environment block.
        const char* p = std::getenv("TZ");
        had_ = p != nullptr;
        if (had_) old_ = p;
        ::setenv("TZ", tz, 1);
        ::tzset();
    }
    ~TzGuard() {
        if (had_) ::setenv("TZ", old_.c_str(), 1); else ::unsetenv("TZ");
        ::tzset();
    }
    TzGuard(const TzGuard&) = delete;
    TzGuard& operator=(const TzGuard&) = delete;

private:
    std::string old_;
    bool had_ = false;
};

}  // namespace

TEST_CASE("iso8601_utc formats an epoch as ISO-8601 UTC", "[time_format]") {
    CHECK(utils::iso8601_utc(0) == "1970-01-01T00:00:00Z");
    CHECK(utils::iso8601_utc(kNineZeros) == "2001-09-09T01:46:40Z");
}

TEST_CASE("iso8601_utc is UTC regardless of the box's local timezone",
          "[time_format]") {
    // This is the whole reason the helper exists rather than a bare
    // localtime()/strftime() at each call site: the output carries a literal
    // trailing Z, and a Z-suffixed LOCAL time is not merely imprecise, it is
    // wrong -- it sorts and compares against other ISO-8601 stamps as though
    // it were UTC, which is exactly what library_screen.cpp's "recently added"
    // filter does with it. A Pi ships to whatever zone the customer sets, so
    // this cannot be left to strftime's localtime default.
    TzGuard guard("America/New_York");

    // Prove the zone actually took effect before trusting the CHECK below.
    // Both glibc and libc++ silently fall back to UTC when a zone name will
    // not resolve (a slim container with no tzdata), and under that fallback
    // localtime_r would agree with gmtime_r -- so this regression guard would
    // stop guarding while still reporting green. 1e9 is 01:46 UTC and 21:46
    // local here, so tm_hour is the discriminator.
    std::tm local{};
    REQUIRE(::localtime_r(&kNineZeros, &local) != nullptr);
    REQUIRE(local.tm_hour != 1);

    CHECK(utils::iso8601_utc(kNineZeros) == "2001-09-09T01:46:40Z");
}

TEST_CASE("iso8601_utc output is fixed-width so it sorts lexicographically",
          "[time_format]") {
    // Both callers depend on this and neither can check it: the controller
    // wizard writes the stamp into JSON that is later compared, and
    // library_screen.cpp compares it against Radarr's added_at with a bare
    // string >=. Lexicographic order equals chronological order ONLY while
    // every field stays zero-padded to a fixed width.
    const std::string a = utils::iso8601_utc(kNineZeros);
    const std::string b = utils::iso8601_utc(kNineZeros + 1);
    const std::string epoch = utils::iso8601_utc(0);

    REQUIRE(a.size() == 20);
    REQUIRE(b.size() == 20);
    REQUIRE(epoch.size() == 20);
    CHECK(a < b);          // one second later sorts later
    CHECK(epoch < a);      // 1970 sorts before 2001
    // Separator positions, which the ordering silently relies on.
    CHECK(a[4] == '-');
    CHECK(a[7] == '-');
    CHECK(a[10] == 'T');
    CHECK(a[13] == ':');
    CHECK(a[16] == ':');
    CHECK(a[19] == 'Z');
}

TEST_CASE("iso8601_utc zero-pads single-digit fields", "[time_format]") {
    // 2001-01-02T03:04:05Z — month, day, hour, minute AND second all single
    // digit in one value. The two fixtures above already pin padding field by
    // field (the epoch pins all five at 0, kNineZeros pins month/day/hour), so
    // this is a belt-and-braces regression pin on the combination rather than
    // new coverage.
    CHECK(utils::iso8601_utc(978404645) == "2001-01-02T03:04:05Z");
}

TEST_CASE("iso8601_utc never returns an off-width stamp", "[time_format]") {
    // The contract is "empty or exactly 20 chars", and THIS is the case that
    // covers the difference between the implementation's `n != 20` check and a
    // weaker `n == 0`. An off-width stamp is well-formed enough to pass a
    // caller's is-it-empty check and then sort wrongly against real 4-digit
    // stamps -- which is the silent-miscompare class the helper exists to stop.
    //
    // Asserted as the contract rather than as a specific string, because %Y is
    // exactly where the two libcs disagree. Measured for year 1:
    //   Darwin/libc++  -> "0001-01-01T00:00:00Z" (20, zero-padded)  => accepted
    //   glibc/aarch64  -> "1-01-01T00:00:00Z"    (17, not padded)   => rejected
    // Both are correct under the contract; asserting .empty() here would encode
    // one platform's padding behavior and fail on the other. Year 10000 is 21
    // chars on both, so that one is rejected everywhere.
    for (const long long v : {-62135596800LL,    // year 1
                              253402300800LL,    // year 10000
                              -2208988800LL,     // 1900
                              4102444800LL}) {   // 2100
        const std::string s = utils::iso8601_utc(static_cast<std::time_t>(v));
        CHECK((s.empty() || s.size() == 20));
    }

    // The in-range boundaries stay ACCEPTED on both platforms, so the width
    // check cannot be "hardened" into something that rejects valid input.
    CHECK(utils::iso8601_utc(-30610224000LL) == "1000-01-01T00:00:00Z");
    CHECK(utils::iso8601_utc(253402300799LL) == "9999-12-31T23:59:59Z");
}
