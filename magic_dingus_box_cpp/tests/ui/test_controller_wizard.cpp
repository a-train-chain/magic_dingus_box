// ControllerWizard behavior tests.
//
// The wizard's collaborators (InputManager, Toast) are supplied by
// ui_test_doubles.cpp as link seams, so everything below is the real
// ControllerWizard driven exactly the way main.cpp drives it: raw events in
// via on_raw_event(), everything else via on_action(), tick() once a frame.
//
// The heaviest emphasis is on ESCAPE PATHS. This box has no keyboard, and the
// wizard is the one screen that deliberately makes the user's gamepad stop
// working — a phase with no exit would strand the appliance.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "retroarch/controller_profile.h"
#include "retroarch/logical_controls.h"
#include "ui/controller_wizard.h"
#include "ui_test_doubles.h"

using ui::ControllerWizard;
using Phase = ui::ControllerWizard::Phase;
using platform::InputAction;

namespace {

constexpr uint16_t kVid = 0x1234, kPid = 0x5678;
constexpr uint16_t kOtherVid = 0x0079, kOtherPid = 0x0006;
// linux/input-event-codes.h numbering, hardcoded because that header does
// not exist on macOS where this binary builds.
constexpr uint16_t kEvKey = 0x01, kEvAbs = 0x03;
constexpr uint16_t kAbsX = 0x00, kAbsHat0Y = 0x11;
constexpr uint16_t kBtn0 = 0x130;

retroarch::CaptureDeviceCaps make_caps() {
    retroarch::CaptureDeviceCaps c;
    c.vid = kVid;
    c.pid = kPid;
    c.name = "Test Pad";
    for (uint16_t i = 0; i < 32; ++i) c.key_codes.push_back(kBtn0 + i);
    c.axes.push_back({kAbsX, -32768, 32767, 0});
    c.axes.push_back({0x01, -32768, 32767, 0});
    c.axes.push_back({0x10, -1, 1, 0});
    c.axes.push_back({kAbsHat0Y, -1, 1, 0});
    return c;
}

platform::RawInputEvent raw(uint16_t type, uint16_t code, int32_t value,
                            uint16_t vid = kVid, uint16_t pid = kPid) {
    platform::RawInputEvent e;
    e.vid = vid;
    e.pid = pid;
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

// Press + release one button on the target pad: one completed capture.
void press_button(ControllerWizard& w, uint16_t code) {
    w.on_raw_event(raw(kEvKey, code, 1));
    w.on_raw_event(raw(kEvKey, code, 0));
}

// Fresh temp store per test so save/merge assertions can't see each other.
struct ScopedStore {
    std::string path;
    explicit ScopedStore(const std::string& tag) {
        path = (std::filesystem::temp_directory_path() /
                ("mdb_wizard_" + tag + ".json")).string();
        std::filesystem::remove(path);
        ::setenv("MAGIC_CONTROLLER_PROFILES_FILE", path.c_str(), 1);
    }
    ~ScopedStore() {
        std::filesystem::remove(path);
        ::unsetenv("MAGIC_CONTROLLER_PROFILES_FILE");
    }
};

// open() + pick the device + confirm the style, i.e. land in CAPTURE.
void open_to_capture(ControllerWizard& w, platform::InputManager& im,
                     bool n64_style = false) {
    ui_test::fake_reset();
    ui_test::fake_set_caps(make_caps());
    w.open(&im);
    press_button(w, kBtn0);                 // any press picks the pad
    REQUIRE(w.phase() == Phase::PICK_STYLE);
    if (n64_style) w.on_action(act(InputAction::ROTATE, true, 1));
    w.on_action(act(InputAction::SELECT));
    REQUIRE(w.phase() == Phase::CAPTURE);
}

// Capture every step with a distinct button, landing in TEST.
void capture_all(ControllerWizard& w) {
    const size_t n = w.step_count();
    for (size_t i = 0; i < n; ++i) press_button(w, static_cast<uint16_t>(kBtn0 + i));
    REQUIRE(w.phase() == Phase::TEST);
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle + raw-capture handshake
// ---------------------------------------------------------------------------

TEST_CASE("open enables raw capture and starts at PICK_DEVICE", "[wizard]") {
    ui_test::fake_reset();
    platform::InputManager im;
    ControllerWizard w;
    CHECK_FALSE(w.is_active());

    w.open(&im);
    CHECK(w.is_active());
    CHECK(w.phase() == Phase::PICK_DEVICE);
    CHECK(ui_test::fake_raw_capture_enabled());
}

TEST_CASE("close disables raw capture and resets state", "[wizard]") {
    ui_test::fake_reset();
    ui_test::fake_set_caps(make_caps());
    platform::InputManager im;
    ControllerWizard w;
    w.open(&im);
    press_button(w, kBtn0);
    REQUIRE(w.phase() == Phase::PICK_STYLE);

    w.close();
    CHECK_FALSE(w.is_active());
    CHECK_FALSE(ui_test::fake_raw_capture_enabled());
    CHECK(w.phase() == Phase::PICK_DEVICE);
    CHECK(w.device_name().empty());

    // Idempotent: a second close (main.cpp's pump racing the dispatch loop)
    // must not touch raw capture again.
    const int calls = ui_test::fake_raw_capture_calls();
    w.close();
    CHECK(ui_test::fake_raw_capture_calls() == calls);
}

// ---------------------------------------------------------------------------
// PICK_DEVICE
// ---------------------------------------------------------------------------

TEST_CASE("PICK_DEVICE claims the pad that pressed a button", "[wizard]") {
    ui_test::fake_reset();
    ui_test::fake_set_caps(make_caps());
    platform::InputManager im;
    ControllerWizard w;
    w.open(&im);

    // A release, and an axis wobble, must not claim anything.
    w.on_raw_event(raw(kEvKey, kBtn0, 0));
    w.on_raw_event(raw(kEvAbs, kAbsX, 32000));
    CHECK(w.phase() == Phase::PICK_DEVICE);

    w.on_raw_event(raw(kEvKey, kBtn0, 1));
    CHECK(w.phase() == Phase::PICK_STYLE);
    CHECK(w.device_name() == "Test Pad");
}

TEST_CASE("PICK_DEVICE stays put when the pad's caps can't be read", "[wizard]") {
    ui_test::fake_reset();   // no caps registered at all
    platform::InputManager im;
    ControllerWizard w;
    w.open(&im);

    w.on_raw_event(raw(kEvKey, kBtn0, 1));
    CHECK(w.phase() == Phase::PICK_DEVICE);
    CHECK_FALSE(w.status_line().empty());
    CHECK(w.is_active());          // not closed — another pad can still be tried

    // ...and the retry works once the device is readable.
    ui_test::fake_set_caps(make_caps());
    w.on_raw_event(raw(kEvKey, kBtn0, 1));
    CHECK(w.phase() == Phase::PICK_STYLE);
}

// ---------------------------------------------------------------------------
// PICK_STYLE
// ---------------------------------------------------------------------------

TEST_CASE("PICK_STYLE toggles and confirms the style", "[wizard]") {
    ui_test::fake_reset();
    ui_test::fake_set_caps(make_caps());
    platform::InputManager im;
    ControllerWizard w;
    w.open(&im);
    press_button(w, kBtn0);
    REQUIRE(w.phase() == Phase::PICK_STYLE);
    CHECK(w.style_cursor() == 0);

    w.on_action(act(InputAction::ROTATE, true, 1));
    CHECK(w.style_cursor() == 1);
    w.on_action(act(InputAction::ROTATE_VERTICAL, true, -1));
    CHECK(w.style_cursor() == 0);

    w.on_action(act(InputAction::SELECT));
    CHECK(w.phase() == Phase::CAPTURE);
    CHECK(w.step_count() == retroarch::capture_steps(
                                retroarch::ControllerStyle::PS_STYLE).size());
    CHECK(w.prompt() == retroarch::control_prompt(retroarch::LogicalControl::DPAD_UP));
}

TEST_CASE("PICK_STYLE N64 choice drives the N64 prompt list", "[wizard]") {
    platform::InputManager im;
    ControllerWizard w;
    open_to_capture(w, im, /*n64_style=*/true);
    CHECK(w.step_count() == retroarch::capture_steps(
                                retroarch::ControllerStyle::N64_STYLE).size());
    CHECK(w.prompt() ==
          retroarch::control_prompt(retroarch::LogicalControl::N64_DPAD_UP));
}

TEST_CASE("PICK_STYLE ignores the target pad's own buttons", "[wizard]") {
    ui_test::fake_reset();
    ui_test::fake_set_caps(make_caps());
    platform::InputManager im;
    ControllerWizard w;
    w.open(&im);
    press_button(w, kBtn0);
    REQUIRE(w.phase() == Phase::PICK_STYLE);

    // Nothing is known about this pad's layout yet, so it must not be able
    // to pick a style or advance.
    press_button(w, kBtn0 + 3);
    CHECK(w.phase() == Phase::PICK_STYLE);
    CHECK(w.style_cursor() == 0);
}

// ---------------------------------------------------------------------------
// CAPTURE
// ---------------------------------------------------------------------------

TEST_CASE("CAPTURE advances on each captured control", "[wizard]") {
    platform::InputManager im;
    ControllerWizard w;
    open_to_capture(w, im);
    REQUIRE(w.step_index() == 0);

    press_button(w, kBtn0);
    CHECK(w.step_index() == 1);
    CHECK(w.captured().count(retroarch::LogicalControl::DPAD_UP) == 1);
    CHECK(w.prompt() ==
          retroarch::control_prompt(retroarch::LogicalControl::DPAD_DOWN));
}

TEST_CASE("CAPTURE ignores raw events from other pads", "[wizard]") {
    platform::InputManager im;
    ControllerWizard w;
    open_to_capture(w, im);

    w.on_raw_event(raw(kEvKey, kBtn0, 1, kOtherVid, kOtherPid));
    w.on_raw_event(raw(kEvKey, kBtn0, 0, kOtherVid, kOtherPid));
    CHECK(w.step_index() == 0);
    CHECK(w.captured().empty());
}

TEST_CASE("CAPTURE reports a duplicate instead of advancing", "[wizard]") {
    platform::InputManager im;
    ControllerWizard w;
    open_to_capture(w, im);

    press_button(w, kBtn0);
    REQUIRE(w.step_index() == 1);
    press_button(w, kBtn0);           // same button again
    CHECK(w.step_index() == 1);
    CHECK(w.status_line().find("DPAD UP") != std::string::npos);
    // ...and the message names the way forward. On pads that share one axis
    // pair between the d-pad and the left stick, EVERY stick step is a
    // legitimate duplicate and skipping is the only correct answer; the
    // operator should not have to work that out.
    CHECK(w.status_line().find("skip") != std::string::npos);
}

TEST_CASE("CAPTURE skip and redo move the cursor", "[wizard]") {
    platform::InputManager im;
    ControllerWizard w;
    open_to_capture(w, im);

    w.on_action(act(InputAction::PLAY_PAUSE));       // BTN2 = skip
    CHECK(w.step_index() == 1);
    CHECK(w.captured().count(retroarch::LogicalControl::DPAD_UP) == 0);

    press_button(w, kBtn0 + 1);
    REQUIRE(w.step_index() == 2);
    w.on_action(act(InputAction::PREV));             // BTN1 = redo
    CHECK(w.step_index() == 1);
    CHECK(w.captured().count(retroarch::LogicalControl::DPAD_DOWN) == 0);

    // Redo at the very first step is a no-op, not an underflow.
    w.on_action(act(InputAction::PREV));
    w.on_action(act(InputAction::PREV));
    CHECK(w.step_index() == 0);
    CHECK(w.phase() == Phase::CAPTURE);
}

TEST_CASE("CAPTURE accepts hat and axis gestures", "[wizard]") {
    platform::InputManager im;
    ControllerWizard w;
    open_to_capture(w, im);

    w.on_raw_event(raw(kEvAbs, kAbsHat0Y, -1));   // hat up
    w.on_raw_event(raw(kEvAbs, kAbsHat0Y, 0));
    REQUIRE(w.step_index() == 1);
    const auto* up = &w.captured().at(retroarch::LogicalControl::DPAD_UP);
    CHECK(up->kind == retroarch::PhysicalBinding::Kind::HAT);
    CHECK(up->direction == -1);

    w.on_raw_event(raw(kEvAbs, kAbsHat0Y, 1));    // hat down
    w.on_raw_event(raw(kEvAbs, kAbsHat0Y, 0));
    CHECK(w.step_index() == 2);
    CHECK(w.captured().at(retroarch::LogicalControl::DPAD_DOWN).direction == 1);
}

TEST_CASE("skipping the LAST step still finishes the session", "[wizard]") {
    // Regression guard: a session that becomes done() via skip rather than
    // via a capture used to leave the wizard on a prompt-less CAPTURE screen
    // with nothing to press.
    platform::InputManager im;
    ControllerWizard w;
    open_to_capture(w, im);

    const size_t n = w.step_count();
    for (size_t i = 0; i < n; ++i) w.on_action(act(InputAction::PLAY_PAUSE));
    CHECK(w.phase() == Phase::TEST);
    CHECK(w.captured().empty());
    CHECK(w.is_active());
}

// ---------------------------------------------------------------------------
// TEST phase + save
// ---------------------------------------------------------------------------

TEST_CASE("TEST lights the control that is currently pressed", "[wizard]") {
    platform::InputManager im;
    ControllerWizard w;
    open_to_capture(w, im);
    capture_all(w);

    const auto up = retroarch::LogicalControl::DPAD_UP;
    CHECK(w.test_lit().at(up) == false);
    w.on_raw_event(raw(kEvKey, kBtn0, 1));
    CHECK(w.test_lit().at(up) == true);
    w.on_raw_event(raw(kEvKey, kBtn0, 0));
    CHECK(w.test_lit().at(up) == false);

    // Another pad's identical code must not light anything.
    w.on_raw_event(raw(kEvKey, kBtn0, 1, kOtherVid, kOtherPid));
    CHECK(w.test_lit().at(up) == false);
}

TEST_CASE("TEST redo restarts capture from step 0", "[wizard]") {
    platform::InputManager im;
    ControllerWizard w;
    open_to_capture(w, im);
    capture_all(w);

    w.on_action(act(InputAction::PREV));
    CHECK(w.phase() == Phase::CAPTURE);
    CHECK(w.step_index() == 0);
    CHECK(w.captured().empty());
    CHECK(w.test_lit().empty());
}

TEST_CASE("saving writes this pad's profile without losing the others", "[wizard]") {
    ScopedStore store("merge");

    // A pre-existing entry for a different pad.
    {
        std::map<std::string, retroarch::PhysicalProfile> pre;
        pre[retroarch::vidpid_key(kOtherVid, kOtherPid)] =
            retroarch::builtin_dragonrise_profile();
        REQUIRE(retroarch::save_profile_store(pre));
    }

    platform::InputManager im;
    ControllerWizard w;
    open_to_capture(w, im);
    capture_all(w);
    w.on_action(act(InputAction::SELECT));
    CHECK(w.phase() == Phase::DONE);
    CHECK(ui_test::fake_last_toast() == "Controller saved: Test Pad");

    const auto after = retroarch::load_profile_store();
    CHECK(after.size() == 2);
    REQUIRE(after.count(retroarch::vidpid_key(kOtherVid, kOtherPid)) == 1);
    REQUIRE(after.count(retroarch::vidpid_key(kVid, kPid)) == 1);

    const auto& saved = after.at(retroarch::vidpid_key(kVid, kPid));
    CHECK(saved.name == "Test Pad");
    CHECK(saved.vid == kVid);
    CHECK(saved.pid == kPid);
    CHECK(saved.style == retroarch::ControllerStyle::PS_STYLE);
    CHECK(saved.controls.size() ==
          retroarch::capture_steps(retroarch::ControllerStyle::PS_STYLE).size());
}

TEST_CASE("re-running the wizard replaces that pad's entry in place", "[wizard]") {
    ScopedStore store("replace");
    platform::InputManager im;

    for (int round = 0; round < 2; ++round) {
        ControllerWizard w;
        open_to_capture(w, im);
        capture_all(w);
        w.on_action(act(InputAction::SELECT));
        REQUIRE(w.phase() == Phase::DONE);
        w.on_action(act(InputAction::SELECT));   // dismiss
    }
    CHECK(retroarch::load_profile_store().size() == 1);
}

TEST_CASE("an all-skipped session can never be saved", "[wizard]") {
    // Skipping every step produces a session that is COMPLETE — so it reaches
    // TEST, where save normally lives — but captured nothing. Persisting that
    // writes an empty profile keyed by this pad's VID/PID, and a captured
    // profile SHADOWS the builtin one in resolve_profile()'s captured ->
    // builtin -> legacy order. A pad that worked fine off its builtin profile
    // would silently go dead in RetroArch.
    ScopedStore store("all_skipped");
    platform::InputManager im;
    ControllerWizard w;
    open_to_capture(w, im);

    const size_t n = w.step_count();
    for (size_t i = 0; i < n; ++i) w.on_action(act(InputAction::PLAY_PAUSE));
    REQUIRE(w.phase() == Phase::TEST);
    REQUIRE(w.captured().empty());
    CHECK_FALSE(w.can_save());

    // SELECT is consumed (the wizard owns the screen) but writes nothing and
    // does not advance to DONE — the operator stays where redo and cancel are.
    CHECK(w.on_action(act(InputAction::SELECT)));
    CHECK(w.phase() == Phase::TEST);
    CHECK(w.is_active());
    CHECK(retroarch::load_profile_store().empty());
    CHECK_FALSE(w.status_line().empty());

    // Redo, then capture for real, and saving works again.
    w.on_action(act(InputAction::PREV));
    REQUIRE(w.phase() == Phase::CAPTURE);
    capture_all(w);
    CHECK(w.can_save());
    w.on_action(act(InputAction::SELECT));
    CHECK(w.phase() == Phase::DONE);
    CHECK(retroarch::load_profile_store().size() == 1);
}

// ---------------------------------------------------------------------------
// can_save() is a COMPLETENESS gate, not an emptiness gate
// ---------------------------------------------------------------------------
//
// The fielded failure this pins: capture one control, skip the other 23,
// press Select. That wrote a one-control profile keyed by this pad's VID/PID,
// which unconditionally shadows the builtin (resolve_mapping_for_pad) for BOTH
// players -- the two shipped pads share a VID/PID -- and left
// enable_hotkey_btn empty, so retroarch_launcher skips the hotkey block
// entirely and Select+Start no longer opens the RetroArch menu. Recovery meant
// completing all 24 steps again, on a pad that no longer worked.

TEST_CASE("one captured control is not enough to save", "[wizard][can_save]") {
    ScopedStore store("one_control");
    platform::InputManager im;
    ControllerWizard w;
    open_to_capture(w, im);

    press_button(w, kBtn0);                       // capture step 0 (D-pad up)
    const size_t n = w.step_count();
    for (size_t i = 1; i < n; ++i) w.on_action(act(InputAction::PLAY_PAUSE));
    REQUIRE(w.phase() == Phase::TEST);
    REQUIRE(w.captured().size() == 1);            // non-empty: the old bar

    CHECK_FALSE(w.can_save());
    CHECK(w.on_action(act(InputAction::SELECT)));  // consumed, but refused
    CHECK(w.phase() == Phase::TEST);
    CHECK(retroarch::load_profile_store().empty());
    // ...and the operator is told what is outstanding, not just refused.
    CHECK(w.status_line().find("DPAD DOWN") != std::string::npos);
    CHECK(w.status_line().find("CROSS") != std::string::npos);
    CHECK(w.status_line().find("START") != std::string::npos);
}

TEST_CASE("can_save requires the d-pad plus confirm and Start", "[wizard][can_save]") {
    platform::InputManager im;

    SECTION("PS-style") {
        ControllerWizard w;
        open_to_capture(w, im);
        const auto steps = retroarch::capture_steps(retroarch::ControllerStyle::PS_STYLE);
        const auto required = retroarch::required_controls(retroarch::ControllerStyle::PS_STYLE);
        // Capture exactly the required set, skip everything else.
        for (size_t i = 0; i < steps.size(); ++i) {
            const bool needed = std::find(required.begin(), required.end(),
                                          steps[i]) != required.end();
            if (needed) press_button(w, static_cast<uint16_t>(kBtn0 + i));
            else w.on_action(act(InputAction::PLAY_PAUSE));
        }
        REQUIRE(w.phase() == Phase::TEST);
        CHECK(w.captured().size() == required.size());
        CHECK(w.can_save());
        CHECK(w.missing_required_line().empty());
    }

    SECTION("PS-style, one d-pad direction short") {
        ControllerWizard w;
        open_to_capture(w, im);
        const auto steps = retroarch::capture_steps(retroarch::ControllerStyle::PS_STYLE);
        const auto required = retroarch::required_controls(retroarch::ControllerStyle::PS_STYLE);
        for (size_t i = 0; i < steps.size(); ++i) {
            const bool needed = std::find(required.begin(), required.end(),
                                          steps[i]) != required.end();
            if (needed && steps[i] != retroarch::LogicalControl::DPAD_RIGHT)
                press_button(w, static_cast<uint16_t>(kBtn0 + i));
            else
                w.on_action(act(InputAction::PLAY_PAUSE));
        }
        REQUIRE(w.phase() == Phase::TEST);
        CHECK_FALSE(w.can_save());
        CHECK(w.missing_required_line() == "Still needed: DPAD RIGHT");
    }

    SECTION("N64-style uses the N64 vocabulary") {
        ControllerWizard w;
        open_to_capture(w, im, /*n64_style=*/true);
        const auto steps = retroarch::capture_steps(retroarch::ControllerStyle::N64_STYLE);
        const auto required = retroarch::required_controls(retroarch::ControllerStyle::N64_STYLE);
        for (size_t i = 0; i < steps.size(); ++i) {
            const bool needed = std::find(required.begin(), required.end(),
                                          steps[i]) != required.end();
            if (needed) press_button(w, static_cast<uint16_t>(kBtn0 + i));
            else w.on_action(act(InputAction::PLAY_PAUSE));
        }
        REQUIRE(w.phase() == Phase::TEST);
        CHECK(w.can_save());
    }
}

TEST_CASE("a failed save reports failure instead of Saved!", "[wizard][save]") {
    // The store path's parent is a regular FILE, so save_profile_store()'s
    // create_directories() fails and the write never happens. Stands in for
    // the real failure modes on the box: read-only /opt, full SD card, bad
    // ownership on config/. Before this fix save_profile_() discarded
    // save_profile_store()'s return value and returned true unconditionally,
    // so the wizard advanced to DONE and painted "Saved!" over a write that
    // did not occur — on an appliance with no other diagnostic surface.
    struct BlockedStore {
        std::filesystem::path blocker;
        BlockedStore() {
            blocker = std::filesystem::temp_directory_path() / "mdb_wizard_blocker";
            std::filesystem::remove_all(blocker);
            std::ofstream(blocker.string()) << "not a directory";
            ::setenv("MAGIC_CONTROLLER_PROFILES_FILE",
                     (blocker / "sub" / "profiles.json").string().c_str(), 1);
        }
        ~BlockedStore() {
            ::unsetenv("MAGIC_CONTROLLER_PROFILES_FILE");
            std::error_code ec;
            std::filesystem::remove(blocker, ec);
        }
    } blocked;
    REQUIRE_FALSE(retroarch::save_profile_store({}));   // the premise holds

    platform::InputManager im;
    ControllerWizard w;
    open_to_capture(w, im);
    capture_all(w);
    REQUIRE(w.can_save());

    CHECK(w.on_action(act(InputAction::SELECT)));
    CHECK(w.phase() == Phase::TEST);          // no "Saved!" screen
    CHECK(w.is_active());                     // ...and Select can be retried
    CHECK_FALSE(w.status_line().empty());
    CHECK(ui_test::fake_last_toast().find("NOT saved") != std::string::npos);

    // Point the store somewhere writable and the SAME press now succeeds.
    ScopedStore store("save_recovery");
    CHECK(w.on_action(act(InputAction::SELECT)));
    CHECK(w.phase() == Phase::DONE);
    CHECK(ui_test::fake_last_toast() == "Controller saved: Test Pad");
    CHECK(retroarch::load_profile_store().size() == 1);
}

TEST_CASE("can_save follows capture and redo", "[wizard][can_save]") {
    platform::InputManager im;
    ControllerWizard w;
    open_to_capture(w, im);
    CHECK_FALSE(w.can_save());          // nothing captured yet

    // Capture the whole required prefix (d-pad + Cross is steps 0..4)...
    for (uint16_t i = 0; i < 5; ++i) press_button(w, static_cast<uint16_t>(kBtn0 + i));
    CHECK_FALSE(w.can_save());          // START still outstanding
    CHECK(w.missing_required_line() == "Still needed: START");

    // ...skip to START (step 13) and capture it.
    while (w.step_index() < 13) w.on_action(act(InputAction::PLAY_PAUSE));
    press_button(w, kBtn0 + 13);
    CHECK(w.can_save());

    w.on_action(act(InputAction::PREV));   // redo START away again
    CHECK_FALSE(w.can_save());
}

TEST_CASE("a partially captured session can never be saved", "[wizard]") {
    ScopedStore store("partial");
    platform::InputManager im;
    ControllerWizard w;
    open_to_capture(w, im);

    press_button(w, kBtn0);
    press_button(w, kBtn0 + 1);
    REQUIRE(w.phase() == Phase::CAPTURE);

    // SELECT means nothing mid-capture; saving is only reachable from TEST,
    // which is only reachable from a completed session.
    w.on_action(act(InputAction::SELECT));
    CHECK(w.phase() == Phase::CAPTURE);
    CHECK(retroarch::load_profile_store().empty());

    // ...and cancelling from here writes nothing either.
    w.on_action(act(InputAction::SETTINGS_MENU));
    CHECK_FALSE(w.is_active());
    CHECK(retroarch::load_profile_store().empty());
}

// ---------------------------------------------------------------------------
// ESCAPE PATHS — one per phase, none of them via the pad being configured
// ---------------------------------------------------------------------------

// NOT THE PATH PRODUCTION TAKES. main.cpp consumes every
// InputAction::SETTINGS_MENU in its BTN4 hold/volume block and `continue`s, so
// the event never reaches on_action() on hardware; the operator's press
// arrives as settings_menu.toggle() instead. This branch is a legitimate
// defensive path (and the only cancel a keyboard-less QUIT-less surface could
// use if that dispatch ever changed), but coverage of the REAL escape hatch
// lives in test_settings_menu_wizard.cpp. Do not read this test as covering it.
TEST_CASE("SETTINGS_MENU cancels from every phase", "[wizard][escape]") {
    ScopedStore store("cancel");
    platform::InputManager im;

    SECTION("PICK_DEVICE") {
        ui_test::fake_reset();
        ControllerWizard w;
        w.open(&im);
        REQUIRE(w.phase() == Phase::PICK_DEVICE);
        CHECK(w.on_action(act(InputAction::SETTINGS_MENU)));
        CHECK_FALSE(w.is_active());
        CHECK_FALSE(ui_test::fake_raw_capture_enabled());
    }
    SECTION("PICK_STYLE") {
        ui_test::fake_reset();
        ui_test::fake_set_caps(make_caps());
        ControllerWizard w;
        w.open(&im);
        press_button(w, kBtn0);
        REQUIRE(w.phase() == Phase::PICK_STYLE);
        CHECK(w.on_action(act(InputAction::SETTINGS_MENU)));
        CHECK_FALSE(w.is_active());
        CHECK_FALSE(ui_test::fake_raw_capture_enabled());
    }
    SECTION("CAPTURE") {
        ControllerWizard w;
        open_to_capture(w, im);
        CHECK(w.on_action(act(InputAction::SETTINGS_MENU)));
        CHECK_FALSE(w.is_active());
        CHECK_FALSE(ui_test::fake_raw_capture_enabled());
    }
    SECTION("TEST") {
        ControllerWizard w;
        open_to_capture(w, im);
        capture_all(w);
        CHECK(w.on_action(act(InputAction::SETTINGS_MENU)));
        CHECK_FALSE(w.is_active());
        CHECK_FALSE(ui_test::fake_raw_capture_enabled());
    }
    SECTION("DONE") {
        ControllerWizard w;
        open_to_capture(w, im);
        capture_all(w);
        w.on_action(act(InputAction::SELECT));
        REQUIRE(w.phase() == Phase::DONE);
        CHECK(w.on_action(act(InputAction::SETTINGS_MENU)));
        CHECK_FALSE(w.is_active());
        CHECK_FALSE(ui_test::fake_raw_capture_enabled());
    }
}

TEST_CASE("QUIT cancels from every phase", "[wizard][escape]") {
    ScopedStore store("quit");
    platform::InputManager im;

    SECTION("PICK_DEVICE") {
        ui_test::fake_reset();
        ControllerWizard w;
        w.open(&im);
        CHECK(w.on_action(act(InputAction::QUIT)));
        CHECK_FALSE(w.is_active());
    }
    SECTION("CAPTURE") {
        ControllerWizard w;
        open_to_capture(w, im);
        CHECK(w.on_action(act(InputAction::QUIT)));
        CHECK_FALSE(w.is_active());
    }
    SECTION("TEST") {
        ControllerWizard w;
        open_to_capture(w, im);
        capture_all(w);
        CHECK(w.on_action(act(InputAction::QUIT)));
        CHECK_FALSE(w.is_active());
    }
}

TEST_CASE("DONE closes on SELECT", "[wizard][escape]") {
    ScopedStore store("done");
    platform::InputManager im;
    ControllerWizard w;
    open_to_capture(w, im);
    capture_all(w);
    w.on_action(act(InputAction::SELECT));
    REQUIRE(w.phase() == Phase::DONE);

    w.on_action(act(InputAction::SELECT));
    CHECK_FALSE(w.is_active());
    CHECK_FALSE(ui_test::fake_raw_capture_enabled());
}

TEST_CASE("tick is a no-op while the pad is present and the user is active",
          "[wizard][escape]") {
    platform::InputManager im;
    ControllerWizard w;
    open_to_capture(w, im);
    for (int i = 0; i < 10; ++i) CHECK(w.tick());
    CHECK(w.is_active());
}

TEST_CASE("unplugging the target pad cancels the wizard", "[wizard][escape]") {
    platform::InputManager im;

    SECTION("during CAPTURE") {
        ControllerWizard w;
        open_to_capture(w, im);
        ui_test::fake_unplug();
        // The unplug probe is rate-limited to 1 Hz; nothing should happen
        // before the first poll is due.
        CHECK(w.tick());
        CHECK(w.is_active());
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        CHECK_FALSE(w.tick());
        CHECK_FALSE(w.is_active());
        CHECK_FALSE(ui_test::fake_raw_capture_enabled());
        CHECK(ui_test::fake_last_toast() ==
              "Controller disconnected - setup cancelled");
    }
    SECTION("during TEST") {
        ControllerWizard w;
        open_to_capture(w, im);
        capture_all(w);
        ui_test::fake_unplug();
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        CHECK_FALSE(w.tick());
        CHECK_FALSE(w.is_active());
    }
    SECTION("DONE survives it — the profile is already on disk") {
        ScopedStore store("unplug_done");
        ControllerWizard w;
        open_to_capture(w, im);
        capture_all(w);
        w.on_action(act(InputAction::SELECT));
        REQUIRE(w.phase() == Phase::DONE);
        ui_test::fake_unplug();
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        CHECK(w.tick());
        CHECK(w.is_active());
    }
}

TEST_CASE("tick on a closed wizard is harmless", "[wizard][escape]") {
    ui_test::fake_reset();
    ControllerWizard w;
    CHECK(w.tick());
    CHECK_FALSE(w.on_action(act(InputAction::SELECT)));
    w.on_raw_event(raw(kEvKey, kBtn0, 1));   // must not crash or wake it up
    CHECK_FALSE(w.is_active());
}

// ---------------------------------------------------------------------------
// Labels
// ---------------------------------------------------------------------------

TEST_CASE("short_control_label is screen-friendly", "[wizard]") {
    CHECK(ui::short_control_label(retroarch::LogicalControl::CROSS) == "CROSS");
    CHECK(ui::short_control_label(retroarch::LogicalControl::DPAD_UP) == "DPAD UP");
    CHECK(ui::short_control_label(retroarch::LogicalControl::N64_C_UP) == "C UP");
    CHECK(ui::short_control_label(retroarch::LogicalControl::N64_STICK_LEFT) ==
          "STICK LEFT");
}
