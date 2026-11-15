#include "bt_pairing.h"

#include <algorithm>
#include <cctype>

namespace platform::bt {

namespace {

constexpr uint32_t kMajorPeripheral = 0x05;
constexpr uint32_t kSubJoystick = 0x01;
constexpr uint32_t kSubGamepad = 0x02;
constexpr uint16_t kAppearanceJoystick = 0x03c3;
constexpr uint16_t kAppearanceGamepad = 0x03c4;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool contains_error(const std::string& error, const char* name) {
    return error.find(name) != std::string::npos;
}

std::string display_name(const BtDevice& d) {
    return d.name.empty() ? std::string("wireless controller") : d.name;
}

}  // namespace

bool is_gamepad_candidate(const BtDevice& d) {
    // BlueZ's icon is derived from the class / appearance. When it has one,
    // it is authoritative either way.
    if (d.icon == "input-gaming") return true;
    if (!d.icon.empty()) return false;

    if (d.cls != 0) {
        const uint32_t major = (d.cls >> 8) & 0x1f;
        const uint32_t sub = (d.cls >> 2) & 0x0f;
        return major == kMajorPeripheral && (sub == kSubJoystick || sub == kSubGamepad);
    }
    if (d.appearance != 0) {
        return d.appearance == kAppearanceJoystick || d.appearance == kAppearanceGamepad;
    }
    // Nothing but a name (some pads advertise no class in pairing mode).
    const std::string n = lower(d.name);
    for (const char* hint : {"controller", "gamepad", "joy-con", "8bitdo", "dualsense",
                             "dualshock", "xbox"}) {
        if (n.find(hint) != std::string::npos) return true;
    }
    return false;
}

std::optional<std::pair<uint16_t, uint16_t>> parse_modalias_ids(const std::string& m) {
    // "<source>:vXXXXpXXXXdXXXX"
    const auto colon = m.find(':');
    if (colon == std::string::npos) return std::nullopt;
    const std::string rest = m.substr(colon + 1);
    if (rest.size() < 10 || rest[0] != 'v' || rest[5] != 'p') return std::nullopt;
    auto hex4 = [](const std::string& s, uint16_t* out) {
        if (s.size() != 4) return false;
        unsigned v = 0;
        for (char c : s) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (!std::isxdigit(u)) return false;
            v = (v << 4) | (std::isdigit(u) ? u - '0' : std::tolower(u) - 'a' + 10);
        }
        *out = static_cast<uint16_t>(v);
        return true;
    };
    uint16_t vid = 0, pid = 0;
    if (!hex4(rest.substr(1, 4), &vid) || !hex4(rest.substr(6, 4), &pid)) {
        return std::nullopt;
    }
    return std::make_pair(vid, pid);
}

// ---------------------------------------------------------------------------
// PairingFlow
// ---------------------------------------------------------------------------

PairingFlow::PairingFlow(Clock::time_point now) : scan_started_(now), step_started_(now) {}

std::vector<BtCommand> PairingFlow::take_commands() {
    std::vector<BtCommand> out;
    out.swap(pending_);
    return out;
}

std::optional<std::pair<uint16_t, uint16_t>> PairingFlow::device_ids() const {
    return parse_modalias_ids(modalias_);
}

void PairingFlow::start_scan_(Clock::time_point now) {
    phase_ = Phase::SCANNING;
    scan_requested_ = true;
    scan_started_ = now;
    searching_a_while_ = false;
    pending_.push_back({BtCommandType::START_SCAN, ""});
}

void PairingFlow::fail_(const std::string& message) {
    phase_ = Phase::FAILED;
    message_ = message;
    removing_.clear();
    // Leave the radio quiet while the screen waits for a retry.
    pending_.push_back({BtCommandType::STOP_SCAN, ""});
}

void PairingFlow::pair_failed_(Clock::time_point now) {
    ++pair_failures_;
    if (pair_failures_ >= kMaxPairFailures) {
        fail_("Couldn't pair with " + name_ +
              ". Put it in pairing mode again, then try again.");
        return;
    }
    message_ = "Couldn't pair - put the controller back in pairing mode";
    path_.clear();
    start_scan_(now);
}

void PairingFlow::retry(Clock::time_point now) {
    if (phase_ != Phase::FAILED) return;
    pair_failures_ = 0;
    connect_failed_ = false;
    removing_.clear();
    path_.clear();
    name_.clear();
    modalias_.clear();
    message_.clear();
    start_scan_(now);
}

void PairingFlow::on_snapshot(const BtSnapshot& s, Clock::time_point now) {
    if (phase_ == Phase::UNAVAILABLE || phase_ == Phase::CONNECTED ||
        phase_ == Phase::FAILED) {
        return;
    }

    switch (s.adapter) {
        case BtAdapterState::UNKNOWN:
            return;
        case BtAdapterState::NO_BLUETOOTH:
            phase_ = Phase::UNAVAILABLE;
            message_ = "Bluetooth isn't available on this box";
            return;
        case BtAdapterState::BLOCKED:
            phase_ = Phase::UNAVAILABLE;
            message_ = "Bluetooth is switched off on this box";
            return;
        case BtAdapterState::OFF:
            if (!power_requested_) {
                power_requested_ = true;
                pending_.push_back({BtCommandType::POWER_ON, ""});
            }
            return;
        case BtAdapterState::ON:
            break;
    }

    if (phase_ == Phase::STARTING) {
        if (!scan_requested_) start_scan_(now);
        return;
    }

    // Keep what we know about the chosen pad fresh (its name resolves late).
    for (const BtDevice& d : s.devices) {
        if (d.path != path_ || path_.empty()) continue;
        if (!d.name.empty()) name_ = d.name;
        if (!d.modalias.empty()) modalias_ = d.modalias;
        if (phase_ == Phase::CONNECTING && d.connected) {
            phase_ = Phase::CONNECTED;
            message_.clear();
        }
    }

    if (phase_ != Phase::SCANNING || !removing_.empty()) return;

    const BtDevice* best = nullptr;
    for (const BtDevice& d : s.devices) {
        if (!is_gamepad_candidate(d) || !d.has_rssi || d.connected) continue;
        if (!best || d.rssi > best->rssi) best = &d;
    }
    if (!best) return;

    name_ = display_name(*best);
    if (best->paired && removed_.count(best->path) == 0) {
        // Remembered from an older pairing; its keys are gone from the pad.
        removing_ = best->path;
        pending_.push_back({BtCommandType::REMOVE, best->path});
        return;
    }
    path_ = best->path;
    modalias_ = best->modalias;
    message_.clear();
    phase_ = Phase::PAIRING;
    step_started_ = now;
    // Discovery competes with pairing for the radio (one combo chip shares
    // it with Wi-Fi on both boards); stop it first.
    pending_.push_back({BtCommandType::STOP_SCAN, ""});
    pending_.push_back({BtCommandType::PAIR, path_});
}

void PairingFlow::on_result(const BtResult& r, Clock::time_point now) {
    switch (r.type) {
        case BtCommandType::POWER_ON:
            if (!r.ok && phase_ == Phase::STARTING) {
                phase_ = Phase::UNAVAILABLE;
                message_ = "Couldn't turn Bluetooth on";
            }
            return;

        case BtCommandType::START_SCAN:
            if (!r.ok && phase_ == Phase::SCANNING &&
                !contains_error(r.error, "InProgress")) {
                fail_("Couldn't start looking for controllers");
            }
            return;

        case BtCommandType::STOP_SCAN:
        case BtCommandType::TRUST:
            // A failed TRUST only costs automatic reconnects; the connect
            // that follows still tells us whether the pad works now.
            return;

        case BtCommandType::REMOVE:
            if (r.device_path != removing_) return;
            removing_.clear();
            removed_.insert(r.device_path);
            return;

        case BtCommandType::PAIR:
            if (phase_ != Phase::PAIRING || r.device_path != path_) return;
            if (r.ok || contains_error(r.error, "AlreadyExists")) {
                phase_ = Phase::CONNECTING;
                step_started_ = now;
                connect_failed_ = false;
                pending_.push_back({BtCommandType::TRUST, path_});
                pending_.push_back({BtCommandType::CONNECT, path_});
            } else {
                pair_failed_(now);
            }
            return;

        case BtCommandType::CONNECT:
            if (phase_ != Phase::CONNECTING || r.device_path != path_) return;
            if (r.ok || contains_error(r.error, "AlreadyConnected")) {
                phase_ = Phase::CONNECTED;
                message_.clear();
            } else {
                // The pad often connects itself right after pairing; give
                // it a moment before calling this a failure.
                connect_failed_ = true;
                step_started_ = now;
            }
            return;
    }
}

void PairingFlow::tick(Clock::time_point now) {
    switch (phase_) {
        case Phase::SCANNING:
            if (now - scan_started_ >= kHintAfter) searching_a_while_ = true;
            return;
        case Phase::PAIRING:
            if (now - step_started_ >= kPairTimeout) {
                fail_(name_ + " didn't answer. Put it in pairing mode again, then try again.");
            }
            return;
        case Phase::CONNECTING: {
            const auto limit = connect_failed_ ? kConnectGrace : kPairTimeout;
            if (now - step_started_ >= limit) {
                fail_("Paired, but " + name_ +
                      " didn't connect. Press its PS / Home / Xbox button.");
            }
            return;
        }
        default:
            return;
    }
}

}  // namespace platform::bt
