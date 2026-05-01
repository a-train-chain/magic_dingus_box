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
    app::AppState state;
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
    pinger.next_result = false;

    media_browser::VpnHealthMonitor monitor(
        state,
        [&pinger]() { return pinger(); },
        std::chrono::milliseconds(5));

    monitor.start();
    // Let at least 4 polls happen (~20ms). Three failures + buffer.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    monitor.stop();

    REQUIRE(state.media_browser_vpn_healthy == false);
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
