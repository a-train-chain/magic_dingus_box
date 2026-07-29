#include "time_format.h"

namespace utils {

std::string iso8601_utc(std::time_t t) {
    // gmtime_r, not gmtime: the kiosk runs background threads (VpnHealthMonitor,
    // the Prowlarr availability workers, the GPIO poll thread), and gmtime
    // returns a pointer into one shared static tm that a concurrent caller can
    // overwrite between the conversion and the strftime.
    //
    // ::gmtime_r and not std::gmtime_r: gmtime_r is POSIX, not ISO C, so <ctime>
    // is not required to place it in namespace std -- and neither libstdc++ nor
    // libc++ does. Global scope is the only spelling that compiles on both the
    // macOS test build and the Pi.
    std::tm tm_utc{};
    if (::gmtime_r(&t, &tm_utc) == nullptr) return {};

    // 32 is comfortably more than the 20 chars + NUL a 4-digit year needs; the
    // width check below, not the buffer size, is what enforces the contract.
    char buf[32];
    const std::size_t n =
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

    // EXACTLY 20, not merely "nonzero" -- see the contract in the header.
    //
    // An off-width stamp is a well-formed string that would pass a caller's
    // is-it-empty check and then sort WRONGLY against real 4-digit stamps:
    // "1-01-01T..." lands above every one of them (a cutoff that hides
    // everything), "10000-01-01T..." below every one of them (a cutoff that
    // hides nothing). That is the same silent-miscompare class this helper
    // exists to eliminate, so off-width is a failure, not an output.
    //
    // Measured, not assumed: glibc renders year 1 as 17 chars while Darwin
    // zero-pads it to 20, and both render year 10000 as 21. Checking width
    // rather than a year range is what makes the contract hold on both.
    //
    // Unreachable from system_clock::now() in practice; enforced anyway so the
    // header's guarantee is a fact rather than a hope, because callers are
    // documented to treat any non-empty return as safe to compare.
    if (n != 20) return {};
    return buf;
}

}  // namespace utils
