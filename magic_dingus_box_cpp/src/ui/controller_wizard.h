#pragma once
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "platform/bt_pairing.h"
#include "platform/input_manager.h"
#include "retroarch/capture_session.h"

namespace ui {

// Short, screen-friendly label for a logical control ("CROSS", "C UP",
// "LSTICK UP"). Derived from logical_control_key() rather than from a second
// hand-written table, so it cannot drift out of sync with the vocabulary.
std::string short_control_label(retroarch::LogicalControl c);

// Controller Setup wizard: the screen behind Settings -> Controller Setup.
//
// State + behavior only -- the drawing lives in controller_wizard_renderer.cpp
// (a private Renderer method, same split as the Phone Remote pairing screen).
//
// TWO PROPERTIES THIS CLASS EXISTS TO GUARANTEE:
//
//  1. THE KIOSK IS NEVER TRAPPED. This appliance has no keyboard, and the
//     whole point of the wizard is that the pad being configured may be
//     unusable. So every phase must be escapable WITHOUT that pad:
//       - SETTINGS_MENU (box BTN4 / phone black) or QUIT cancels from every
//         phase, including PICK_DEVICE (which never sees an InputAction from
//         the target pad at all, because raw capture swallows those);
//       - tick() self-closes after 2 minutes of no input of any kind;
//       - tick() also self-closes when the target pad is unplugged mid-run.
//     There is deliberately NO phase whose only exit is "make the broken pad
//     work".
//
//  2. THE PAD BEING CONFIGURED DRIVES NOTHING BUT CAPTURE. While the wizard
//     is open, InputManager::set_raw_capture(true) diverts real joysticks to
//     raw events (on_raw_event) and leaves keyboards, the rotary encoder, the
//     box buttons and the phone remote producing normal InputActions
//     (on_action). Chrome -- style choice, skip, redo, cancel, save -- is
//     therefore always driveable from a surface that is known to work.
//
// PAIRING (Settings -> Pair Wireless Controller, or Green on PICK_DEVICE):
//
//   PAIR_SEARCH  a BlueZ session looks for a controller in pairing mode and
//                pairs, trusts and connects it (platform::bt::PairingFlow;
//                the screen only shows its state). Red / knob retries a
//                failed pairing; the knob closes the screen when the box has
//                no usable Bluetooth.
//   PAIR_PRESS   connected; "press any button on it". The press claims the
//                pad exactly like PICK_DEVICE does (other pads are ignored
//                when BlueZ told us the new pad's IDs).
//   then         a saved/built-in profile -> DONE ("Ready to play!");
//                an automatic layout (retroarch/standard_layout.h) ->
//                AUTO_READY; otherwise -> PICK_STYLE, the mapping steps.
//
// AUTO_READY is the TEST screen for an automatic layout: the controls light
// as they are pressed. Start on the pad itself, or the knob, finishes;
// Yellow maps the buttons by hand instead. Nothing is saved -- the automatic
// layout is recomputed from the pad every time, so it improves with
// updates. PICK_DEVICE also lands here for a wired pad with an automatic
// layout and no saved profile.
//
// The BlueZ session runs only while PAIR_SEARCH is up; every way out of the
// wizard ends it (BluetoothPort::stop() never blocks).
class ControllerWizard {
public:
    enum class Phase {
        PICK_DEVICE, PICK_STYLE, CAPTURE, TEST, DONE,
        PAIR_SEARCH, PAIR_PRESS, AUTO_READY,
    };

    void open(platform::InputManager* input);   // enables raw capture
    // open() straight into PAIR_SEARCH.
    void open_for_pairing(platform::InputManager* input);
    void close();                               // disables raw capture
    bool is_active() const { return active_; }

    // The BlueZ session pairing uses. nullptr (the default, and every Mac
    // build) means "no Bluetooth": pairing shows that instead of searching.
    void set_bluetooth(platform::bt::BluetoothPort* port) { bt_ = port; }
    bool can_pair() const { return bt_ != nullptr; }
    // Pairing state for the renderer; nullptr outside the pairing phases.
    const platform::bt::PairingFlow* pairing() const { return flow_.get(); }
    // DONE's headline: "Saved!" after the mapping steps, "Ready to play!"
    // after pairing or an automatic layout.
    const std::string& done_title() const { return done_title_; }

    // Called every frame from main.cpp with the drained raw events.
    void on_raw_event(const platform::RawInputEvent& ev);
    // InputActions from OTHER surfaces (box buttons, rotary, phone remote,
    // keyboard). Returns true when consumed.
    bool on_action(const platform::InputEvent& ev);
    // Inactivity timeout; returns false when the wizard closed itself.
    bool tick();

    // Renderer readouts
    Phase phase() const { return phase_; }
    const std::string& device_name() const { return device_name_; }
    int style_cursor() const { return style_cursor_; }       // 0=PS, 1=N64
    retroarch::ControllerStyle style() const;
    std::string prompt() const;                              // current step text
    size_t step_index() const; size_t step_count() const;
    std::string status_line() const;                         // duplicate/skip feedback
    // Is this session complete enough to persist?
    //
    // A captured profile SHADOWS the builtin one for its VID/PID in
    // resolve_mapping_for_pad()'s captured -> builtin -> legacy order, and the
    // file is deliberately OTA-immune. So an under-captured profile does not
    // merely give a worse mapping -- it disables the pad in games, on BOTH
    // players (the two fielded pads share a VID/PID), and the hotkey block is
    // skipped wholesale when enable_hotkey_btn resolves to "", taking
    // Z+Start / Select+Start (the only way into the RetroArch menu) with it.
    //
    // "Non-empty" was too weak a bar: capture one control, skip 23, press
    // Select, and that is what shipped. The bar is now
    // retroarch::required_controls(style) -- the four d-pad directions plus
    // confirm and Start. missing_required() names what is still outstanding so
    // the TEST screen can say so instead of just withholding the option.
    std::vector<retroarch::LogicalControl> missing_required() const;
    bool can_save() const { return missing_required().empty(); }
    // "Still needed: D-PAD UP, CROSS" — empty when nothing is missing.
    std::string missing_required_line() const;
    // TEST phase: which captured controls are currently "lit"
    const std::map<retroarch::LogicalControl, bool>& test_lit() const;
    // Bindings captured SO FAR. Kept in sync after every feed/skip/redo, not
    // only at the end, so the CAPTURE screen's step list can show which
    // earlier steps landed and which were skipped.
    const std::map<retroarch::LogicalControl, retroarch::PhysicalBinding>& captured() const;

private:
    bool active_ = false;
    Phase phase_ = Phase::PICK_DEVICE;
    platform::InputManager* input_ = nullptr;
    uint16_t vid_ = 0, pid_ = 0;
    std::string device_name_;
    int style_cursor_ = 0;
    std::optional<retroarch::CaptureDeviceCaps> caps_;
    std::unique_ptr<retroarch::CaptureSession> session_;
    std::map<retroarch::LogicalControl, retroarch::PhysicalBinding> captured_;
    std::map<retroarch::LogicalControl, bool> test_lit_;
    std::string status_;
    std::chrono::steady_clock::time_point last_input_;
    // device_caps() walks every open device and queries libevdev per code, so
    // the unplug check runs at 1 Hz rather than per frame.
    std::chrono::steady_clock::time_point last_caps_poll_;

    platform::bt::BluetoothPort* bt_ = nullptr;
    bool bt_session_ = false;
    std::unique_ptr<platform::bt::PairingFlow> flow_;
    platform::bt::PairingFlow::Phase last_flow_phase_{};
    // The paired pad's IDs from BlueZ's modalias, when it reported one.
    std::optional<std::pair<uint16_t, uint16_t>> paired_ids_;
    std::string done_title_ = "Saved!";

    void begin_pairing_();
    void end_pairing_session_();
    void pump_pairing_(std::chrono::steady_clock::time_point now);
    // PICK_DEVICE / PAIR_PRESS: this press picks the pad. Decides the next
    // phase from the profile store and the automatic layout.
    void claim_device_(const platform::RawInputEvent& ev, bool after_pairing);
    void enter_auto_ready_(const retroarch::PhysicalProfile& profile);
    void finish_ready_();

    // False when the session is too incomplete to save (nothing written), and
    // ALSO false when the write itself failed -- the caller distinguishes the
    // two by checking can_save() first.
    bool save_profile_();
    void start_capture_();        // (re)build session_ for the chosen style
    void sync_captured_();        // mirror session_->results() into captured_
    void finish_capture_();       // session complete -> TEST phase
    // TEST phase: given a captured binding and one raw event from the target
    // pad, nullopt when the event does not concern that binding at all,
    // otherwise the binding's new lit state.
    std::optional<bool> binding_lit_(const retroarch::PhysicalBinding& b,
                                     const platform::RawInputEvent& ev) const;
};

}  // namespace ui
