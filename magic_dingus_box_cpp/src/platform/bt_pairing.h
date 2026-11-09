#pragma once

// Bluetooth controller pairing: the pure half.
//
// Controller Setup's "Pair a wireless controller" step runs this flow. It is
// a state machine with no I/O: it reads snapshots of BlueZ's state and the
// results of the commands it asked for, and says which BlueZ command to run
// next. The kiosk's BlueZ worker (bluez_worker.h, sd-bus, Linux-only) runs
// those commands on its own thread, so the render loop never waits on the
// radio. Mac-tested in tests/platform/test_bt_pairing.cpp.
//
// The customer's whole job is "put the controller in pairing mode". The flow
// then:
//   1. powers the radio if it is off, and starts discovery;
//   2. picks the first CONTROLLER that is advertising (gamepad class, icon or
//      appearance, or a controller-ish name) -- the closest one if several;
//      never a phone, headphones or keyboard;
//   3. if BlueZ still remembers that pad from an older pairing (it is in
//      pairing mode again, so it has dropped those keys -- e.g. it was paired
//      to a PS5 since), removes it first;
//   4. stops discovery, pairs, marks it TRUSTED (so it reconnects by itself
//      every time its home button is pressed) and connects.
// A failed pairing goes back to looking once, then stops with a message and
// waits for retry().
//
// No list to pick from on purpose: the person pairing is holding the
// controller they want, and may have nothing else that works yet.

#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace platform::bt {

enum class BtAdapterState {
    UNKNOWN,        // no snapshot yet
    NO_BLUETOOTH,   // no BlueZ on the bus, or no adapter
    BLOCKED,        // rfkill
    OFF,            // present, not powered
    ON,
};

// One org.bluez.Device1 object, as much of it as pairing needs.
struct BtDevice {
    std::string path;      // /org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF
    std::string address;
    std::string name;      // empty until the name is resolved
    std::string icon;      // BlueZ's own guess: "input-gaming", "phone", ...
    uint32_t cls = 0;      // BR/EDR Class of Device (0 = not reported)
    uint16_t appearance = 0;  // LE appearance (0 = not reported)
    bool paired = false;
    bool trusted = false;
    bool connected = false;
    // RSSI is only present while discovery is seeing the device advertise,
    // i.e. while it is in pairing mode right now.
    bool has_rssi = false;
    int16_t rssi = 0;
    std::string modalias;  // "usb:v054Cp0CE6d0100"
};

struct BtSnapshot {
    BtAdapterState adapter = BtAdapterState::UNKNOWN;
    bool discovering = false;
    std::vector<BtDevice> devices;
    std::string error;     // why the worker could not read BlueZ, if it could not
};

enum class BtCommandType { POWER_ON, START_SCAN, STOP_SCAN, REMOVE, PAIR, TRUST, CONNECT };

struct BtCommand {
    BtCommandType type;
    std::string device_path;   // empty for adapter commands
};

struct BtResult {
    BtCommandType type;
    std::string device_path;
    bool ok = false;
    std::string error;         // D-Bus error name (+ message) when !ok
};

// True for a device that is a game controller.
bool is_gamepad_candidate(const BtDevice& device);

// (vid, pid) from a BlueZ modalias ("usb:v054Cp0CE6d0100",
// "bluetooth:v045Ep0B13d0509"). nullopt when absent or malformed.
std::optional<std::pair<uint16_t, uint16_t>> parse_modalias_ids(const std::string& modalias);

class PairingFlow {
public:
    using Clock = std::chrono::steady_clock;

    enum class Phase {
        STARTING,     // waiting for the first snapshot / radio power
        SCANNING,     // looking for a controller in pairing mode
        PAIRING,      // pairing with device_name()
        CONNECTING,   // paired; trusting + connecting
        CONNECTED,    // done: the controller is connected
        FAILED,       // gave up; retry() starts over
        UNAVAILABLE,  // no usable Bluetooth on this box
    };

    explicit PairingFlow(Clock::time_point now);

    void on_snapshot(const BtSnapshot& snapshot, Clock::time_point now);
    void on_result(const BtResult& result, Clock::time_point now);
    void tick(Clock::time_point now);
    // FAILED -> look again from scratch.
    void retry(Clock::time_point now);

    // Commands decided since the last call, in order.
    std::vector<BtCommand> take_commands();

    Phase phase() const { return phase_; }
    // A short line for the screen: why it failed, or what to do now. Empty
    // when the phase says it all.
    const std::string& message() const { return message_; }
    // The controller being paired (or connected), once one was picked.
    const std::string& device_name() const { return name_; }
    const std::string& device_path() const { return path_; }
    std::optional<std::pair<uint16_t, uint16_t>> device_ids() const;
    // SCANNING for 30 s with no controller found: the screen adds a hint.
    bool searching_a_while() const { return searching_a_while_; }

    static constexpr std::chrono::seconds kHintAfter{30};
    static constexpr std::chrono::seconds kPairTimeout{60};
    static constexpr std::chrono::seconds kConnectGrace{15};
    static constexpr int kMaxPairFailures = 2;

private:
    void start_scan_(Clock::time_point now);
    void fail_(const std::string& message);
    void pair_failed_(Clock::time_point now);

    Phase phase_ = Phase::STARTING;
    std::vector<BtCommand> pending_;
    std::string message_;
    std::string name_;
    std::string path_;
    std::string modalias_;
    bool power_requested_ = false;
    bool scan_requested_ = false;
    bool searching_a_while_ = false;
    int pair_failures_ = 0;
    // Paths removed this session (stale pairings): pair them, do not remove
    // again.
    std::set<std::string> removed_;
    // A REMOVE in flight; nothing else is decided until it answers.
    std::string removing_;
    bool connect_failed_ = false;
    Clock::time_point scan_started_;
    Clock::time_point step_started_;
};

// The kiosk side of the flow: a session with BlueZ. Implemented by
// BluezWorker on the box; tests use a fake. All calls are made from the UI
// thread and never block on the radio.
class BluetoothPort {
public:
    virtual ~BluetoothPort() = default;
    // Begin a pairing session (connect to BlueZ, register the pairing agent,
    // start publishing snapshots). Idempotent.
    virtual void start() = 0;
    // End it: stop discovery, unregister the agent. Idempotent. Never
    // removes or disconnects anything.
    virtual void stop() = 0;
    virtual void send(const BtCommand& command) = 0;
    // The newest snapshot since the last call, if there is one.
    virtual std::optional<BtSnapshot> take_snapshot() = 0;
    virtual std::vector<BtResult> take_results() = 0;
};

}  // namespace platform::bt
