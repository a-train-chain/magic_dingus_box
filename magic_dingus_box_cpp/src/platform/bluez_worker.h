#pragma once

// The kiosk's BlueZ session for Controller Setup's pairing step: the
// I/O half of platform/bt_pairing.h. Kiosk-only (sd-bus); in no Mac target.
//
// One background thread per session, with its own system-bus connection:
//  - publishes a BtSnapshot (adapter state + every org.bluez.Device1) about
//    twice a second;
//  - runs the BtCommands PairingFlow decides, Pair and Connect
//    asynchronously so the agent below can answer while they are in flight;
//  - registers a NoInputNoOutput pairing agent as the default for the life
//    of the session. Modern pads pair with "Just Works" and never ask it
//    anything; it exists for older pads that ask for a PIN ("0000") and to
//    authorize the new pad's input service while the screen is up.
//
// NEVER BLOCKS THE RENDER THREAD. start()/stop()/send()/take_*() only touch
// a mutex-guarded queue. stop() does not join: it tells the session to end
// and detaches, because ending it makes D-Bus calls, and a wedged
// bluetoothd must not be able to hold the kiosk's main loop past its 10 s
// systemd watchdog. Every synchronous call on the worker thread is capped
// at 5 s; Pair at 40 s and Connect at 20 s.
//
// Without libsystemd at build time (HAVE_SYSTEMD unset) the session reports
// NO_BLUETOOTH, and the screen says Bluetooth is not available. The release
// workflow refuses such a binary (it already requires sd_notify, the same
// library).

#include <memory>
#include <optional>
#include <vector>

#include "bt_pairing.h"

namespace platform {

class BluezWorker : public bt::BluetoothPort {
public:
    BluezWorker() = default;
    ~BluezWorker() override;
    BluezWorker(const BluezWorker&) = delete;
    BluezWorker& operator=(const BluezWorker&) = delete;

    void start() override;
    void stop() override;
    void send(const bt::BtCommand& command) override;
    std::optional<bt::BtSnapshot> take_snapshot() override;
    std::vector<bt::BtResult> take_results() override;

    struct Session;   // shared with the worker thread

private:
    std::shared_ptr<Session> session_;
};

}  // namespace platform
