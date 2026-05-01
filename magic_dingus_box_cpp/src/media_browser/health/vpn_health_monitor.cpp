// src/media_browser/health/vpn_health_monitor.cpp
#include "media_browser/health/vpn_health_monitor.h"

#include "app/app_state.h"

#include <curl/curl.h>
#include <spdlog/spdlog.h>

namespace media_browser {

namespace {

// Default ping: GET http://127.0.0.1:7878/ping with 3s timeout.
// Returns true on HTTP 2xx, false on any error or non-2xx.
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
                                   std::chrono::milliseconds poll_interval)
    : state_(state), ping_fn_(std::move(ping_fn)),
      poll_interval_(poll_interval) {}

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
        bool ok = ping_fn_();
        if (ok) {
            // Recovery is instant — any successful poll flips healthy.
            consecutive_failures_.store(0);
            state_.media_browser_vpn_healthy = true;
        } else {
            int n = consecutive_failures_.fetch_add(1) + 1;
            if (n >= kFailureThreshold) {
                // Three-strikes: flip unhealthy.
                state_.media_browser_vpn_healthy = false;
            }
        }
        std::this_thread::sleep_for(poll_interval_);
    }
}

}  // namespace media_browser
