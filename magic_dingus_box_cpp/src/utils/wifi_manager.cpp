#include "wifi_manager.h"
#include <array>
#include <memory>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <thread>
#include <cstdio>

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
    // Check if nmcli exists
    std::string output = exec_command("which nmcli");
    return !output.empty();
}

void WifiManager::scan_networks_async() {
    if (is_scanning_) {
        std::cout << "WifiManager: Scan already in progress" << std::endl;
        return;
    }
    
    std::cout << "WifiManager: Starting async scan..." << std::endl;
    is_scanning_ = true;
    std::thread([this]() {
        // -f: fields (SSID, SIGNAL, SECURITY, IN-USE)
        // -t: terse (colon separated, escaping)
        // dev wifi list
        std::string cmd = "nmcli -t -f SSID,SIGNAL,SECURITY,IN-USE dev wifi list --rescan yes 2>&1";
        std::string output = exec_command(cmd.c_str());
        
        std::cout << "WifiManager: Scan complete. Output length: " << output.length() << std::endl;
        // std::cout << "Raw output: " << output << std::endl; // Uncomment if needed, might be spammy
        
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
    if (is_connecting_) return;
    
    is_connecting_ = true;
    connection_result_ = ConnectionResult::IN_PROGRESS;
    
    std::thread([this, ssid, password]() {
        std::string cmd;
        
        // Delete existing connections with matching SSID to avoid "property is missing" errors
        // Use UUIDs to avoid ambiguity with names containing spaces
        std::string list_out = exec_command("sudo nmcli -t -f UUID,NAME connection show");
        std::istringstream stream(list_out);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.empty()) continue;
            
            // Format is UUID:NAME
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            
            std::string uuid = line.substr(0, colon);
            std::string name = line.substr(colon + 1);
            
            // Query SSID for this connection using UUID
            std::string query = "sudo nmcli -t -f 802-11-wireless.ssid connection show " + uuid + " 2>/dev/null";
            std::string conn_ssid = exec_command(query.c_str());
            
            // Trim newline
            conn_ssid.erase(std::remove(conn_ssid.begin(), conn_ssid.end(), '\n'), conn_ssid.end());
            
            if (conn_ssid == ssid) {
                std::cout << "WifiManager: Deleting stale connection '" << name << "' (UUID: " << uuid << ") matches SSID '" << ssid << "'" << std::endl;
                std::string del = "sudo nmcli connection delete " + uuid;
                exec_command(del.c_str());
            }
        }

        if (password.empty()) {
            // Open network
            cmd = "sudo nmcli dev wifi connect \"" + ssid + "\"";
        } else {
            // Secure network
            cmd = "sudo nmcli dev wifi connect \"" + ssid + "\" password \"" + password + "\"";
        }
        
        std::cout << "WifiManager: Executing connect command: " << cmd << std::endl;
        std::string output = exec_command(cmd.c_str());
        std::cout << "WifiManager: Connect output: " << output << std::endl;
        
        // nmcli output contains "successfully activated" on success
        if (output.find("successfully activated") != std::string::npos) {
            std::cout << "WifiManager: Connection SUCCESS detected" << std::endl;
            
            // Explicitly ensure autoconnect is enabled for persistence
            // Usually default, but we enforce it per user request
            std::string auto_cmd = "sudo nmcli connection modify id \"" + ssid + "\" connection.autoconnect yes 2>/dev/null";
            exec_command(auto_cmd.c_str());
            std::cout << "WifiManager: Enforced autoconnect=yes for '" << ssid << "'" << std::endl;
            
            connection_result_ = ConnectionResult::SUCCESS;
        } else {
            std::cerr << "WifiManager: Connection FAILURE detected from output" << std::endl;
            // Additional check: maybe it's already connected?
            if (output.find("Error: Connection activation failed") != std::string::npos) {
                 std::cerr << "WifiManager: Activation failed (wrong password?)" << std::endl;
            }
            connection_result_ = ConnectionResult::FAILURE;
        }
        
        is_connecting_ = false;
    }).detach();
}

void WifiManager::reset_connection_state() {
    is_connecting_ = false;
    connection_result_ = ConnectionResult::SUCCESS; // Reset to benign state
}

std::string WifiManager::get_current_ssid() {
    // Get active connection on wifi device
    // nmcli -t -f NAME connection show --active
    // This might show UUIDs or other connections, simpler to look at wifi status
    std::string output = exec_command("nmcli -t -f GENERAL.CONNECTION dev show wlan0 2>/dev/null");
    if (output.empty()) {
        // Try simple scan of in-use
         output = exec_command("nmcli -t -f SSID,IN-USE dev wifi list | grep ':*'");
         // Only useful if we parse.
         // Let's stick to device show
         // Fallback if wlan0 isn't the interface name?
         // Try general active connections of type wifi
         output = exec_command("nmcli -t -f NAME,TYPE connection show --active | grep ':802-11-wireless' | cut -d: -f1");
    }
    
    // Clean up output (remove newlines)
    output.erase(std::remove(output.begin(), output.end(), '\n'), output.end());
    // remove "GENERAL.CONNECTION:" prefix if present from device show
    size_t colon = output.find(':');
    if (colon != std::string::npos) {
        output = output.substr(colon + 1);
    }
    
    return output;
}

std::string WifiManager::get_ip_address() {
    std::string output = exec_command("hostname -I | cut -d' ' -f1"); // quick hack
    output.erase(std::remove(output.begin(), output.end(), '\n'), output.end());
    return output;
}

bool WifiManager::is_connected() {
    // Check global connectivity
    std::string output = exec_command("nmcli -t -f CONNECTIVITY general");
    // full, limited
    return (output.find("full") != std::string::npos || output.find("limited") != std::string::npos);
}

bool WifiManager::forget_network(const std::string& ssid) {
    // Use sudo to ensure we can delete root-owned profiles
    std::string cmd = "sudo nmcli connection delete \"" + ssid + "\"";
    std::string output = exec_command(cmd.c_str());
    return (output.find("successfully") != std::string::npos);
}

std::string WifiManager::exec_command(const char* cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    if (!pipe) {
        return "";
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

std::vector<WifiNetwork> WifiManager::parse_nmcli_scan_output(const std::string& output) {
    std::vector<WifiNetwork> networks;
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
        net.saved = false; // We don't know from scan if it's saved without checking connections
        
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
