// src/media_browser/health/vpn_health_monitor.h
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

namespace app { struct AppState; }

namespace media_browser {

// Polls a "VPN healthy?" probe in the background and updates
// state.media_browser_vpn_healthy.
//
// Three-strikes debounce: 3 consecutive failed polls flip healthy
// true->false. Recovery is instant on first success. Initial state
// is whatever the AppState already has — no implicit reset on
// start().
class VpnHealthMonitor {
public:
    using PingFn = std::function<bool()>;

    // Default constructor uses an HTTP ping to localhost:7878/ping with
    // a 3s curl timeout. The poll loop skips this during movie playback
    // (state.video_active) since Radarr is paused then by the kiosk's
    // playback CPU saver, which would otherwise produce false "tunnel
    // down" toasts ~30s into every movie.
    explicit VpnHealthMonitor(app::AppState& state);

    // Test seam: inject a ping function and a custom poll interval.
    VpnHealthMonitor(app::AppState& state,
                     PingFn ping_fn,
                     std::chrono::milliseconds poll_interval);

    ~VpnHealthMonitor();

    // No-op if already running.
    void start();
    // Joins the worker thread. Idempotent.
    void stop();

    // For diagnostics.
    int consecutive_failures() const { return consecutive_failures_.load(); }

private:
    void run();

    app::AppState& state_;
    PingFn ping_fn_;
    std::chrono::milliseconds poll_interval_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<int> consecutive_failures_{0};

    // Cold-boot guard. Set on the first successful ping. Until then,
    // we don't flip the healthy flag to false on the 3-strikes path
    // — the docker stack just hasn't finished coming up yet, so
    // "Radarr unreachable" is the expected state, not a tunnel drop.
    // Without this, every cold reboot showed a "Tunnel down" toast
    // for the ~60s gap between kiosk start and Radarr being ready,
    // which read as a real failure to the operator.
    std::atomic<bool> ever_healthy_{false};

    std::thread worker_;

    static constexpr int kFailureThreshold = 3;
};

}  // namespace media_browser
