#pragma once

// Naming what held the render loop. The loop pings the systemd watchdog
// (WatchdogSec=10), and with the SD card saturated by writes two different
// things can hold it for seconds:
//   - a blocking SYSCALL (a stat/open/read of a file on the card, or a
//     GStreamer call waiting on one), and
//   - a MAJOR PAGE FAULT: the kiosk's own code or data, evicted by the
//     page-cache flood, read back from the same card.
// The fix differs (move the call off the loop, vs keep the pages
// resident), and the journal cannot tell them apart — nor time them: under
// write saturation journald stamps lines seconds after they were written
// (5-11 s on magicpi5, 2026-11-16; a killed kiosk's last lines were stamped
// after its SIGABRT).
//
// So a slow stretch is logged with the calling thread's major-fault count
// during it: "+0" means a blocking call. Logged to std::cerr (the journal,
// over a socket), never spdlog — its file sink is on the card.

#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

#include <sys/resource.h>

namespace utils {

// Major page faults of the CALLING thread so far; -1 where the platform
// cannot say (macOS has no RUSAGE_THREAD).
inline long thread_major_faults() {
#ifdef RUSAGE_THREAD
    struct rusage ru {};
    if (getrusage(RUSAGE_THREAD, &ru) == 0) return ru.ru_majflt;
#endif
    return -1;
}

// "+3" / "n/a": the faults between two thread_major_faults() readings.
inline std::string fault_delta(long before, long after) {
    if (before < 0 || after < 0) return "n/a";
    return "+" + std::to_string(after - before);
}

// The render loop's iteration timer. tick() at the same point of every
// iteration; it returns a report line when the PREVIOUS iteration took
// kThreshold or more. An `exempt` iteration (a game session runs inside
// one, with the watchdog disarmed) only re-anchors.
class LoopStallReport {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kThreshold{1000};

    std::optional<std::string> tick(Clock::time_point now, long faults, bool exempt) {
        std::optional<std::string> line;
        if (started_ && !exempt && now - last_ >= kThreshold) {
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_).count();
            line = "Render loop: an iteration took " + std::to_string(ms) +
                   " ms (major page faults on this thread " + fault_delta(last_faults_, faults) +
                   "; +0 = a blocking call, not the kiosk's pages)";
        }
        started_ = true;
        last_ = now;
        last_faults_ = faults;
        return line;
    }

private:
    bool started_ = false;
    Clock::time_point last_{};
    long last_faults_ = -1;
};

// A stretch of render-thread work that should be instant; logs itself on
// destruction when it took `threshold` or more.
class SlowSection {
public:
    explicit SlowSection(std::string what,
                         std::chrono::milliseconds threshold = std::chrono::milliseconds(250))
        : what_(std::move(what)), threshold_(threshold),
          start_(std::chrono::steady_clock::now()), faults_(thread_major_faults()) {}
    ~SlowSection() {
        const auto took = std::chrono::steady_clock::now() - start_;
        if (took < threshold_) return;
        std::cerr << "Slow on the render loop: " << what_ << " took "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(took).count()
                  << " ms (major page faults " << fault_delta(faults_, thread_major_faults())
                  << ")" << std::endl;
    }
    SlowSection(const SlowSection&) = delete;
    SlowSection& operator=(const SlowSection&) = delete;

private:
    std::string what_;
    std::chrono::milliseconds threshold_;
    std::chrono::steady_clock::time_point start_;
    long faults_;
};

}  // namespace utils
