#include "pairing_screen.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <json/json.h>
#include <cerrno>
#include "../utils/logger.h"

namespace ui {

namespace {
constexpr auto kCodeWindow = std::chrono::seconds(120);
constexpr int kInitialAttempts = 5;

// Wraps std::random_device + a fallback std::mt19937. On Linux
// std::random_device reads from /dev/urandom and can throw if entropy is
// unavailable (extremely unlikely on a Pi 4B running Bookworm — the kernel
// has a hardware RNG and seeds urandom very early). If construction or
// operator() ever throws, we fall back to a time-seeded mt19937 so the
// kiosk doesn't crash. The fallback is NOT cryptographically suitable, but
// the pairing-code threat model (6-digit code, 120 s validity, 5 attempts,
// LAN only) tolerates a temporarily weaker RNG.
uint32_t safe_random_u32() {
    static std::mt19937 fallback{
        static_cast<uint32_t>(
            std::chrono::system_clock::now().time_since_epoch().count())};
    try {
        static std::random_device r;
        return r();
    } catch (...) {
        LOG_WARN("[pairing] std::random_device threw; using mt19937 fallback");
        return fallback();
    }
}
}  // namespace

PairingScreen::PairingScreen(std::string session_path)
    : session_path_(std::move(session_path)),
      tmp_path_(session_path_ + ".tmp") {}

std::string PairingScreen::generate_code_() {
    int v = static_cast<int>(safe_random_u32() % 1000000u);
    char buf[7];
    std::snprintf(buf, sizeof(buf), "%06d", v);
    return std::string(buf);
}

std::string PairingScreen::generate_nonce_() {
    std::ostringstream s;
    s << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i) s << std::setw(8) << safe_random_u32();
    return s.str();  // 32 hex chars = 128 bits
}

void PairingScreen::regenerate() {
    code_ = generate_code_();
    nonce_ = generate_nonce_();
    issued_at_ = std::chrono::system_clock::now();
    expires_at_ = issued_at_ + kCodeWindow;
    write_atomic_();
}

void PairingScreen::tick() {
    namespace fs = std::filesystem;
    if (code_.empty()) {
        regenerate();
        return;
    }
    if (std::chrono::system_clock::now() >= expires_at_) {
        regenerate();
        return;
    }
    if (!fs::exists(session_path_)) {
        // Flask deleted it (5 wrong attempts); roll a fresh code.
        regenerate();
    }
}

void PairingScreen::close() {
    std::filesystem::remove(session_path_);
    code_.clear();
    nonce_.clear();
}

void PairingScreen::select_next_device(int n) {
    if (n <= 0) { selected_device_ = 0; return; }
    selected_device_ = (selected_device_ + 1) % n;
}

void PairingScreen::select_prev_device(int n) {
    if (n <= 0) { selected_device_ = 0; return; }
    selected_device_ = (selected_device_ - 1 + n) % n;
}

void PairingScreen::forget_selected_device(const std::vector<std::string>& ids) {
    if (selected_device_ < 0 || selected_device_ >= (int)ids.size()) return;
    namespace fs = std::filesystem;
    fs::path data_dir = fs::path(session_path_).parent_path();
    std::ofstream f((data_dir / "pending_revocations.txt").string(), std::ios::app);
    f << ids[selected_device_] << "\n";
    // Move selection up if we just removed the last item — caller will
    // re-load the list, but reset to a safe index for the next render.
    if (selected_device_ >= (int)ids.size() - 1 && selected_device_ > 0) {
        --selected_device_;
    }
}

void PairingScreen::write_atomic_() {
    Json::Value root;
    root["schema"] = 1;
    root["code"] = code_;
    root["issued_at"] = static_cast<Json::Int64>(
        std::chrono::duration_cast<std::chrono::seconds>(
            issued_at_.time_since_epoch()).count());
    root["expires_at"] = static_cast<Json::Int64>(
        std::chrono::duration_cast<std::chrono::seconds>(
            expires_at_.time_since_epoch()).count());
    root["attempts_remaining"] = kInitialAttempts;
    root["nonce"] = nonce_;

    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    std::string out = Json::writeString(b, root);

    {
        std::ofstream f(tmp_path_, std::ios::binary | std::ios::trunc);
        f.write(out.data(), static_cast<std::streamsize>(out.size()));
        if (!f.good()) {
            LOG_WARN("[pairing] failed to write {}", tmp_path_);
            std::remove(tmp_path_.c_str());
            return;
        }
    }
    if (std::rename(tmp_path_.c_str(), session_path_.c_str()) != 0) {
        LOG_WARN("[pairing] rename {} -> {} failed (errno {})",
                 tmp_path_, session_path_, errno);
        std::remove(tmp_path_.c_str());
    }
}

}  // namespace ui
