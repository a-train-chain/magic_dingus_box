// tests/media_browser/test_vpn_health_monitor.cpp
#include <catch2/catch_test_macros.hpp>
#include "media_browser/health/vpn_health_monitor.h"
#include "app/app_state.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace {
// Test ping function: returns whatever the controller sets.
struct ScriptedPinger {
    std::atomic<bool> next_result{false};
    bool operator()() const { return next_result.load(); }
};
}  // namespace

TEST_CASE("VpnHealthMonitor flips Healthy after first successful poll",
          "[vpn_health_monitor]") {
    // Explicitly set the precondition. AppState defaults
    // media_browser_vpn_healthy to true (so the kiosk shows MB
    // entries on cold boot until proven otherwise), so this test
    // needs to flip to false manually to exercise the "first
    // successful poll arms the healthy flag" transition.
    app::AppState state;
    state.media_browser_vpn_healthy = false;
    REQUIRE(state.media_browser_vpn_healthy == false);

    ScriptedPinger pinger;
    pinger.next_result = true;

    media_browser::VpnHealthMonitor monitor(
        state,
        [&pinger]() { return pinger(); },
        std::chrono::milliseconds(5));   // tight poll interval for tests

    monitor.start();
    // Allow at least one poll cycle.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    monitor.stop();

    REQUIRE(state.media_browser_vpn_healthy == true);
}

TEST_CASE("VpnHealthMonitor stays Healthy through 2 failures (debounce)",
          "[vpn_health_monitor]") {
    app::AppState state;
    state.media_browser_vpn_healthy = true;

    ScriptedPinger pinger;
    pinger.next_result = false;   // every poll fails

    // 20ms interval + 30ms wait = exactly 2 polls (t=0 and t=20).
    // Wider interval makes the timing robust against scheduler jitter
    // — modern Linux can drift 1-2ms; 5ms intervals are too tight to
    // reliably distinguish "2 polls" from "3 polls".
    media_browser::VpnHealthMonitor monitor(
        state,
        [&pinger]() { return pinger(); },
        std::chrono::milliseconds(20));

    monitor.start();
    // Let exactly 2 polls happen. Should NOT flip yet (n=2, threshold=3).
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    REQUIRE(state.media_browser_vpn_healthy == true);
    monitor.stop();
}

TEST_CASE("VpnHealthMonitor flips Unhealthy after 3 consecutive failures",
          "[vpn_health_monitor]") {
    app::AppState state;
    state.media_browser_vpn_healthy = true;

    ScriptedPinger pinger;
    // Arm the cold-boot guard first: one successful poll → ever_healthy_
    // gets set, so subsequent failures actually flip the flag. Without
    // this prelude the monitor (post-cold-boot-guard fix) would never
    // flip unhealthy because it never saw the tunnel come up.
    pinger.next_result = true;

    media_browser::VpnHealthMonitor monitor(
        state,
        [&pinger]() { return pinger(); },
        std::chrono::milliseconds(5));

    monitor.start();
    // Let one successful poll register (~10ms = 2 poll intervals).
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    REQUIRE(state.media_browser_vpn_healthy == true);

    // Now switch to failing. 3 consecutive failures flip unhealthy.
    pinger.next_result = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    monitor.stop();

    REQUIRE(state.media_browser_vpn_healthy == false);
}

TEST_CASE("VpnHealthMonitor does NOT flip Unhealthy before first successful poll (cold-boot guard)",
          "[vpn_health_monitor]") {
    // Scenario: cold reboot. Kiosk boots before the docker stack finishes
    // coming up. Radarr's /ping fails for ~60-90s. Pre-fix this produced
    // a confusing "Tunnel down" toast within 30s of boot, even though
    // the tunnel was simply still warming up.
    //
    // After the cold-boot guard, the monitor stays in the "healthy"
    // initial state until the FIRST successful ping, regardless of how
    // many failures occurred before then.
    app::AppState state;
    state.media_browser_vpn_healthy = true;

    ScriptedPinger pinger;
    pinger.next_result = false;  // tunnel never comes up in this test

    media_browser::VpnHealthMonitor monitor(
        state,
        [&pinger]() { return pinger(); },
        std::chrono::milliseconds(5));

    monitor.start();
    // Many more failures than the 3-strikes threshold — but no
    // successful poll ever happens.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    monitor.stop();

    // Flag stays at its initial value; no false-positive toast fires.
    REQUIRE(state.media_browser_vpn_healthy == true);
}

TEST_CASE("VpnHealthMonitor recovers immediately on first successful poll",
          "[vpn_health_monitor]") {
    app::AppState state;
    state.media_browser_vpn_healthy = false;

    ScriptedPinger pinger;
    pinger.next_result = true;

    media_browser::VpnHealthMonitor monitor(
        state,
        [&pinger]() { return pinger(); },
        std::chrono::milliseconds(5));

    monitor.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    monitor.stop();

    REQUIRE(state.media_browser_vpn_healthy == true);
}
