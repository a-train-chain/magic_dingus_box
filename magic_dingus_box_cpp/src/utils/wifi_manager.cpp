#include "wifi_manager.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <set>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>
#include <chrono>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>

namespace utils {

WifiManager& WifiManager::instance() {
    static WifiManager instance;
    return instance;
}

WifiManager::WifiManager() 
    : is_scanning_(false)
    , is_connecting_(false)
    , connection_result_(ConnectionResult::SUCCESS) // Default state
{
}

WifiManager::~WifiManager() {
}

bool WifiManager::initialize() {
    std::string output = exec_command_argv({"which", "nmcli"});
    return !output.empty();
}

void WifiManager::scan_networks_async() {
    // Atomically claim the scanning slot. The previous check-then-set
    // pattern (`if (is_scanning_) return; is_scanning_ = true;`) had a
    // window where two rapid callers could both observe false and each
    // spawn a detached thread, doubling up nmcli rescans and racing on
    // scan_results_ writes.
    bool expected = false;
    if (!is_scanning_.compare_exchange_strong(expected, true)) {
        std::cout << "WifiManager: Scan already in progress" << std::endl;
        return;
    }

    std::cout << "WifiManager: Starting async scan..." << std::endl;
    std::thread([this]() {
        // Two-phase scan. The previous single-call form
        //   `nmcli dev wifi list --rescan yes`
        // was supposed to trigger a rescan AND block until results were
        // ready, but on this Pi 4 + Pi OS Bookworm + NetworkManager 1.42
        // it returns cached results immediately and the rescan happens
        // in the background — so the kiosk only ever saw networks
        // already in nmcli's cache (typically just the currently-connected
        // SSID). Symptom: "Scan Networks" shows 1 entry instead of 10+.
        //
        // Fix: explicit `rescan` command (which kicks off the scan and
        // returns once it has been scheduled), then a brief wait for
        // the scan to complete, then a plain `list` to read the fresh
        // results. 4s is the empirically-reliable wait on a Pi 4 with
        // the bcm43436 chip; some scans complete in 2s but tail
        // outliers can take 3.5s, so 4s gives margin without making the
        // UI feel laggy.
        //
        // sudo on the rescan is required by the policykit config on
        // recent Pi OS (the magic user can't request a rescan without
        // it). The list command does NOT need sudo.

        // Phase 1: trigger rescan
        std::string rescan_out = exec_command_argv({"sudo", "nmcli", "dev", "wifi", "rescan"});
        if (!rescan_out.empty()) {
            std::cout << "WifiManager: rescan: " << rescan_out << std::endl;
        }

        // Phase 2: wait for scan to complete
        std::this_thread::sleep_for(std::chrono::seconds(4));

        // Phase 3: read fresh results
        std::string output = exec_command_argv({"nmcli", "-t", "-f", "SSID,SIGNAL,SECURITY,IN-USE",
                                                "dev", "wifi", "list"});

        std::cout << "WifiManager: Scan complete. Output length: " << output.length() << std::endl;

        auto networks = parse_nmcli_scan_output(output);
        std::cout << "WifiManager: Parsed " << networks.size() << " networks." << std::endl;

        {
            std::lock_guard<std::mutex> lock(scan_mutex_);
            scan_results_ = networks;
        }

        is_scanning_ = false;
        std::cout << "WifiManager: is_scanning_ set to false" << std::endl;
    }).detach();
}

std::vector<WifiNetwork> WifiManager::get_scan_results() {
    std::lock_guard<std::mutex> lock(scan_mutex_);
    return scan_results_;
}

void WifiManager::connect_async(const std::string& ssid, const std::string& password) {
    // Atomic claim — see scan_networks_async() for rationale on CAS.
    bool expected = false;
    if (!is_connecting_.compare_exchange_strong(expected, true)) return;

    // Capture the target SSID so the UI can render "Connecting to <SSID>..."
    // immediately, before the worker thread even fires off nmcli. Cleared at
    // the end of the worker (whether the connection succeeded or failed).
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        connecting_ssid_ = ssid;
    }
    connection_result_ = ConnectionResult::IN_PROGRESS;

    std::thread([this, ssid, password]() {
        // Clear previous error
        {
            std::lock_guard<std::mutex> lock(error_mutex_);
            connection_error_.clear();
        }

        // Two distinct flows here, gated on whether the caller passed a
        // password:
        //
        //   - Empty password  → "reconnect to saved profile" (user
        //     clicked a network labeled "Saved" in the UI; no keyboard
        //     prompt was shown). Use the existing profile and its
        //     stored PSK via `connection up id <ssid>`. Do NOT delete
        //     anything first: the previous version of this code
        //     unconditionally wiped the matching profile, then ran
        //     `dev wifi connect` with no password, producing a brand-
        //     new no-PSK profile and a no-secrets activation failure.
        //     Every reconnect attempt destroyed the working profile
        //     and replaced it with a broken one, then reported "Wrong
        //     password" to the operator. Operator had no way to
        //     recover without forgetting + re-typing.
        //
        //   - Non-empty password → "fresh credentials" (operator typed
        //     a password on the keyboard). Existing behavior: clear
        //     out any stale profile with the same SSID (avoids
        //     "property is missing" errors from partial profiles),
        //     then activate via `dev wifi connect <ssid> password <p>`.
        std::string output;
        if (password.empty()) {
            std::cout << "WifiManager: Reconnect (saved profile) — activating '"
                      << ssid << "' via stored PSK" << std::endl;
            output = exec_command_argv({"sudo", "nmcli", "connection", "up", "id", ssid},
                                       CONNECTION_TIMEOUT_SECONDS);
        } else {
            // Delete existing connections with matching SSID to avoid "property is missing" errors
            std::string list_out = exec_command_argv({"sudo", "nmcli", "-t", "-f", "UUID,NAME", "connection", "show"});
            std::istringstream stream(list_out);
            std::string line;
            while (std::getline(stream, line)) {
                if (line.empty()) continue;

                size_t colon = line.find(':');
                if (colon == std::string::npos) continue;

                std::string uuid = line.substr(0, colon);

                std::string conn_ssid = exec_command_argv({"sudo", "nmcli", "-t", "-f",
                                                            "802-11-wireless.ssid", "connection", "show", uuid});
                conn_ssid.erase(std::remove(conn_ssid.begin(), conn_ssid.end(), '\n'), conn_ssid.end());

                // nmcli outputs "802-11-wireless.ssid:ActualSSID"
                size_t ssid_colon = conn_ssid.find(':');
                if (ssid_colon != std::string::npos) {
                    conn_ssid = conn_ssid.substr(ssid_colon + 1);
                }

                if (conn_ssid == ssid) {
                    std::string name = line.substr(colon + 1);
                    std::cout << "WifiManager: Deleting stale connection '" << name
                              << "' (UUID: " << uuid << ") matches SSID '" << ssid << "'" << std::endl;
                    exec_command_argv({"sudo", "nmcli", "connection", "delete", uuid});
                }
            }

            output = exec_command_argv({"sudo", "nmcli", "dev", "wifi", "connect", ssid, "password", password},
                                       CONNECTION_TIMEOUT_SECONDS);
        }

        std::cout << "WifiManager: Connecting to SSID: " << ssid << std::endl;
        std::cout << "WifiManager: Connect output: " << output << std::endl;

        if (output.find("successfully activated") != std::string::npos) {
            std::cout << "WifiManager: Connection SUCCESS detected" << std::endl;
            exec_command_argv({"sudo", "nmcli", "connection", "modify", "id", ssid, "connection.autoconnect", "yes"});
            connection_result_ = ConnectionResult::SUCCESS;
        } else {
            // Parse specific error
            std::string error_msg;
            if (output.empty()) {
                error_msg = "Connection timed out";
                connection_result_ = ConnectionResult::TIMEOUT;
            } else if (output.find("Secrets were required") != std::string::npos ||
                       output.find("secrets were required") != std::string::npos ||
                       output.find("No secrets") != std::string::npos) {
                // The PSK slot in the connection profile was empty.
                //   - Reconnect-to-saved path: the saved profile is
                //     corrupt (no PSK stored). Tell operator to forget
                //     it and re-enter the password.
                //   - Fresh-credentials path: nmcli would have stored
                //     the PSK we passed; if it still says no-secrets,
                //     the PSK was actually wrong → "Wrong password".
                if (password.empty()) {
                    error_msg = "Saved network missing password — forget and reconnect";
                } else {
                    error_msg = "Wrong password";
                }
                connection_result_ = ConnectionResult::FAILURE;
            } else if (output.find("unknown connection") != std::string::npos ||
                       output.find("Unknown connection") != std::string::npos) {
                // `connection up id <ssid>` couldn't find the profile.
                // The "Saved" indicator in the UI was stale; ask operator
                // to re-enter the password to create a fresh profile.
                error_msg = "No saved profile — re-enter password";
                connection_result_ = ConnectionResult::FAILURE;
            } else if (output.find("No network with SSID") != std::string::npos) {
                error_msg = "Network not found";
                connection_result_ = ConnectionResult::FAILURE;
            } else {
                // Extract first line of output for context
                std::string first_line = output.substr(0, output.find('\n'));
                if (first_line.length() > 60) first_line = first_line.substr(0, 60) + "...";
                error_msg = "Connection failed: " + first_line;
                connection_result_ = ConnectionResult::FAILURE;
            }

            std::cerr << "WifiManager: " << error_msg << std::endl;
            {
                std::lock_guard<std::mutex> lock(error_mutex_);
                connection_error_ = error_msg;
            }
        }

        // Clear the target SSID — connection attempt is over (success or fail).
        {
            std::lock_guard<std::mutex> lock(error_mutex_);
            connecting_ssid_.clear();
        }
        is_connecting_ = false;
    }).detach();
}

std::string WifiManager::get_connecting_ssid() const {
    // error_mutex_ is mutable so we can lock it from this const getter.
    std::lock_guard<std::mutex> lock(error_mutex_);
    return connecting_ssid_;
}

void WifiManager::reset_connection_state() {
    is_connecting_ = false;
    connection_result_ = ConnectionResult::SUCCESS; // Reset to benign state
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        connecting_ssid_.clear();
    }
}

std::string WifiManager::get_current_ssid() {
    std::string output = exec_command_argv({"nmcli", "-t", "-f", "GENERAL.CONNECTION", "dev", "show", "wlan0"});
    if (output.empty()) {
        // Fallback: get active wifi connections and filter in C++
        output = exec_command_argv({"nmcli", "-t", "-f", "NAME,TYPE", "connection", "show", "--active"});
        std::istringstream stream(output);
        std::string line;
        while (std::getline(stream, line)) {
            size_t colon = line.rfind(':');
            if (colon != std::string::npos && line.substr(colon + 1) == "802-11-wireless") {
                return line.substr(0, colon);
            }
        }
        return "";
    }

    output.erase(std::remove(output.begin(), output.end(), '\n'), output.end());
    size_t colon = output.find(':');
    if (colon != std::string::npos) {
        output = output.substr(colon + 1);
    }
    return output;
}

std::string WifiManager::get_ip_address() {
    // Read wlan0's IPv4 address specifically.
    //
    // The previous implementation called `hostname -I` and took the
    // FIRST token. That's wrong on a Pi with the USB-Gadget service
    // running: `hostname -I` orders IPs by interface index, and usb0
    // (index 3, statically assigned 10.55.0.1) consistently appears
    // BEFORE wlan0 (index 4). So the "Wi-Fi IP" came back as the USB
    // gadget IP, which then got baked into the Content Manager QR
    // code — sending phones to a 10.55.0.1 URL they couldn't reach.
    //
    // Walk getifaddrs and pick wlan0's IPv4 address directly. No
    // fallback to other interfaces — if wlan0 isn't there or has no
    // address, return empty and let the caller treat it as "Wi-Fi
    // not connected" rather than silently substitute some unrelated
    // interface.
    struct ifaddrs* ifap = nullptr;
    if (getifaddrs(&ifap) == -1) {
        return "";
    }
    std::string ip;
    for (struct ifaddrs* ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (std::strcmp(ifa->ifa_name, "wlan0") != 0) continue;
        char buf[INET_ADDRSTRLEN] = {0};
        auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) {
            ip = buf;
        }
        break;
    }
    freeifaddrs(ifap);
    return ip;
}

bool WifiManager::is_connected() {
    std::string output = exec_command_argv({"nmcli", "-t", "-f", "CONNECTIVITY", "general"});
    return (output.find("full") != std::string::npos || output.find("limited") != std::string::npos);
}

std::string WifiManager::get_connection_error() {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return connection_error_;
}

bool WifiManager::forget_network(const std::string& ssid) {
    // Two-step disconnect+forget. The previous single `connection delete`
    // is supposed to do both (kick the active connection AND remove the
    // saved profile), but if it silently fails (e.g. a stale connection
    // profile, a typo in SSID, polkit denial) the user sees nothing
    // happen on screen. Splitting the steps lets us:
    //   1. Cleanly disconnect first (network drops immediately — visible
    //      to the user)
    //   2. Then delete the profile (so it doesn't auto-reconnect)
    // ...and log each step's output so the journal shows what actually
    // happened. Both steps are best-effort; if "down" fails (e.g. wasn't
    // active) the "delete" still runs, which is the desired end state.
    std::cout << "WifiManager: forget_network('" << ssid << "')" << std::endl;
    if (ssid.empty()) {
        std::cout << "WifiManager: empty SSID — nothing to forget" << std::endl;
        return false;
    }

    // Step 1: bring the connection down (best-effort; harmless if it
    // wasn't active to begin with — `connection down` on an inactive
    // connection just prints a warning, doesn't error fatally).
    std::string down_out = exec_command_argv({"sudo", "nmcli", "connection", "down", ssid});
    std::cout << "WifiManager: 'down' result: " << (down_out.empty() ? "<empty>" : down_out) << std::endl;

    // Step 2: delete the saved profile so NetworkManager doesn't
    // auto-reconnect on the next sweep.
    std::string del_out = exec_command_argv({"sudo", "nmcli", "connection", "delete", ssid});
    std::cout << "WifiManager: 'delete' result: " << (del_out.empty() ? "<empty>" : del_out) << std::endl;

    // Success heuristic: "successfully deleted" appears in the delete
    // output on success. Localized environments may print differently;
    // accept a non-empty output without an "Error:" prefix as success.
    bool deleted = (del_out.find("successfully") != std::string::npos)
                || (!del_out.empty() && del_out.find("Error:") == std::string::npos
                                     && del_out.find("error:") == std::string::npos);
    std::cout << "WifiManager: forget_network → " << (deleted ? "OK" : "FAILED") << std::endl;
    return deleted;
}

std::string WifiManager::exec_command_argv(const std::vector<std::string>& args, int timeout_seconds) {
    if (args.empty()) return "";

    int pipefd[2];
    if (pipe(pipefd) == -1) return "";

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return "";
    }

    if (pid == 0) {
        // Child process
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        std::vector<const char*> argv;
        for (const auto& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);

        execvp(argv[0], const_cast<char* const*>(argv.data()));
        _exit(127);
    }

    // Parent process
    close(pipefd[1]);

    std::string result;
    char buffer[128];

    if (timeout_seconds > 0) {
        // Timeout-aware read using poll()
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
        struct pollfd pfd;
        pfd.fd = pipefd[0];
        pfd.events = POLLIN;

        bool timed_out = false;
        while (true) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                timed_out = true;
                break;
            }

            int ret = poll(&pfd, 1, static_cast<int>(remaining.count()));
            if (ret > 0 && (pfd.revents & POLLIN)) {
                ssize_t n = read(pipefd[0], buffer, sizeof(buffer) - 1);
                if (n <= 0) break; // EOF or error
                buffer[n] = '\0';
                result += buffer;
            } else if (ret == 0) {
                timed_out = true;
                break;
            } else {
                break; // poll error
            }
        }
        close(pipefd[0]);

        if (timed_out) {
            // Kill the child process
            kill(pid, SIGTERM);
            // Brief wait for graceful shutdown
            usleep(200000); // 200ms
            int status;
            if (waitpid(pid, &status, WNOHANG) == 0) {
                // Still alive, force kill
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
            }
            std::cerr << "WifiManager: Command timed out after " << timeout_seconds << "s" << std::endl;
            return ""; // Empty string signals timeout
        }

        int status;
        waitpid(pid, &status, 0);
    } else {
        // No timeout - blocking read
        ssize_t n;
        while ((n = read(pipefd[0], buffer, sizeof(buffer) - 1)) > 0) {
            buffer[n] = '\0';
            result += buffer;
        }
        close(pipefd[0]);

        int status;
        waitpid(pid, &status, 0);
    }

    return result;
}

std::vector<WifiNetwork> WifiManager::parse_nmcli_scan_output(const std::string& output) {
    std::vector<WifiNetwork> networks;

    // Build set of saved WiFi SSIDs for cross-referencing
    std::set<std::string> saved_ssids;
    std::string saved_output = exec_command_argv({"nmcli", "-t", "-f", "NAME,TYPE", "connection", "show"});
    std::istringstream saved_stream(saved_output);
    std::string saved_line;
    while (std::getline(saved_stream, saved_line)) {
        // Format: NAME:TYPE  (e.g., "MyWiFi:802-11-wireless")
        size_t colon = saved_line.rfind(':');
        if (colon != std::string::npos && saved_line.substr(colon + 1) == "802-11-wireless") {
            saved_ssids.insert(saved_line.substr(0, colon));
        }
    }

    std::istringstream stream(output);
    std::string line;

    // Format: SSID:SIGNAL:SECURITY:IN-USE
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        
        // Split by colon
        // NOTE: SSID can contain colons, so we need to be careful.
        // nmcli -t escapes colons in values with backslash? 
        // Actually -t mode is a bit simpler.
        // Let's assume standard output for now or simple parsing.
        // To be robust with SSID colons, we should probably parse from right to left?
        // IN-USE is last (char), SECURITY is second last, SIGNAL is int.
        
        // Find last colon for IN-USE
        size_t last_colon = line.rfind(':');
        if (last_colon == std::string::npos) continue;
        
        std::string in_use_str = line.substr(last_colon + 1);
        
        // Find second last colon for SECURITY
        size_t sec_colon = line.rfind(':', last_colon - 1);
        if (sec_colon == std::string::npos) continue;
        
        std::string security = line.substr(sec_colon + 1, last_colon - sec_colon - 1);
        
        // Find third last colon for SIGNAL
        size_t sig_colon = line.rfind(':', sec_colon - 1);
        if (sig_colon == std::string::npos) continue; // Empty SSID? nmcli shows SSID::... for hidden
        
        std::string signal_str = line.substr(sig_colon + 1, sec_colon - sig_colon - 1);
        
        // SSID is everything before signal
        std::string ssid = line.substr(0, sig_colon);
        
        // Unescape SSID if needed (nmcli escapes colons as \:)
        // Simple replace for now
        
        // Filter empty SSIDs (hidden networks) if desired
        if (ssid.empty()) continue; // keep them? UI might look weird. Skip for now.
        if (ssid.empty()) continue;
        
        WifiNetwork net;
        net.ssid = ssid;
        try {
            net.signal_strength = std::stoi(signal_str);
        } catch (...) {
            net.signal_strength = 0;
        }
        net.security = security;
        net.in_use = (in_use_str == "*");
        net.saved = saved_ssids.count(ssid) > 0;
        
        // Deduplicate: nmcli returns multiple BSSIDs for same SSID
        bool found = false;
        for (auto& existing : networks) {
            if (existing.ssid == net.ssid) {
                // Keep the one with stronger signal or connected
                if (net.in_use) existing.in_use = true;
                if (net.signal_strength > existing.signal_strength) {
                    existing.signal_strength = net.signal_strength;
                    existing.security = net.security;
                }
                found = true;
                break;
            }
        }
        
        if (!found) {
            networks.push_back(net);
        }
    }
    
    // Sort by signal strength (descending)
    std::sort(networks.begin(), networks.end(), [](const WifiNetwork& a, const WifiNetwork& b) {
        // Put connected network first
        if (a.in_use != b.in_use) return a.in_use;
        return a.signal_strength > b.signal_strength;
    });
    
    return networks;
}

} // namespace utils
