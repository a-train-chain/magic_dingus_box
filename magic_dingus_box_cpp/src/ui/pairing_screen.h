#pragma once
#include <chrono>
#include <string>
#include <vector>

namespace ui {

// Generates and persists short-lived pairing codes for the Phone Remote.
// Lifecycle:
//   - regenerate() — writes a fresh 6-digit code + nonce to the session file
//   - tick() — call ~1 Hz while the screen is open; auto-regens on Flask-side
//     deletion (5 wrong attempts) or on local expiry
//   - close() — removes the session file when the user leaves the screen
class PairingScreen {
public:
    explicit PairingScreen(std::string session_path);

    void regenerate();
    void tick();
    void close();

    const std::string& current_code() const { return code_; }
    std::chrono::system_clock::time_point expires_at() const { return expires_at_; }

    // Move selection up/down through the paired-devices list. n is the
    // current device count; clamps the index. Wraps around.
    void select_next_device(int n_devices);
    void select_prev_device(int n_devices);
    int selected_device_index() const { return selected_device_; }

    // Append the given device_id to data/pending_revocations.txt for Flask
    // to drain. Caller passes the list of currently-displayed device_ids;
    // we use selected_device_index() to pick which one.
    void forget_selected_device(const std::vector<std::string>& device_ids);

private:
    std::string session_path_;
    std::string tmp_path_;
    std::string code_;
    std::string nonce_;
    std::chrono::system_clock::time_point issued_at_;
    std::chrono::system_clock::time_point expires_at_;
    int selected_device_ = 0;

    void write_atomic_();
    static std::string generate_code_();
    static std::string generate_nonce_();
};

}  // namespace ui
