// utils/stall_report.h: what held the render loop, and whether it was a
// blocking call (no major page faults) or the kiosk's own pages.

#include <catch2/catch_test_macros.hpp>

#include "utils/stall_report.h"

#include <chrono>

using utils::LoopStallReport;
using namespace std::chrono_literals;

namespace {
const LoopStallReport::Clock::time_point t0{std::chrono::hours(1)};
}

TEST_CASE("fault_delta: a count, or n/a where the platform cannot say", "[stall_report]") {
    CHECK(utils::fault_delta(10, 13) == "+3");
    CHECK(utils::fault_delta(5, 5) == "+0");
    CHECK(utils::fault_delta(-1, 4) == "n/a");
    CHECK(utils::fault_delta(4, -1) == "n/a");
}

TEST_CASE("the loop report names an iteration of a second or more", "[stall_report]") {
    LoopStallReport r;
    CHECK_FALSE(r.tick(t0, 100, false));            // first tick only anchors
    CHECK_FALSE(r.tick(t0 + 16ms, 100, false));     // an ordinary frame
    CHECK_FALSE(r.tick(t0 + 1015ms, 100, false));   // 999 ms: under
    const auto line = r.tick(t0 + 5015ms, 100, false);
    REQUIRE(line);
    CHECK(line->find("4000 ms") != std::string::npos);
    CHECK(line->find("faults on this thread +0") != std::string::npos);
    const auto faulted = r.tick(t0 + 7015ms, 104, false);
    REQUIRE(faulted);
    CHECK(faulted->find("+4") != std::string::npos);
}

TEST_CASE("a game session's long iteration is not a stall", "[stall_report]") {
    LoopStallReport r;
    r.tick(t0, 0, false);
    CHECK_FALSE(r.tick(t0 + 600s, 0, true));  // RetroArch ran inside it
    CHECK_FALSE(r.tick(t0 + 600s + 16ms, 0, false));  // re-anchored there
}

TEST_CASE("thread_major_faults never goes backwards on this thread", "[stall_report]") {
    const long a = utils::thread_major_faults();
    const long b = utils::thread_major_faults();
    if (a >= 0) {
        CHECK(b >= a);
    } else {
        CHECK(b == -1);  // no RUSAGE_THREAD (macOS)
    }
}
