#pragma once

#include <string>
#include <vector>
#include <future>
#include <mutex>
#include <thread>
#include <atomic>

namespace utils {

struct WifiNetwork {
    std::string ssid;
    int signal_strength; // 0-100
    std::string security; // e.g. "WPA2"
    bool in_use;
    bool saved;
};

// Simple async result for connection attempts
enum class ConnectionResult {
    SUCCESS,
    FAILURE,
    TIMEOUT,
    IN_PROGRESS
};

class WifiManager {
public:
    static constexpr int CONNECTION_TIMEOUT_SECONDS = 30;

    static WifiManager& instance();

    // Initialize (check nmcli availability)
    bool initialize();

    // Async scanning
    void scan_networks_async();
    bool is_scanning() const { return is_scanning_; }

    // Get latest scan results (thread-safe)
    std::vector<WifiNetwork> get_scan_results();

    // Async connection. `fresh_credentials` selects the flow:
    //   true  → build a new profile via `dev wifi connect` (password may
    //           legitimately be empty for OPEN networks; any stale profile
    //           with the same SSID is deleted first).
    //   false → activate the existing saved profile via `connection up`.
    // The old two-arg call inferred the flow from password.empty(), which
    // made unsaved OPEN networks take the saved-profile path and fail
    // with "unknown connection".
    void connect_async(const std::string& ssid, const std::string& password,
                       bool fresh_credentials);
    ConnectionResult get_connection_result() const { return connection_result_; }
    bool is_connecting() const { return is_connecting_; }
    // SSID currently being connected to. Empty when no connect is in flight.
    // Used by the settings menu to render "Connecting to <SSID>..." in both
    // the parent Wi-Fi submenu and Toast notifications, instead of a generic
    // "Connecting..." that lost the operator's selection context.
    std::string get_connecting_ssid() const;
    void reset_connection_state();

    // Error details from last connection attempt
    std::string get_connection_error();

    // SSID of the most recent FAILED attempt + whether that failure came
    // from the saved-profile path (stale/corrupt profile). Lets the UI
    // offer "Enter New Password" recovery instead of the old dead-end
    // ("forget and reconnect" with no way to do either). Cleared by
    // reset_connection_state().
    std::string get_last_failed_ssid() const;
    bool last_failure_was_saved_profile() const { return last_failed_saved_; }

    // Status
    std::string get_current_ssid();
    std::string get_ip_address();
    bool is_connected();

    // Forget network
    bool forget_network(const std::string& ssid);

private:
    WifiManager();
    ~WifiManager();

    // Execute command with argument vector (no shell injection)
    // timeout_seconds: if > 0, kill child after this many seconds
    // Default timeout 15s: every caller here runs nmcli/which, none of
    // which legitimately takes longer. The old default of 0 (no timeout)
    // meant a wedged NetworkManager D-Bus call blocked the calling
    // thread FOREVER — on the scan/connect workers that permanently
    // latched is_scanning_/is_connecting_ (the UI showed "scanning..."
    // until reboot), and on render-thread callers it froze the kiosk
    // into the systemd watchdog. Callers with tighter or looser needs
    // still pass their own value.
    std::string exec_command_argv(const std::vector<std::string>& args, int timeout_seconds = 15);

    std::vector<WifiNetwork> parse_nmcli_scan_output(const std::string& output);

    // Thread safety
    std::mutex scan_mutex_;
    std::vector<WifiNetwork> scan_results_;
    std::atomic<bool> is_scanning_;

    // Worker threads are TRACKED, not detached: a detached worker
    // capturing `this` could outlive the singleton at process exit and
    // publish into freed members. The is_scanning_/is_connecting_ CAS
    // gates serialize each thread's use; joins happen before respawn
    // (instant — the gate guarantees the old thread finished) and in
    // the destructor (bounded by exec_command_argv's timeouts).
    std::thread scan_thread_;
    std::thread connect_thread_;

    std::atomic<bool> is_connecting_;
    std::atomic<ConnectionResult> connection_result_;

    std::string connection_error_;
    // mutable: get_connecting_ssid() is logically const but locks this mutex.
    mutable std::mutex error_mutex_;

    // SSID being connected to (set at connect_async() entry, cleared after
    // the worker thread publishes its result). Read concurrently from the
    // render thread, so guarded by the same mutex as connection_error_.
    std::string connecting_ssid_;

    // Recovery context for the UI (guarded by error_mutex_ / atomic).
    std::string last_failed_ssid_;
    std::atomic<bool> last_failed_saved_{false};
};

} // namespace utils
