// Controller Setup: pairing a Bluetooth controller, and the automatic layout
// for modern pads.
//
// The customer story these pin down: Settings -> Pair Wireless Controller
// (or Controller Setup -> Green), hold the pad's pairing buttons, and the
// box finds it, pairs it, asks for one button press to confirm it works,
// and -- for a PlayStation / Xbox / Switch Pro / 8BitDo pad -- is ready to
// play with no button mapping at all. Pads with no automatic layout drop
// into the existing mapping steps with the pad already chosen.
//
// The BlueZ session is a fake BluetoothPort; the pairing decisions
// themselves are the real PairingFlow (tests/platform/test_bt_pairing.cpp
// covers it in depth).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "platform/bt_pairing.h"
#include "retroarch/controller_profile.h"
#include "ui/controller_wizard.h"
#include "ui_test_doubles.h"

using ui::ControllerWizard;
using Phase = ui::ControllerWizard::Phase;
using platform::InputAction;
using namespace platform::bt;

namespace {

constexpr uint16_t kEvKey = 0x01, kEvAbs = 0x03;
constexpr uint16_t kSonyVid = 0x054c, kDualSensePid = 0x0ce6;
constexpr uint16_t kBtnSouth = 0x130, kBtnStart = 0x13b;
const std::string kPadPath = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF";

struct FakePort : BluetoothPort {
    bool started = false;
    int starts = 0, stops = 0;
    std::vector<BtCommand> sent;
    std::optional<BtSnapshot> snap;
    std::vector<BtResult> results;

    void start() override { started = true; ++starts; }
    void stop() override { started = false; ++stops; }
    void send(const BtCommand& c) override { sent.push_back(c); }
    std::optional<BtSnapshot> take_snapshot() override {
        auto s = snap;
        snap.reset();
        return s;
    }
    std::vector<BtResult> take_results() override {
        auto r = results;
        results.clear();
        return r;
    }

    void answer_all_ok() {
        for (const auto& c : sent) results.push_back({c.type, c.device_path, true, ""});
        sent.clear();
    }
    bool was_sent(BtCommandType t) const {
        for (const auto& c : sent) if (c.type == t) return true;
        return false;
    }
};

BtSnapshot powered(std::vector<BtDevice> devices = {}) {
    BtSnapshot s;
    s.adapter = BtAdapterState::ON;
    s.devices = std::move(devices);
    return s;
}

BtDevice dualsense_advertising() {
    BtDevice d;
    d.path = kPadPath;
    d.address = "AA:BB:CC:DD:EE:FF";
    d.name = "DualSense Wireless Controller";
    d.icon = "input-gaming";
    d.has_rssi = true;
    d.rssi = -45;
    d.modalias = "usb:v054Cp0CE6d0100";
    return d;
}

// What hid-playstation reports for a DualSense.
retroarch::CaptureDeviceCaps dualsense_caps() {
    retroarch::CaptureDeviceCaps c;
    c.vid = kSonyVid;
    c.pid = kDualSensePid;
    c.name = "DualSense Wireless Controller";
    c.driver = "playstation";
    c.bustype = 0x05;
    c.key_codes = {0x130, 0x131, 0x133, 0x134, 0x136, 0x137, 0x138,
                   0x139, 0x13a, 0x13b, 0x13c, 0x13d, 0x13e};
    for (uint16_t a : {0x00, 0x01, 0x02, 0x03, 0x04, 0x05}) {
        c.axes.push_back({a, -32768, 32767, 0});
    }
    c.axes.push_back({0x10, -1, 1, 0});
    c.axes.push_back({0x11, -1, 1, 0});
    return c;
}

// A generic HID pad: no automatic layout, needs the mapping steps.
retroarch::CaptureDeviceCaps generic_caps(uint16_t vid, uint16_t pid) {
    retroarch::CaptureDeviceCaps c;
    c.vid = vid;
    c.pid = pid;
    c.name = "Wireless Gamepad";
    c.driver = "hid-generic";
    for (uint16_t i = 0; i < 12; ++i) c.key_codes.push_back(static_cast<uint16_t>(0x120 + i));
    c.axes.push_back({0x00, 0, 255, 127});
    c.axes.push_back({0x01, 0, 255, 127});
    return c;
}

platform::RawInputEvent raw(uint16_t type, uint16_t code, int32_t value,
                            uint16_t vid = kSonyVid, uint16_t pid = kDualSensePid) {
    platform::RawInputEvent e;
    e.vid = vid;
    e.pid = pid;
    e.device_name = "DualSense Wireless Controller";
    e.type = type;
    e.code = code;
    e.value = value;
    return e;
}

platform::InputEvent act(InputAction a, bool pressed = true) {
    platform::InputEvent e{};
    e.action = a;
    e.pressed = pressed;
    return e;
}

struct ScopedStore {
    std::string path;
    explicit ScopedStore(const std::string& tag) {
        path = (std::filesystem::temp_directory_path() /
                ("mdb_wizard_pair_" + tag + ".json")).string();
        std::filesystem::remove(path);
        ::setenv("MAGIC_CONTROLLER_PROFILES_FILE", path.c_str(), 1);
    }
    ~ScopedStore() {
        std::filesystem::remove(path);
        ::unsetenv("MAGIC_CONTROLLER_PROFILES_FILE");
    }
};

// Radio on -> discovery -> the DualSense in pairing mode -> paired ->
// connected. Lands in PAIR_PRESS.
void pair_dualsense(ControllerWizard& w, FakePort& port) {
    port.snap = powered();
    w.tick();
    REQUIRE(port.was_sent(BtCommandType::START_SCAN));
    port.answer_all_ok();
    w.tick();
    port.snap = powered({dualsense_advertising()});
    w.tick();
    REQUIRE(port.was_sent(BtCommandType::PAIR));
    port.answer_all_ok();
    w.tick();
    REQUIRE(port.was_sent(BtCommandType::CONNECT));
    port.answer_all_ok();
    w.tick();
    REQUIRE(w.phase() == Phase::PAIR_PRESS);
}

struct Fixture {
    platform::InputManager im;
    ControllerWizard w;
    FakePort port;
    ScopedStore store{"fixture"};
    Fixture() {
        ui_test::fake_reset();
        w.set_bluetooth(&port);
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Getting into pairing
// ---------------------------------------------------------------------------

TEST_CASE("pairing: opening for pairing starts a BlueZ session", "[wizard][pairing]") {
    Fixture f;
    f.w.open_for_pairing(&f.im);
    CHECK(f.w.is_active());
    CHECK(f.w.phase() == Phase::PAIR_SEARCH);
    CHECK(f.port.started);
    // Raw capture is on, so a pad pressed later can be claimed.
    CHECK(ui_test::fake_raw_capture_enabled());
    REQUIRE(f.w.pairing() != nullptr);
}

TEST_CASE("pairing: Green on the first Controller Setup screen pairs a new pad",
          "[wizard][pairing]") {
    Fixture f;
    f.w.open(&f.im);
    REQUIRE(f.w.phase() == Phase::PICK_DEVICE);
    CHECK(f.w.can_pair());
    f.w.on_action(act(InputAction::NEXT));
    CHECK(f.w.phase() == Phase::PAIR_SEARCH);
    CHECK(f.port.started);
}

TEST_CASE("pairing: without Bluetooth the screen says so", "[wizard][pairing]") {
    platform::InputManager im;
    ControllerWizard w;
    ui_test::fake_reset();
    w.open_for_pairing(&im);
    REQUIRE(w.phase() == Phase::PAIR_SEARCH);
    REQUIRE(w.pairing() != nullptr);
    CHECK(w.pairing()->phase() == PairingFlow::Phase::UNAVAILABLE);
    CHECK_FALSE(w.pairing()->message().empty());
    // Green does nothing on PICK_DEVICE when there is no Bluetooth.
    w.close();
    w.open(&im);
    CHECK_FALSE(w.can_pair());
    w.on_action(act(InputAction::NEXT));
    CHECK(w.phase() == Phase::PICK_DEVICE);
}

// ---------------------------------------------------------------------------
// From connected to ready
// ---------------------------------------------------------------------------

TEST_CASE("pairing: a connected pad ends the BlueZ session and asks for a press",
          "[wizard][pairing]") {
    Fixture f;
    f.w.open_for_pairing(&f.im);
    pair_dualsense(f.w, f.port);
    CHECK_FALSE(f.port.started);
    CHECK(f.w.device_name() == "DualSense Wireless Controller");
}

TEST_CASE("pairing: a PlayStation pad is ready with no button mapping",
          "[wizard][pairing]") {
    Fixture f;
    ui_test::fake_set_caps(dualsense_caps());
    f.w.open_for_pairing(&f.im);
    pair_dualsense(f.w, f.port);

    f.w.on_raw_event(raw(kEvKey, kBtnSouth, 1));
    REQUIRE(f.w.phase() == Phase::AUTO_READY);
    CHECK(f.w.captured().count(retroarch::LogicalControl::CROSS) == 1);
    CHECK(f.w.captured().count(retroarch::LogicalControl::RSTICK_RIGHT) == 1);

    // The test screen lights what is pressed.
    f.w.on_raw_event(raw(kEvKey, 0x133, 1));   // Triangle
    CHECK(f.w.test_lit().at(retroarch::LogicalControl::TRIANGLE));
}

TEST_CASE("pairing: Start on the new pad finishes", "[wizard][pairing]") {
    Fixture f;
    ui_test::fake_set_caps(dualsense_caps());
    f.w.open_for_pairing(&f.im);
    pair_dualsense(f.w, f.port);
    f.w.on_raw_event(raw(kEvKey, kBtnSouth, 1));
    REQUIRE(f.w.phase() == Phase::AUTO_READY);
    f.w.on_raw_event(raw(kEvKey, kBtnStart, 1));
    CHECK(f.w.phase() == Phase::DONE);
    CHECK(f.w.done_title() == "Ready to play!");
    // Automatic layouts are never written to the profile store: a saved
    // profile would shadow future improvements to the automatic one.
    CHECK(retroarch::load_profile_store().empty());
}

TEST_CASE("pairing: the knob also finishes, Yellow maps buttons by hand",
          "[wizard][pairing]") {
    Fixture f;
    ui_test::fake_set_caps(dualsense_caps());
    f.w.open_for_pairing(&f.im);
    pair_dualsense(f.w, f.port);
    f.w.on_raw_event(raw(kEvKey, kBtnSouth, 1));
    REQUIRE(f.w.phase() == Phase::AUTO_READY);

    SECTION("knob") {
        f.w.on_action(act(InputAction::SELECT));
        CHECK(f.w.phase() == Phase::DONE);
    }
    SECTION("yellow") {
        f.w.on_action(act(InputAction::PREV));
        CHECK(f.w.phase() == Phase::PICK_STYLE);
        // ...and the mapping steps run on the paired pad.
        f.w.on_action(act(InputAction::SELECT));
        CHECK(f.w.phase() == Phase::CAPTURE);
    }
}

TEST_CASE("pairing: a pad with no automatic layout goes to the mapping steps",
          "[wizard][pairing]") {
    Fixture f;
    ui_test::fake_set_caps(generic_caps(kSonyVid, kDualSensePid));
    f.w.open_for_pairing(&f.im);
    pair_dualsense(f.w, f.port);
    f.w.on_raw_event(raw(kEvKey, 0x120, 1));
    CHECK(f.w.phase() == Phase::PICK_STYLE);
}

TEST_CASE("pairing: a pad with a saved Controller Setup profile is simply ready",
          "[wizard][pairing]") {
    Fixture f;
    retroarch::PhysicalProfile mine;
    mine.name = "DualSense Wireless Controller";
    mine.vid = kSonyVid;
    mine.pid = kDualSensePid;
    REQUIRE(retroarch::save_profile_store(
        {{retroarch::vidpid_key(kSonyVid, kDualSensePid), mine}}));
    ui_test::fake_set_caps(dualsense_caps());
    f.w.open_for_pairing(&f.im);
    pair_dualsense(f.w, f.port);
    f.w.on_raw_event(raw(kEvKey, kBtnSouth, 1));
    CHECK(f.w.phase() == Phase::DONE);
    CHECK(f.w.done_title() == "Ready to play!");
}

TEST_CASE("pairing: presses from the other, already-working pads are ignored",
          "[wizard][pairing]") {
    Fixture f;
    ui_test::fake_set_caps(dualsense_caps());
    f.w.open_for_pairing(&f.im);
    pair_dualsense(f.w, f.port);
    f.w.on_raw_event(raw(kEvKey, 0x120, 1, 0x0079, 0x0006));   // the USB pad
    CHECK(f.w.phase() == Phase::PAIR_PRESS);
    f.w.on_raw_event(raw(kEvKey, kBtnSouth, 1));
    CHECK(f.w.phase() == Phase::AUTO_READY);
}

// ---------------------------------------------------------------------------
// Failure and escape
// ---------------------------------------------------------------------------

TEST_CASE("pairing: cancel ends the BlueZ session", "[wizard][pairing][escape]") {
    Fixture f;
    f.w.open_for_pairing(&f.im);
    REQUIRE(f.port.started);
    f.w.on_action(act(InputAction::SETTINGS_MENU));
    CHECK_FALSE(f.w.is_active());
    CHECK_FALSE(f.port.started);
    CHECK_FALSE(ui_test::fake_raw_capture_enabled());
}

TEST_CASE("pairing: Red tries again after a failed pairing", "[wizard][pairing]") {
    Fixture f;
    f.w.open_for_pairing(&f.im);
    f.port.snap = powered();
    f.w.tick();
    f.port.answer_all_ok();
    f.w.tick();
    for (int i = 0; i < PairingFlow::kMaxPairFailures; ++i) {
        f.port.snap = powered({dualsense_advertising()});
        f.w.tick();
        f.port.results.push_back({BtCommandType::PAIR, kPadPath, false,
                                  "org.bluez.Error.AuthenticationFailed"});
        f.port.sent.clear();
        f.w.tick();
        f.port.answer_all_ok();
        f.w.tick();
    }
    REQUIRE(f.w.pairing()->phase() == PairingFlow::Phase::FAILED);
    f.port.sent.clear();
    f.w.on_action(act(InputAction::PLAY_PAUSE));
    f.w.tick();
    CHECK(f.w.pairing()->phase() == PairingFlow::Phase::SCANNING);
    CHECK(f.port.was_sent(BtCommandType::START_SCAN));
}

TEST_CASE("pairing: when Bluetooth is unavailable the knob closes the screen",
          "[wizard][pairing][escape]") {
    Fixture f;
    f.w.open_for_pairing(&f.im);
    BtSnapshot none;
    none.adapter = BtAdapterState::NO_BLUETOOTH;
    f.port.snap = none;
    f.w.tick();
    REQUIRE(f.w.pairing()->phase() == PairingFlow::Phase::UNAVAILABLE);
    f.w.on_action(act(InputAction::SELECT));
    CHECK_FALSE(f.w.is_active());
}

// ---------------------------------------------------------------------------
// Wired pads with an automatic layout
// ---------------------------------------------------------------------------

TEST_CASE("automatic layout: a wired DualSense picked in Controller Setup is shown, not mapped",
          "[wizard][auto]") {
    Fixture f;
    ui_test::fake_set_caps(dualsense_caps());
    f.w.open(&f.im);
    f.w.on_raw_event(raw(kEvKey, kBtnSouth, 1));
    CHECK(f.w.phase() == Phase::AUTO_READY);
}

TEST_CASE("automatic layout: a pad with a saved profile goes straight to re-mapping",
          "[wizard][auto]") {
    // Someone who saved a profile and comes back to Controller Setup wants
    // to change it.
    Fixture f;
    retroarch::PhysicalProfile mine;
    mine.vid = kSonyVid;
    mine.pid = kDualSensePid;
    REQUIRE(retroarch::save_profile_store(
        {{retroarch::vidpid_key(kSonyVid, kDualSensePid), mine}}));
    ui_test::fake_set_caps(dualsense_caps());
    f.w.open(&f.im);
    f.w.on_raw_event(raw(kEvKey, kBtnSouth, 1));
    CHECK(f.w.phase() == Phase::PICK_STYLE);
}

TEST_CASE("automatic layout: analog triggers light on the test screen",
          "[wizard][auto]") {
    // An Xbox (xpad) trigger rests at 0 of 0..1023 and is an AXIS binding.
    Fixture f;
    retroarch::CaptureDeviceCaps xbox;
    xbox.vid = 0x045e;
    xbox.pid = 0x028e;
    xbox.name = "Microsoft X-Box 360 pad";
    xbox.driver = "xpad";
    xbox.key_codes = {0x130, 0x131, 0x133, 0x134, 0x136, 0x137, 0x13a,
                      0x13b, 0x13c, 0x13d, 0x13e};
    xbox.axes = {{0x00, -32768, 32767, 0}, {0x01, -32768, 32767, 0},
                 {0x02, 0, 1023, 0},       {0x03, -32768, 32767, 0},
                 {0x04, -32768, 32767, 0}, {0x05, 0, 1023, 0},
                 {0x10, -1, 1, 0},         {0x11, -1, 1, 0}};
    ui_test::fake_set_caps(xbox);
    f.w.open(&f.im);
    f.w.on_raw_event(raw(kEvKey, kBtnSouth, 1, 0x045e, 0x028e));
    REQUIRE(f.w.phase() == Phase::AUTO_READY);
    f.w.on_raw_event(raw(kEvAbs, 0x02, 1023, 0x045e, 0x028e));
    CHECK(f.w.test_lit().at(retroarch::LogicalControl::L2));
}

TEST_CASE("pairing: cancel works on every pairing step", "[wizard][pairing][escape]") {
    Fixture f;
    ui_test::fake_set_caps(dualsense_caps());
    f.w.open_for_pairing(&f.im);
    pair_dualsense(f.w, f.port);

    SECTION("PAIR_PRESS") {
        CHECK(f.w.on_action(act(InputAction::SETTINGS_MENU)));
        CHECK_FALSE(f.w.is_active());
    }
    SECTION("AUTO_READY") {
        f.w.on_raw_event(raw(kEvKey, kBtnSouth, 1));
        REQUIRE(f.w.phase() == Phase::AUTO_READY);
        CHECK(f.w.on_action(act(InputAction::QUIT)));
        CHECK_FALSE(f.w.is_active());
    }
    CHECK_FALSE(ui_test::fake_raw_capture_enabled());
}

TEST_CASE("automatic layout: the pad disconnecting closes the screen",
          "[wizard][auto][escape]") {
    Fixture f;
    ui_test::fake_set_caps(dualsense_caps());
    f.w.open(&f.im);
    f.w.on_raw_event(raw(kEvKey, kBtnSouth, 1));
    REQUIRE(f.w.phase() == Phase::AUTO_READY);
    ui_test::fake_unplug();
    // The unplug check runs at 1 Hz.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (f.w.is_active() && std::chrono::steady_clock::now() < deadline) {
        f.w.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK_FALSE(f.w.is_active());
}
