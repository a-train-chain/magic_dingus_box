// platform::bt — the Bluetooth controller pairing flow behind Controller
// Setup's "Pair a wireless controller" step (platform/bt_pairing.h).
//
// The flow is the pure half: it reads BlueZ snapshots and command results
// and decides the next BlueZ command. The kiosk's BlueZ worker
// (bluez_worker.cpp, sd-bus, Linux-only) executes those commands. Nothing
// here touches D-Bus, so the whole customer-facing sequence -- find the pad
// in pairing mode, pair, trust, connect, and every failure on the way -- is
// pinned on the Mac.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

#include "platform/bt_pairing.h"

using namespace platform::bt;
using Clock = std::chrono::steady_clock;
using std::chrono::seconds;

namespace {

BtDevice pad(const std::string& addr, const std::string& name, int16_t rssi = -50) {
    BtDevice d;
    d.address = addr;
    d.path = "/org/bluez/hci0/dev_" + addr;
    d.name = name;
    d.icon = "input-gaming";
    d.cls = 0x002508;   // Peripheral / gamepad
    d.has_rssi = true;
    d.rssi = rssi;
    return d;
}

BtSnapshot snapshot(std::vector<BtDevice> devices,
                    BtAdapterState adapter = BtAdapterState::ON) {
    BtSnapshot s;
    s.adapter = adapter;
    s.devices = std::move(devices);
    return s;
}

BtResult ok(BtCommandType t, const std::string& path = "") {
    return BtResult{t, path, true, ""};
}
BtResult fail(BtCommandType t, const std::string& path, const std::string& err) {
    return BtResult{t, path, false, err};
}

bool has(const std::vector<BtCommand>& cmds, BtCommandType t,
         const std::string& path = "") {
    for (const auto& c : cmds) {
        if (c.type == t && (path.empty() || c.device_path == path)) return true;
    }
    return false;
}

// Drives a flow from a fresh start to SCANNING on a powered adapter.
PairingFlow scanning_flow(Clock::time_point t0) {
    PairingFlow f(t0);
    f.on_snapshot(snapshot({}), t0);
    auto cmds = f.take_commands();
    REQUIRE(has(cmds, BtCommandType::START_SCAN));
    f.on_result(ok(BtCommandType::START_SCAN), t0);
    REQUIRE(f.phase() == PairingFlow::Phase::SCANNING);
    return f;
}

}  // namespace

// ---------------------------------------------------------------------------
// Which advertising devices count as a controller
// ---------------------------------------------------------------------------

TEST_CASE("bt: gamepad class, icon or appearance marks a controller", "[bt]") {
    BtDevice d;
    d.icon = "input-gaming";
    CHECK(is_gamepad_candidate(d));

    BtDevice by_class;
    by_class.cls = 0x002508;   // DualSense: Peripheral, gamepad
    CHECK(is_gamepad_candidate(by_class));
    by_class.cls = 0x002504;   // joystick
    CHECK(is_gamepad_candidate(by_class));

    BtDevice by_appearance;    // Bluetooth LE: Xbox Series
    by_appearance.appearance = 0x03c4;
    CHECK(is_gamepad_candidate(by_appearance));
}

TEST_CASE("bt: a pad advertising only its name still counts", "[bt]") {
    // Some pads (8BitDo, cheap clones) advertise no class in pairing mode.
    BtDevice d;
    d.name = "8BitDo SN30 Pro+";
    CHECK(is_gamepad_candidate(d));
    d.name = "Pro Controller";
    CHECK(is_gamepad_candidate(d));
    d.name = "Xbox Wireless Controller";
    CHECK(is_gamepad_candidate(d));
}

TEST_CASE("bt: phones, headphones and keyboards are never paired", "[bt]") {
    BtDevice phone;
    phone.name = "Alex's iPhone";
    phone.icon = "phone";
    phone.cls = 0x5a020c;
    CHECK_FALSE(is_gamepad_candidate(phone));

    BtDevice headphones;
    headphones.name = "WH-1000XM4";
    headphones.icon = "audio-headset";
    headphones.cls = 0x240404;
    CHECK_FALSE(is_gamepad_candidate(headphones));

    BtDevice keyboard;
    keyboard.name = "Magic Keyboard";
    keyboard.cls = 0x002540;   // Peripheral, keyboard (not a gamepad subtype)
    CHECK_FALSE(is_gamepad_candidate(keyboard));
}

TEST_CASE("bt: modalias gives the controller's USB-style IDs", "[bt]") {
    auto ids = parse_modalias_ids("usb:v054Cp0CE6d0100");
    REQUIRE(ids.has_value());
    CHECK(ids->first == 0x054c);
    CHECK(ids->second == 0x0ce6);
    ids = parse_modalias_ids("bluetooth:v045Ep0B13d0509");
    REQUIRE(ids.has_value());
    CHECK(ids->first == 0x045e);
    CHECK(ids->second == 0x0b13);
    CHECK_FALSE(parse_modalias_ids("").has_value());
    CHECK_FALSE(parse_modalias_ids("garbage").has_value());
}

// ---------------------------------------------------------------------------
// Getting the radio ready
// ---------------------------------------------------------------------------

TEST_CASE("bt: a powered adapter starts scanning straight away", "[bt]") {
    const auto t0 = Clock::now();
    PairingFlow f(t0);
    CHECK(f.phase() == PairingFlow::Phase::STARTING);
    f.on_snapshot(snapshot({}), t0);
    const auto cmds = f.take_commands();
    CHECK(has(cmds, BtCommandType::START_SCAN));
    CHECK_FALSE(has(cmds, BtCommandType::POWER_ON));
}

TEST_CASE("bt: a switched-off adapter is powered on first", "[bt]") {
    const auto t0 = Clock::now();
    PairingFlow f(t0);
    f.on_snapshot(snapshot({}, BtAdapterState::OFF), t0);
    auto cmds = f.take_commands();
    CHECK(has(cmds, BtCommandType::POWER_ON));
    CHECK_FALSE(has(cmds, BtCommandType::START_SCAN));
    // Asked once, not on every snapshot.
    f.on_snapshot(snapshot({}, BtAdapterState::OFF), t0);
    CHECK(f.take_commands().empty());
    f.on_result(ok(BtCommandType::POWER_ON), t0);
    f.on_snapshot(snapshot({}), t0);
    CHECK(has(f.take_commands(), BtCommandType::START_SCAN));
}

TEST_CASE("bt: no Bluetooth on the box says so and stops", "[bt]") {
    const auto t0 = Clock::now();
    PairingFlow f(t0);
    f.on_snapshot(snapshot({}, BtAdapterState::NO_BLUETOOTH), t0);
    CHECK(f.phase() == PairingFlow::Phase::UNAVAILABLE);
    CHECK_FALSE(f.message().empty());
    CHECK(f.take_commands().empty());
}

TEST_CASE("bt: a blocked radio says so and stops", "[bt]") {
    const auto t0 = Clock::now();
    PairingFlow f(t0);
    f.on_snapshot(snapshot({}, BtAdapterState::BLOCKED), t0);
    CHECK(f.phase() == PairingFlow::Phase::UNAVAILABLE);
}

TEST_CASE("bt: power-on failing is reported, not retried forever", "[bt]") {
    const auto t0 = Clock::now();
    PairingFlow f(t0);
    f.on_snapshot(snapshot({}, BtAdapterState::OFF), t0);
    f.take_commands();
    f.on_result(fail(BtCommandType::POWER_ON, "", "org.bluez.Error.Blocked"), t0);
    CHECK(f.phase() == PairingFlow::Phase::UNAVAILABLE);
}

TEST_CASE("bt: the BlueZ worker failing to start is reported", "[bt]") {
    const auto t0 = Clock::now();
    PairingFlow f(t0);
    BtSnapshot s = snapshot({}, BtAdapterState::NO_BLUETOOTH);
    s.error = "Failed to connect to system bus";
    f.on_snapshot(s, t0);
    CHECK(f.phase() == PairingFlow::Phase::UNAVAILABLE);
}

// ---------------------------------------------------------------------------
// Finding and pairing the controller
// ---------------------------------------------------------------------------

TEST_CASE("bt: the controller in pairing mode is paired, trusted, connected",
          "[bt]") {
    const auto t0 = Clock::now();
    auto f = scanning_flow(t0);
    const auto ds = pad("AA_BB_CC_DD_EE_FF", "DualSense Wireless Controller");

    f.on_snapshot(snapshot({ds}), t0);
    auto cmds = f.take_commands();
    CHECK(has(cmds, BtCommandType::STOP_SCAN));
    CHECK(has(cmds, BtCommandType::PAIR, ds.path));
    CHECK(f.phase() == PairingFlow::Phase::PAIRING);
    CHECK(f.device_name() == "DualSense Wireless Controller");

    f.on_result(ok(BtCommandType::STOP_SCAN), t0);
    f.on_result(ok(BtCommandType::PAIR, ds.path), t0);
    cmds = f.take_commands();
    CHECK(has(cmds, BtCommandType::TRUST, ds.path));
    CHECK(has(cmds, BtCommandType::CONNECT, ds.path));
    CHECK(f.phase() == PairingFlow::Phase::CONNECTING);

    f.on_result(ok(BtCommandType::TRUST, ds.path), t0);
    f.on_result(ok(BtCommandType::CONNECT, ds.path), t0);
    CHECK(f.phase() == PairingFlow::Phase::CONNECTED);
    CHECK(f.device_path() == ds.path);
}

TEST_CASE("bt: devices that are not controllers are ignored", "[bt]") {
    const auto t0 = Clock::now();
    auto f = scanning_flow(t0);
    BtDevice headphones;
    headphones.path = "/org/bluez/hci0/dev_11_22_33_44_55_66";
    headphones.name = "WH-1000XM4";
    headphones.icon = "audio-headset";
    headphones.has_rssi = true;
    f.on_snapshot(snapshot({headphones}), t0);
    CHECK(f.take_commands().empty());
    CHECK(f.phase() == PairingFlow::Phase::SCANNING);
}

TEST_CASE("bt: an already-connected controller is not re-paired", "[bt]") {
    const auto t0 = Clock::now();
    auto f = scanning_flow(t0);
    auto mine = pad("AA_BB_CC_DD_EE_FF", "DualSense Wireless Controller");
    mine.paired = mine.trusted = mine.connected = true;
    f.on_snapshot(snapshot({mine}), t0);
    CHECK(f.take_commands().empty());
}

TEST_CASE("bt: a paired controller that was not seen this scan is left alone",
          "[bt]") {
    // BlueZ keeps an object for every paired pad; only RSSI says it is
    // advertising (in pairing mode) right now.
    const auto t0 = Clock::now();
    auto f = scanning_flow(t0);
    auto asleep = pad("AA_BB_CC_DD_EE_FF", "Wireless Controller");
    asleep.paired = true;
    asleep.has_rssi = false;
    f.on_snapshot(snapshot({asleep}), t0);
    CHECK(f.take_commands().empty());
}

TEST_CASE("bt: the closest controller wins when two are in pairing mode", "[bt]") {
    const auto t0 = Clock::now();
    auto f = scanning_flow(t0);
    const auto far = pad("11_11_11_11_11_11", "Xbox Wireless Controller", -85);
    const auto near = pad("22_22_22_22_22_22", "DualSense Wireless Controller", -40);
    f.on_snapshot(snapshot({far, near}), t0);
    CHECK(has(f.take_commands(), BtCommandType::PAIR, near.path));
}

TEST_CASE("bt: a controller paired before (e.g. to a PS5) is forgotten, then paired",
          "[bt]") {
    // In pairing mode again, the pad has dropped the box's keys; BlueZ still
    // has the old ones and pairing would fail. Remove first.
    const auto t0 = Clock::now();
    auto f = scanning_flow(t0);
    auto stale = pad("AA_BB_CC_DD_EE_FF", "DualSense Wireless Controller");
    stale.paired = true;
    f.on_snapshot(snapshot({stale}), t0);
    auto cmds = f.take_commands();
    CHECK(has(cmds, BtCommandType::REMOVE, stale.path));
    CHECK_FALSE(has(cmds, BtCommandType::PAIR));

    f.on_result(ok(BtCommandType::REMOVE, stale.path), t0);
    // Discovery sees it again, unpaired now.
    auto fresh = stale;
    fresh.paired = false;
    f.on_snapshot(snapshot({fresh}), t0);
    CHECK(has(f.take_commands(), BtCommandType::PAIR, fresh.path));
}

TEST_CASE("bt: a failed pairing goes back to looking, once", "[bt]") {
    const auto t0 = Clock::now();
    auto f = scanning_flow(t0);
    const auto ds = pad("AA_BB_CC_DD_EE_FF", "DualSense Wireless Controller");
    f.on_snapshot(snapshot({ds}), t0);
    f.take_commands();
    f.on_result(fail(BtCommandType::PAIR, ds.path,
                     "org.bluez.Error.AuthenticationFailed"), t0);
    CHECK(f.phase() == PairingFlow::Phase::SCANNING);
    CHECK(has(f.take_commands(), BtCommandType::START_SCAN));
    CHECK_FALSE(f.message().empty());   // "put it back in pairing mode"

    // Second failure: stop and say so.
    f.on_result(ok(BtCommandType::START_SCAN), t0);
    f.on_snapshot(snapshot({ds}), t0);
    f.take_commands();
    f.on_result(fail(BtCommandType::PAIR, ds.path,
                     "org.bluez.Error.ConnectionAttemptFailed"), t0);
    CHECK(f.phase() == PairingFlow::Phase::FAILED);
}

TEST_CASE("bt: retry after a failure starts looking again", "[bt]") {
    const auto t0 = Clock::now();
    auto f = scanning_flow(t0);
    const auto ds = pad("AA_BB_CC_DD_EE_FF", "DualSense Wireless Controller");
    for (int i = 0; i < 2; ++i) {
        f.on_snapshot(snapshot({ds}), t0);
        f.take_commands();
        f.on_result(fail(BtCommandType::PAIR, ds.path, "org.bluez.Error.Failed"), t0);
        f.on_result(ok(BtCommandType::START_SCAN), t0);
    }
    REQUIRE(f.phase() == PairingFlow::Phase::FAILED);
    f.retry(t0);
    CHECK(f.phase() == PairingFlow::Phase::SCANNING);
    CHECK(has(f.take_commands(), BtCommandType::START_SCAN));
}

TEST_CASE("bt: 'already paired' from BlueZ is a success", "[bt]") {
    const auto t0 = Clock::now();
    auto f = scanning_flow(t0);
    const auto ds = pad("AA_BB_CC_DD_EE_FF", "DualSense Wireless Controller");
    f.on_snapshot(snapshot({ds}), t0);
    f.take_commands();
    f.on_result(fail(BtCommandType::PAIR, ds.path, "org.bluez.Error.AlreadyExists"), t0);
    CHECK(f.phase() == PairingFlow::Phase::CONNECTING);
}

TEST_CASE("bt: the pad connecting by itself counts as connected", "[bt]") {
    const auto t0 = Clock::now();
    auto f = scanning_flow(t0);
    auto ds = pad("AA_BB_CC_DD_EE_FF", "DualSense Wireless Controller");
    f.on_snapshot(snapshot({ds}), t0);
    f.take_commands();
    f.on_result(ok(BtCommandType::PAIR, ds.path), t0);
    f.take_commands();
    // Connect() errors because the pad beat us to it...
    f.on_result(fail(BtCommandType::CONNECT, ds.path,
                     "org.bluez.Error.AlreadyConnected"), t0);
    CHECK(f.phase() == PairingFlow::Phase::CONNECTED);
}

TEST_CASE("bt: a failed connect waits for the pad to connect itself", "[bt]") {
    const auto t0 = Clock::now();
    auto f = scanning_flow(t0);
    auto ds = pad("AA_BB_CC_DD_EE_FF", "DualSense Wireless Controller");
    f.on_snapshot(snapshot({ds}), t0);
    f.take_commands();
    f.on_result(ok(BtCommandType::PAIR, ds.path), t0);
    f.take_commands();
    f.on_result(fail(BtCommandType::CONNECT, ds.path, "org.bluez.Error.Failed"), t0);
    CHECK(f.phase() == PairingFlow::Phase::CONNECTING);

    SECTION("it does") {
        ds.paired = ds.connected = true;
        f.on_snapshot(snapshot({ds}), t0 + seconds(3));
        CHECK(f.phase() == PairingFlow::Phase::CONNECTED);
    }
    SECTION("it does not") {
        f.tick(t0 + seconds(16));
        CHECK(f.phase() == PairingFlow::Phase::FAILED);
        CHECK_FALSE(f.message().empty());
    }
}

TEST_CASE("bt: a pairing that never answers times out", "[bt]") {
    const auto t0 = Clock::now();
    auto f = scanning_flow(t0);
    const auto ds = pad("AA_BB_CC_DD_EE_FF", "DualSense Wireless Controller");
    f.on_snapshot(snapshot({ds}), t0);
    f.take_commands();
    f.tick(t0 + seconds(30));
    CHECK(f.phase() == PairingFlow::Phase::PAIRING);
    f.tick(t0 + seconds(61));
    CHECK(f.phase() == PairingFlow::Phase::FAILED);
}

TEST_CASE("bt: a long search gets a hint but keeps looking", "[bt]") {
    const auto t0 = Clock::now();
    auto f = scanning_flow(t0);
    CHECK_FALSE(f.searching_a_while());
    f.tick(t0 + seconds(31));
    CHECK(f.searching_a_while());
    CHECK(f.phase() == PairingFlow::Phase::SCANNING);
}

TEST_CASE("bt: the connected controller's IDs come from its modalias", "[bt]") {
    const auto t0 = Clock::now();
    auto f = scanning_flow(t0);
    auto ds = pad("AA_BB_CC_DD_EE_FF", "DualSense Wireless Controller");
    ds.modalias = "usb:v054Cp0CE6d0100";
    f.on_snapshot(snapshot({ds}), t0);
    f.take_commands();
    f.on_result(ok(BtCommandType::PAIR, ds.path), t0);
    f.on_result(ok(BtCommandType::CONNECT, ds.path), t0);
    REQUIRE(f.phase() == PairingFlow::Phase::CONNECTED);
    const auto ids = f.device_ids();
    REQUIRE(ids.has_value());
    CHECK(ids->first == 0x054c);
    CHECK(ids->second == 0x0ce6);
}

TEST_CASE("bt: results for some other device are ignored", "[bt]") {
    const auto t0 = Clock::now();
    auto f = scanning_flow(t0);
    const auto ds = pad("AA_BB_CC_DD_EE_FF", "DualSense Wireless Controller");
    f.on_snapshot(snapshot({ds}), t0);
    f.take_commands();
    f.on_result(ok(BtCommandType::PAIR, "/org/bluez/hci0/dev_00_00_00_00_00_00"), t0);
    CHECK(f.phase() == PairingFlow::Phase::PAIRING);
}
