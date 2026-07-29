// SettingsMenuManager's half of the Controller Setup wizard lifecycle.
//
// WHY THIS FILE EXISTS, separately from test_controller_wizard.cpp:
//
// The wizard's own on_action(SETTINGS_MENU) cancel branch CANNOT FIRE ON
// HARDWARE. main.cpp's per-event dispatch handles InputAction::SETTINGS_MENU
// at the very top, inside the BTN4 hold/volume block, and that block ends in
// `continue` on both press and release (main.cpp ~2159-2206) — so the event is
// consumed hundreds of lines before the wizard interception block ever sees
// it. What the operator's BTN4 press actually reaches is
// settings_menu.toggle(), whose wizard_active_ branch calls
// close_controller_wizard().
//
// That is the escape hatch on the one screen whose failure mode is "the
// appliance is bricked and the owner has no keyboard". Everything below drives
// it through the real SettingsMenuManager, exactly as main.cpp does.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#include "app/app_state.h"
#include "retroarch/controller_profile.h"
#include "ui/controller_wizard.h"
#include "ui/settings_menu.h"
#include "ui_test_doubles.h"

using ui::ControllerWizard;
using ui::SettingsMenuManager;
using Phase = ui::ControllerWizard::Phase;
using platform::InputAction;

namespace {

constexpr uint16_t kVid = 0x1234, kPid = 0x5678;
// linux/input-event-codes.h numbering, hardcoded because that header does not
// exist on macOS where this binary builds.
constexpr uint16_t kEvKey = 0x01;
constexpr uint16_t kBtn0 = 0x130;

retroarch::CaptureDeviceCaps make_caps() {
    retroarch::CaptureDeviceCaps c;
    c.vid = kVid;
    c.pid = kPid;
    c.name = "Test Pad";
    for (uint16_t i = 0; i < 32; ++i) c.key_codes.push_back(kBtn0 + i);
    c.axes.push_back({0x00, -32768, 32767, 0});
    c.axes.push_back({0x01, -32768, 32767, 0});
    c.axes.push_back({0x10, -1, 1, 0});
    c.axes.push_back({0x11, -1, 1, 0});
    return c;
}

platform::RawInputEvent raw(uint16_t type, uint16_t code, int32_t value) {
    platform::RawInputEvent e;
    e.vid = kVid;
    e.pid = kPid;
    e.device_name = "Test Pad";
    e.type = type;
    e.code = code;
    e.value = value;
    return e;
}

platform::InputEvent act(InputAction a, bool pressed = true, int delta = 0) {
    platform::InputEvent e{};
    e.action = a;
    e.delta = delta;
    e.pressed = pressed;
    return e;
}

void press_button(ControllerWizard& w, uint16_t code) {
    w.on_raw_event(raw(kEvKey, code, 1));
    w.on_raw_event(raw(kEvKey, code, 0));
}

// Fresh temp store per test, so the DONE-phase section's save cannot leak into
// the operator's real controller_profiles.json or into another test.
struct ScopedStore {
    std::string path;
    explicit ScopedStore(const std::string& tag) {
        path = (std::filesystem::temp_directory_path() /
                ("mdb_smwiz_" + tag + ".json")).string();
        std::filesystem::remove(path);
        ::setenv("MAGIC_CONTROLLER_PROFILES_FILE", path.c_str(), 1);
    }
    ~ScopedStore() {
        std::filesystem::remove(path);
        ::unsetenv("MAGIC_CONTROLLER_PROFILES_FILE");
    }
};

// The whole production entry sequence: Settings open, then Settings ->
// Controller Setup. Leaves the wizard armed at PICK_DEVICE.
struct Fixture {
    app::AppState state{};
    platform::InputManager im;
    SettingsMenuManager sm{&state};

    Fixture() {
        ui_test::fake_reset();
        ui_test::fake_set_caps(make_caps());
        sm.open();
        sm.open_controller_wizard(&im);
        REQUIRE(sm.is_controller_wizard_active());
        REQUIRE(sm.controller_wizard()->is_active());
        REQUIRE(ui_test::fake_raw_capture_enabled());
    }

    ControllerWizard& wiz() { return *sm.controller_wizard(); }

    // Walk the wizard from PICK_DEVICE to the requested phase using only the
    // inputs main.cpp would deliver.
    void drive_to(Phase target) {
        if (target == Phase::PICK_DEVICE) return;
        press_button(wiz(), kBtn0);                          // pad claims itself
        REQUIRE(wiz().phase() == Phase::PICK_STYLE);
        if (target == Phase::PICK_STYLE) return;
        wiz().on_action(act(InputAction::SELECT));            // confirm PS style
        REQUIRE(wiz().phase() == Phase::CAPTURE);
        if (target == Phase::CAPTURE) return;
        const size_t n = wiz().step_count();
        for (size_t i = 0; i < n; ++i)
            press_button(wiz(), static_cast<uint16_t>(kBtn0 + i));
        REQUIRE(wiz().phase() == Phase::TEST);
        if (target == Phase::TEST) return;
        wiz().on_action(act(InputAction::SELECT));            // save
        REQUIRE(wiz().phase() == Phase::DONE);
    }
};

// Every close path must leave BOTH the flag and the wizard itself retired, and
// must hand InputManager back out of raw-capture mode. A half-retired wizard is
// the bricked-appliance state this whole screen is built to avoid.
void check_fully_retired(SettingsMenuManager& sm) {
    CHECK_FALSE(sm.is_controller_wizard_active());
    CHECK_FALSE(sm.controller_wizard()->is_active());
    CHECK_FALSE(ui_test::fake_raw_capture_enabled());
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------

TEST_CASE("open_controller_wizard arms the wizard and raw capture",
          "[settings_menu][wizard]") {
    Fixture f;   // its REQUIREs are the assertions
    CHECK(f.wiz().phase() == Phase::PICK_DEVICE);
    CHECK(f.sm.is_active());
}

// ---------------------------------------------------------------------------
// THE PRODUCTION ESCAPE PATH
// ---------------------------------------------------------------------------

TEST_CASE("toggle() retires the wizard from every phase and lands back on Settings",
          "[settings_menu][wizard][escape]") {
    ScopedStore store("toggle");

    SECTION("PICK_DEVICE") {
        Fixture f;
        f.drive_to(Phase::PICK_DEVICE);
        f.sm.toggle();
        check_fully_retired(f.sm);
    }
    SECTION("PICK_STYLE") {
        Fixture f;
        f.drive_to(Phase::PICK_STYLE);
        f.sm.toggle();
        check_fully_retired(f.sm);
    }
    SECTION("CAPTURE") {
        Fixture f;
        f.drive_to(Phase::CAPTURE);
        f.sm.toggle();
        check_fully_retired(f.sm);
    }
    SECTION("TEST") {
        Fixture f;
        f.drive_to(Phase::TEST);
        f.sm.toggle();
        check_fully_retired(f.sm);
    }
    SECTION("DONE") {
        Fixture f;
        f.drive_to(Phase::DONE);
        f.sm.toggle();
        check_fully_retired(f.sm);
    }
}

TEST_CASE("toggle() out of the wizard does NOT also close Settings",
          "[settings_menu][wizard][escape]") {
    // The operator came from the Settings list and must land back on it: one
    // BTN4 press is one level of back, not two. If this ever regressed to
    // closing Settings outright, cancelling the wizard would dump the user on
    // the playlist screen with no idea whether anything was saved.
    Fixture f;
    f.drive_to(Phase::CAPTURE);

    f.sm.toggle();
    check_fully_retired(f.sm);
    CHECK(f.sm.is_active());
    CHECK_FALSE(f.sm.is_closing());

    // ...and the wizard branch does not stick: the NEXT press closes Settings
    // the ordinary way.
    f.sm.toggle();
    CHECK(f.sm.is_closing());
}

// ---------------------------------------------------------------------------
// Defense in depth: the other two dismissal entry points
// ---------------------------------------------------------------------------
//
// Neither close() nor force_close() is reachable while the wizard is up today
// (main.cpp's wizard interception block `continue`s before the SELECT dispatch
// that launches a game, and the force_close() at main.cpp:1347 is gated on
// current_screen == MediaBrowser, which that same `continue` makes
// unreachable). These tests pin the invariant "Settings dismissed => wizard
// retired" so it holds structurally rather than resting on main.cpp continuing
// to swallow every event.

TEST_CASE("close() retires the wizard", "[settings_menu][wizard][escape]") {
    Fixture f;
    f.drive_to(Phase::CAPTURE);
    f.sm.close();
    check_fully_retired(f.sm);
    CHECK(f.sm.is_closing());
}

TEST_CASE("force_close() retires the wizard", "[settings_menu][wizard][escape]") {
    Fixture f;
    f.drive_to(Phase::CAPTURE);
    f.sm.force_close();
    check_fully_retired(f.sm);
    CHECK_FALSE(f.sm.is_active());
    CHECK_FALSE(f.sm.is_closing());   // teleport, no animation
}

// ---------------------------------------------------------------------------
// Races between the wizard retiring itself and the menu catching up
// ---------------------------------------------------------------------------

TEST_CASE("close_controller_wizard after the wizard self-retired is a no-op",
          "[settings_menu][wizard][escape]") {
    // tick()'s idle timeout and unplug watchdog both close the wizard from
    // under the menu, a frame before main.cpp's pump clears wizard_active_.
    // close_controller_wizard() must not re-toggle raw capture in that window.
    Fixture f;
    f.drive_to(Phase::CAPTURE);

    ui_test::fake_unplug();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    REQUIRE_FALSE(f.wiz().tick());        // watchdog fired
    REQUIRE_FALSE(f.wiz().is_active());
    REQUIRE(f.sm.is_controller_wizard_active());   // flag not caught up yet
    const int calls = ui_test::fake_raw_capture_calls();

    f.sm.close_controller_wizard();
    check_fully_retired(f.sm);
    CHECK(ui_test::fake_raw_capture_calls() == calls);

    // ...and repeating it stays a no-op.
    f.sm.close_controller_wizard();
    CHECK(ui_test::fake_raw_capture_calls() == calls);
    check_fully_retired(f.sm);
}

// ---------------------------------------------------------------------------
// Reset Controller Setup — the wizard's undo
// ---------------------------------------------------------------------------
//
// A captured profile unconditionally shadows the built-in one for its VID/PID
// and the file is deliberately OTA-immune, so before this row existed a bad
// capture could only be corrected by completing all 24 wizard steps again on
// the pad the bad capture had just broken. There was no path back to the
// shipped mapping from the kiosk at all.

namespace {

// Index of the row whose label starts with `prefix` in the current submenu,
// or -1. The System submenu's length varies with build flags and unlock
// state, so rows are located by label rather than by a hardcoded index.
int row_index(const SettingsMenuManager& sm, const std::string& prefix) {
    const auto& items = sm.get_submenu_items();
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].label.rfind(prefix, 0) == 0) return static_cast<int>(i);
    }
    return -1;
}

// Put one captured profile in the store, as a completed wizard run would.
void seed_store() {
    std::map<std::string, retroarch::PhysicalProfile> s;
    s[retroarch::vidpid_key(kVid, kPid)] = retroarch::builtin_dragonrise_profile();
    REQUIRE(retroarch::save_profile_store(s));
}

}  // namespace

TEST_CASE("the reset row is hidden when nothing has been captured",
          "[settings_menu][reset]") {
    ScopedStore store("reset_hidden");
    app::AppState state{};
    SettingsMenuManager sm{&state};
    ui_test::fake_reset();
    sm.open();
    sm.enter_submenu(ui::MenuSection::SYSTEM);
    CHECK(row_index(sm, "Reset Controller Setup") == -1);
}

TEST_CASE("Reset Controller Setup confirms, then restores the built-in mapping",
          "[settings_menu][reset]") {
    ScopedStore store("reset_confirm");
    seed_store();

    app::AppState state{};
    SettingsMenuManager sm{&state};
    ui_test::fake_reset();
    sm.open();
    sm.enter_submenu(ui::MenuSection::SYSTEM);

    const int row = row_index(sm, "Reset Controller Setup");
    REQUIRE(row >= 0);
    sm.navigate(row);
    REQUIRE(sm.get_selected_index() == row);

    // First press ARMS. Nothing is destroyed yet, and the row says so.
    REQUIRE(sm.select_current() == ui::MenuSection::SYSTEM);
    CHECK(retroarch::load_profile_store().size() == 1);
    CHECK(row_index(sm, "Confirm: erase controller setup?") >= 0);
    CHECK_FALSE(sm.take_controller_profiles_dirty());

    // main.cpp answers a SYSTEM section by re-entering the submenu; that must
    // not silently disarm the confirm (the trap the Wi-Fi row hit).
    sm.enter_submenu(ui::MenuSection::SYSTEM);
    REQUIRE(row_index(sm, "Confirm: erase controller setup?") >= 0);

    // Second press (same row, selection preserved across the rebuild) erases.
    REQUIRE(sm.get_selected_index() == row_index(sm, "Confirm:"));
    REQUIRE(sm.select_current() == ui::MenuSection::SYSTEM);
    CHECK(retroarch::load_profile_store().empty());
    CHECK(ui_test::fake_last_toast().rfind("Controller setup reset", 0) == 0);
    // ...and the row is gone, because there is nothing left to reset.
    CHECK(row_index(sm, "Reset Controller Setup") == -1);
    CHECK(row_index(sm, "Confirm: erase controller setup?") == -1);

    // main.cpp must rebuild the menu-nav overlays: an overlay derived from a
    // profile that no longer exists would keep remapping menu buttons.
    CHECK(sm.take_controller_profiles_dirty());
    CHECK_FALSE(sm.take_controller_profiles_dirty());   // consume-once
}

TEST_CASE("leaving the System submenu disarms the reset confirm",
          "[settings_menu][reset]") {
    ScopedStore store("reset_disarm");
    seed_store();

    app::AppState state{};
    SettingsMenuManager sm{&state};
    ui_test::fake_reset();
    sm.open();
    sm.enter_submenu(ui::MenuSection::SYSTEM);
    sm.navigate(row_index(sm, "Reset Controller Setup"));
    REQUIRE(sm.select_current() == ui::MenuSection::SYSTEM);
    REQUIRE(row_index(sm, "Confirm:") >= 0);

    sm.exit_submenu();
    sm.enter_submenu(ui::MenuSection::SYSTEM);
    CHECK(row_index(sm, "Confirm:") == -1);
    CHECK(row_index(sm, "Reset Controller Setup") >= 0);
    CHECK(retroarch::load_profile_store().size() == 1);   // nothing destroyed
}

TEST_CASE("retiring the wizard touches neither pairing nor settings.json",
          "[settings_menu][wizard]") {
    // close()/force_close() also call close_pairing_screen(), and the menu's
    // toggle handlers persist settings. No wizard cancel path should reach
    // either — a cancel is not a state change worth writing to disk.
    Fixture f;
    f.drive_to(Phase::CAPTURE);
    f.sm.toggle();
    f.sm.force_close();
    CHECK(ui_test::fake_pairing_calls() == 0);
    CHECK(ui_test::fake_settings_saves() == 0);
}
