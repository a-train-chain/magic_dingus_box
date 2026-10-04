// Unit tests for wait_for_service — the "services ready" gate that holds a
// post-playback mutation until the *arr container a FullPause session stopped
// is answering again. Driven entirely through injected clock / sleep hooks,
// so a 90 s deadline runs in microseconds and never touches a socket.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <vector>

#include "media_browser/service_gate.h"

namespace mb = media_browser;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

// A fake clock the hooks share: sleep() and a slow ping both advance it.
struct FakeTime {
    Clock::time_point t{};
    std::vector<std::chrono::milliseconds> sleeps;
};

mb::ServiceGateHooks hooks_on(FakeTime& ft) {
    mb::ServiceGateHooks h;
    h.now = [&ft] { return ft.t; };
    h.sleep = [&ft](std::chrono::milliseconds d) {
        ft.sleeps.push_back(d);
        ft.t += d;
    };
    return h;
}

}  // namespace

TEST_CASE("service gate: an answering service is Ready at once, no sleep",
          "[service_gate]") {
    FakeTime ft;
    auto h = hooks_on(ft);
    int pings = 0;
    h.ping = [&] { ++pings; return true; };
    CHECK(mb::wait_for_service(h) == mb::GateResult::Ready);
    CHECK(pings == 1);
    CHECK(ft.sleeps.empty());
}

TEST_CASE("service gate: polls until the restarted container answers",
          "[service_gate]") {
    // The FullPause shape: docker start returns long before Sonarr/Radarr
    // answer (20-40 s). The gate must keep polling, not give up on the
    // first refusal.
    FakeTime ft;
    auto h = hooks_on(ft);
    const auto start = ft.t;
    h.ping = [&] { return ft.t - start >= 30s; };
    mb::ServiceGateTiming timing;
    timing.poll_interval = 2s;
    CHECK(mb::wait_for_service(h, timing) == mb::GateResult::Ready);
    CHECK(ft.t - start >= 30s);
    CHECK(ft.t - start < 33s);  // answered within one poll interval
}

TEST_CASE("service gate: never answering times out at the deadline",
          "[service_gate]") {
    FakeTime ft;
    auto h = hooks_on(ft);
    const auto start = ft.t;
    int pings = 0;
    h.ping = [&] { ++pings; return false; };
    mb::ServiceGateTiming timing;
    timing.deadline = 90s;
    timing.poll_interval = 2s;
    CHECK(mb::wait_for_service(h, timing) == mb::GateResult::TimedOut);
    // Bounded: it neither returns early nor overshoots by more than a slice.
    CHECK(ft.t - start >= 90s);
    CHECK(ft.t - start <= 90s + timing.slice);
    CHECK(pings >= 40);
}

TEST_CASE("service gate: a slow ping counts against the deadline",
          "[service_gate]") {
    // Each refused request can itself burn the 5 s curl timeout; the
    // deadline is wall-clock, not a poll count.
    FakeTime ft;
    auto h = hooks_on(ft);
    const auto start = ft.t;
    int pings = 0;
    h.ping = [&] { ++pings; ft.t += 5s; return false; };
    mb::ServiceGateTiming timing;
    timing.deadline = 20s;
    timing.poll_interval = 2s;
    CHECK(mb::wait_for_service(h, timing) == mb::GateResult::TimedOut);
    CHECK(pings <= 4);
    CHECK(ft.t - start <= 25s);
}

TEST_CASE("service gate: waits for the quiet-mode worker before pinging",
          "[service_gate]") {
    // A resume queued at leave() runs on the MovieQuietMode worker. A ping
    // that lands while the PAUSE is still executing could see the service
    // up a moment before docker stop takes it down — so the idle wait
    // comes first, in bounded slices.
    FakeTime ft;
    auto h = hooks_on(ft);
    const auto start = ft.t;
    std::vector<std::chrono::milliseconds> idle_waits;
    h.wait_quiet_idle = [&](std::chrono::milliseconds d) {
        idle_waits.push_back(d);
        ft.t += d;
        return ft.t - start >= 3s;  // worker goes idle after ~3 s
    };
    bool pinged_before_idle = false;
    h.ping = [&] {
        if (ft.t - start < 3s) pinged_before_idle = true;
        return true;
    };
    CHECK(mb::wait_for_service(h) == mb::GateResult::Ready);
    CHECK_FALSE(pinged_before_idle);
    REQUIRE_FALSE(idle_waits.empty());
    for (const auto d : idle_waits) CHECK(d <= 1s);  // sliced, cancellable
}

TEST_CASE("service gate: a quiet worker that never idles still times out",
          "[service_gate]") {
    FakeTime ft;
    auto h = hooks_on(ft);
    const auto start = ft.t;
    h.wait_quiet_idle = [&](std::chrono::milliseconds d) {
        ft.t += d;
        return false;
    };
    bool pinged = false;
    h.ping = [&] { pinged = true; return true; };
    mb::ServiceGateTiming timing;
    timing.deadline = 10s;
    CHECK(mb::wait_for_service(h, timing) == mb::GateResult::TimedOut);
    CHECK_FALSE(pinged);
    CHECK(ft.t - start <= 11s);
}

TEST_CASE("service gate: cancellation wins promptly in every phase",
          "[service_gate]") {
    FakeTime ft;
    auto h = hooks_on(ft);
    const auto start = ft.t;
    bool cancel = false;
    h.cancelled = [&] { return cancel; };

    SECTION("before anything runs") {
        cancel = true;
        h.ping = [] { return true; };
        CHECK(mb::wait_for_service(h) == mb::GateResult::Cancelled);
    }
    SECTION("while polling a refusing service") {
        h.ping = [&] {
            if (ft.t - start >= 6s) cancel = true;
            return false;
        };
        CHECK(mb::wait_for_service(h) == mb::GateResult::Cancelled);
        // Shutdown must not sit out the rest of a 2 s poll interval.
        CHECK(ft.t - start <= 6s + std::chrono::milliseconds(500));
    }
    SECTION("while waiting for the quiet worker") {
        h.wait_quiet_idle = [&](std::chrono::milliseconds d) {
            ft.t += d;
            if (ft.t - start >= 2s) cancel = true;
            return false;
        };
        h.ping = [] { return true; };
        CHECK(mb::wait_for_service(h) == mb::GateResult::Cancelled);
        CHECK(ft.t - start <= 3s);
    }
}

TEST_CASE("service gate: no ping hook is a programming error reported as TimedOut",
          "[service_gate]") {
    FakeTime ft;
    auto h = hooks_on(ft);
    CHECK(mb::wait_for_service(h) == mb::GateResult::TimedOut);
    CHECK(ft.sleeps.empty());  // and it does not spin out the deadline
}
