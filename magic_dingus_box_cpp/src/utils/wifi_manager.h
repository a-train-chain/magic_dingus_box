#pragma once

#include <string>
#include <vector>
#include <future>
#include <mutex>
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

    // Async connection
    void connect_async(const std::string& ssid, const std::string& password);
    ConnectionResult get_connection_result() const { return connection_result_; }
    bool is_connecting() const { return is_connecting_; }
    void reset_connection_state();

    // Error details from last connection attempt
    std::string get_connection_error();

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
    std::string exec_command_argv(const std::vector<std::string>& args, int timeout_seconds = 0);

    std::vector<WifiNetwork> parse_nmcli_scan_output(const std::string& output);

    // Thread safety
    std::mutex scan_mutex_;
    std::vector<WifiNetwork> scan_results_;
    std::atomic<bool> is_scanning_;

    std::atomic<bool> is_connecting_;
    std::atomic<ConnectionResult> connection_result_;

    std::string connection_error_;
    std::mutex error_mutex_;
};

} // namespace utils
