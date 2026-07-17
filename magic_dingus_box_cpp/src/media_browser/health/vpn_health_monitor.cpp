// src/media_browser/health/vpn_health_monitor.cpp
#include "media_browser/health/vpn_health_monitor.h"

#include "app/app_state.h"

#include <curl/curl.h>
#include <spdlog/spdlog.h>

namespace media_browser {

namespace {

// Default ping: GET http://127.0.0.1:7878/ping with 3s timeout.
// Returns true on HTTP 2xx, false on any error or non-2xx.
//
// NB: Radarr is paused by the kiosk's playback CPU saver during movie
// playback (PlaybackScreen::enter() runs `docker pause mdb_radarr`).
// The poll loop in run() skips calling this when state.video_active is
// true, so a paused Radarr never trips the false-alarm path. We can't
// switch to Gluetun's /v1/publicip/ip endpoint because Gluetun's control
// server (port 8000) isn't published to the host — only the four app
// ports (7878, 8080, 8191, 9696) are. Adding the gluetun port mapping
// would force a full stack restart on the next setup_services.sh run,
// not worth it for a signal that the video_active guard already fixes.
bool default_radarr_ping() {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:7878/ping");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);   // HEAD-equivalent; we don't read body
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK && http_code >= 200 && http_code < 300;
}

}  // namespace

VpnHealthMonitor::VpnHealthMonitor(app::AppState& state)
    : VpnHealthMonitor(state, &default_radarr_ping, std::chrono::seconds(10)) {}

VpnHealthMonitor::VpnHealthMonitor(app::AppState& state,
                                   PingFn ping_fn,
                                   std::chrono::milliseconds poll_interval,
                                   std::chrono::milliseconds post_session_grace)
    : state_(state), ping_fn_(std::move(ping_fn)),
      poll_interval_(poll_interval),
      post_session_grace_(post_session_grace) {}

VpnHealthMonitor::~VpnHealthMonitor() {
    stop();
}

void VpnHealthMonitor::start() {
    if (worker_.joinable()) return;   // already running
    stop_flag_.store(false);
    worker_ = std::thread([this] { run(); });
}

void VpnHealthMonitor::stop() {
    stop_flag_.store(true);
    if (worker_.joinable()) worker_.join();
}

void VpnHealthMonitor::run() {
    while (!stop_flag_.load()) {
        // Skip the poll while the kiosk has intentionally quieted the
        // media stack: movie playback (PlaybackScreen::enter() pauses
        // Radarr) or a game session (GameQuietMode stops Radarr /
        // Prowlarr / Byparr; is_loading_game stays true for the whole
        // blocked RetroArch session). Polling then would time out and
        // the 3-strikes counter would flip the tunnel-unhealthy flag —
        // a false alarm. Skip cleanly: don't increment failures, don't
        // change state, and keep pushing the post-session grace
        // deadline so the freshly docker-start-ed containers get time
        // to come up after the session ends. Real tunnel failures are
        // detected once the grace expires.
        const bool session_active =
            state_.video_active.load() || state_.is_loading_game.load();
        if (session_active) {
            consecutive_failures_.store(0);
            grace_until_ =
                std::chrono::steady_clock::now() + post_session_grace_;
            std::this_thread::sleep_for(poll_interval_);
            continue;
        }

        bool ok = ping_fn_();
        if (ok) {
            // Recovery is instant — any successful poll flips healthy.
            consecutive_failures_.store(0);
            state_.media_browser_vpn_healthy = true;
            ever_healthy_.store(true);  // arm the 3-strikes path
            grace_until_ = {};          // healthy again — grace over
        } else if (std::chrono::steady_clock::now() < grace_until_) {
            // Radarr is still restarting after a game/movie session; a
            // failure here is expected, not a tunnel drop.
            consecutive_failures_.store(0);
        } else {
            int n = consecutive_failures_.fetch_add(1) + 1;
            // Three-strikes: flip unhealthy — BUT only after we've seen
            // at least one successful poll in this process lifetime.
            // Without this guard the kiosk shows "Tunnel down" for the
            // ~60s cold-boot gap between kiosk start and Radarr being
            // ready (docker stack lags behind kiosk boot by ~90s). Real
            // tunnel drops mid-session still fire the toast because by
            // then ever_healthy_ is true.
            if (n >= kFailureThreshold && ever_healthy_.load()) {
                state_.media_browser_vpn_healthy = false;
            }
        }
        std::this_thread::sleep_for(poll_interval_);
    }
}

}  // namespace media_browser
