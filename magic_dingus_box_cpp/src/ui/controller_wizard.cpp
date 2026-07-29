#include "controller_wizard.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "toast.h"
#include "retroarch/controller_profile.h"
#include "retroarch/logical_controls.h"

namespace ui {

namespace {

using platform::InputAction;

constexpr uint16_t kEvKey = 0x01;
constexpr uint16_t kEvAbs = 0x03;
constexpr uint16_t kHatFirst = 0x10, kHatLast = 0x17;

// No input of ANY kind for this long and the wizard puts itself away. This is
// one of the three unconditional escapes (see the header): a box left on the
// capture screen must return to Settings by itself.
constexpr std::chrono::seconds kIdleTimeout{120};
constexpr std::chrono::seconds kCapsPollPeriod{1};

// SETTINGS_MENU is box BTN4 / the phone remote's black button; QUIT is the
// keyboard's Esc/Q. Both cancel, from every phase.
bool is_cancel(const platform::InputEvent& ev) {
    return ev.pressed && (ev.action == InputAction::SETTINGS_MENU ||
                          ev.action == InputAction::QUIT);
}

}  // namespace

std::string short_control_label(retroarch::LogicalControl c) {
    std::string key = retroarch::logical_control_key(c);
    if (key.rfind("n64_", 0) == 0) key.erase(0, 4);
    for (char& ch : key) {
        if (ch == '_') ch = ' ';
        else ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return key;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ControllerWizard::open(platform::InputManager* input) {
    input_ = input;
    active_ = true;
    phase_ = Phase::PICK_DEVICE;
    vid_ = 0;
    pid_ = 0;
    device_name_.clear();
    style_cursor_ = 0;
    caps_.reset();
    session_.reset();
    captured_.clear();
    test_lit_.clear();
    status_.clear();
    last_input_ = std::chrono::steady_clock::now();
    last_caps_poll_ = last_input_;
    if (input_) input_->set_raw_capture(true);
}

void ControllerWizard::close() {
    // set_raw_capture(false) DISCARDS the pending raw-event queue (see the
    // ordering note on InputManager::set_raw_capture). That is correct here
    // and only here: by the time we close, whatever is still queued belongs
    // to a session we are throwing away. main.cpp drains the queue before it
    // can ever reach this call, which is what keeps a real capture from being
    // silently lost.
    if (input_) input_->set_raw_capture(false);
    input_ = nullptr;
    active_ = false;
    phase_ = Phase::PICK_DEVICE;
    vid_ = 0;
    pid_ = 0;
    device_name_.clear();
    style_cursor_ = 0;
    caps_.reset();
    session_.reset();
    captured_.clear();
    test_lit_.clear();
    status_.clear();
}

// ---------------------------------------------------------------------------
// Raw events (the pad being configured)
// ---------------------------------------------------------------------------

void ControllerWizard::on_raw_event(const platform::RawInputEvent& ev) {
    if (!active_) return;
    last_input_ = std::chrono::steady_clock::now();

    switch (phase_) {
        case Phase::PICK_DEVICE: {
            // Any button press on any pad claims it as the target. Presses
            // only (value==1): a pad that arrives mid-release, or an axis
            // resting off-center, must not pick itself.
            if (ev.type != kEvKey || ev.value != 1) break;
            vid_ = ev.vid;
            pid_ = ev.pid;
            device_name_ = ev.device_name;
            caps_ = input_ ? input_->device_caps(vid_, pid_) : std::nullopt;
            if (!caps_) {
                // Stay on this phase: another pad (or the same one, replugged)
                // can still be picked, and cancel is always available.
                status_ = "Couldn't read that controller - try another";
                break;
            }
            status_.clear();
            phase_ = Phase::PICK_STYLE;
            break;
        }

        case Phase::PICK_STYLE:
            // Deliberately inert. The target pad must not be able to choose
            // its own style: at this point nothing is known about which of
            // its buttons is which.
            break;

        case Phase::CAPTURE: {
            if (!session_) break;
            if (ev.vid != vid_ || ev.pid != pid_) break;  // other pads ignored
            switch (session_->feed(ev.type, ev.code, ev.value)) {
                case retroarch::CaptureSession::FeedResult::NONE:
                    break;
                case retroarch::CaptureSession::FeedResult::CAPTURED:
                    status_.clear();
                    sync_captured_();
                    break;
                case retroarch::CaptureSession::FeedResult::DUPLICATE:
                    status_ = "Already used for " +
                              short_control_label(session_->last_duplicate_of());
                    break;
                case retroarch::CaptureSession::FeedResult::DONE:
                    status_.clear();
                    finish_capture_();
                    break;
            }
            break;
        }

        case Phase::TEST: {
            if (ev.vid != vid_ || ev.pid != pid_) break;
            for (const auto& [control, binding] : captured_) {
                if (auto lit = binding_lit_(binding, ev)) test_lit_[control] = *lit;
            }
            break;
        }

        case Phase::DONE:
            break;
    }
}

// ---------------------------------------------------------------------------
// Actions (every OTHER input surface)
// ---------------------------------------------------------------------------

bool ControllerWizard::on_action(const platform::InputEvent& ev) {
    if (!active_) return false;
    last_input_ = std::chrono::steady_clock::now();

    // Cancel is checked before the per-phase switch precisely so it cannot be
    // forgotten in a phase. Every phase, no exceptions.
    if (is_cancel(ev)) {
        close();
        return true;
    }

    switch (phase_) {
        case Phase::PICK_DEVICE:
            // Nothing else to do here; the pad picks itself.
            return false;

        case Phase::PICK_STYLE:
            if ((ev.action == InputAction::ROTATE ||
                 ev.action == InputAction::ROTATE_VERTICAL) && ev.delta != 0) {
                style_cursor_ = style_cursor_ == 0 ? 1 : 0;
                return true;
            }
            if (ev.action == InputAction::SELECT && ev.pressed) {
                start_capture_();
                return true;
            }
            return false;

        case Phase::CAPTURE:
            if (!session_) return false;
            if (ev.action == InputAction::PLAY_PAUSE && ev.pressed) {   // BTN2
                session_->skip();
                status_ = "Skipped";
                sync_captured_();
                // Skipping the LAST step finishes the session just as a
                // capture does. Without this the wizard would sit on a done()
                // session with no prompt and no way forward.
                if (session_->done()) finish_capture_();
                return true;
            }
            if (ev.action == InputAction::PREV && ev.pressed) {         // BTN1
                if (session_->redo_last()) {
                    status_ = "Redo";
                    sync_captured_();
                } else {
                    status_ = "Nothing to redo";
                }
                return true;
            }
            return false;

        case Phase::TEST:
            if (ev.action == InputAction::SELECT && ev.pressed) {
                // An all-skipped session is COMPLETE, so it lands here, but it
                // captured nothing. Refuse it: see can_save() for why an empty
                // profile is worse than no profile. The renderer already drops
                // "Select: save" from the footer in that state, so this branch
                // only catches a press the chrome never invited.
                if (!save_profile_()) {
                    status_ = "Nothing captured - redo or cancel";
                    return true;
                }
                phase_ = Phase::DONE;
                Toast::show("Controller saved: " + device_name_);
                return true;
            }
            if (ev.action == InputAction::PREV && ev.pressed) {         // BTN1
                start_capture_();   // start over, same pad and style
                return true;
            }
            return false;

        case Phase::DONE:
            if (ev.action == InputAction::SELECT && ev.pressed) {
                close();
                return true;
            }
            return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Per-frame tick: idle timeout + target-pad-unplugged watchdog
// ---------------------------------------------------------------------------

bool ControllerWizard::tick() {
    if (!active_) return true;   // nothing to time out
    const auto now = std::chrono::steady_clock::now();

    if (now - last_input_ > kIdleTimeout) {
        close();
        return false;
    }

    // Only meaningful once a target exists AND the flow depends on it. In
    // PICK_DEVICE nothing is chosen yet; in DONE the profile is already on
    // disk and unplugging must not blow the confirmation away.
    if ((phase_ == Phase::CAPTURE || phase_ == Phase::TEST) &&
        now - last_caps_poll_ >= kCapsPollPeriod) {
        last_caps_poll_ = now;
        // InputManager drops joystick nodes that report -ENODEV, so a missing
        // caps snapshot is exactly "that pad is gone".
        if (input_ && !input_->device_caps(vid_, pid_)) {
            Toast::show("Controller disconnected - setup cancelled");
            close();
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Readouts
// ---------------------------------------------------------------------------

retroarch::ControllerStyle ControllerWizard::style() const {
    return style_cursor_ == 1 ? retroarch::ControllerStyle::N64_STYLE
                              : retroarch::ControllerStyle::PS_STYLE;
}

std::string ControllerWizard::prompt() const {
    if (!session_ || session_->done()) return "";
    return retroarch::control_prompt(session_->current_control());
}

size_t ControllerWizard::step_index() const {
    return session_ ? session_->step_index() : 0;
}

size_t ControllerWizard::step_count() const {
    return session_ ? session_->step_count()
                    : retroarch::capture_steps(style()).size();
}

std::string ControllerWizard::status_line() const { return status_; }

const std::map<retroarch::LogicalControl, bool>& ControllerWizard::test_lit() const {
    return test_lit_;
}

const std::map<retroarch::LogicalControl, retroarch::PhysicalBinding>&
ControllerWizard::captured() const {
    return captured_;
}

// ---------------------------------------------------------------------------
// Internals
// ---------------------------------------------------------------------------

void ControllerWizard::start_capture_() {
    if (!caps_) return;   // unreachable: PICK_STYLE is only entered with caps
    session_ = std::make_unique<retroarch::CaptureSession>(style(), *caps_);
    captured_.clear();
    test_lit_.clear();
    status_.clear();
    phase_ = Phase::CAPTURE;
}

void ControllerWizard::sync_captured_() {
    if (session_) captured_ = session_->results();
}

void ControllerWizard::finish_capture_() {
    sync_captured_();
    test_lit_.clear();
    for (const auto& [control, binding] : captured_) {
        (void)binding;
        test_lit_[control] = false;
    }
    phase_ = Phase::TEST;
}

std::optional<bool> ControllerWizard::binding_lit_(
    const retroarch::PhysicalBinding& b, const platform::RawInputEvent& ev) const {
    using Kind = retroarch::PhysicalBinding::Kind;

    if (ev.type == kEvKey) {
        if (b.kind != Kind::BUTTON || b.code != ev.code) return std::nullopt;
        // Direction-insensitive: a button has none. value 2 is autorepeat.
        return ev.value != 0;
    }
    if (ev.type != kEvAbs || b.code != ev.code) return std::nullopt;

    const bool is_hat = ev.code >= kHatFirst && ev.code <= kHatLast;
    if (is_hat) {
        if (b.kind != Kind::HAT) return std::nullopt;
        const int dir = ev.value == 0 ? 0 : (ev.value < 0 ? -1 : +1);
        return dir == b.direction;
    }

    if (b.kind != Kind::AXIS || !caps_) return std::nullopt;
    const retroarch::CaptureDeviceCaps::AxisRange* ax = nullptr;
    for (const auto& a : caps_->axes) {
        if (a.code == ev.code) { ax = &a; break; }
    }
    if (!ax) return std::nullopt;
    // Same "half the room actually available on this side" rule the capture
    // state machine uses to arm an axis, so a stick that captured cleanly
    // also lights cleanly.
    const int avail_pos = std::max(1, ax->max - ax->rest);
    const int avail_neg = std::max(1, ax->rest - ax->min);
    const int delta = ev.value - ax->rest;
    int dir = 0;
    if (delta > 0 && delta > avail_pos / 2) dir = +1;
    else if (delta < 0 && -delta > avail_neg / 2) dir = -1;
    return dir == b.direction;
}

bool ControllerWizard::save_profile_() {
    // Last line of defence, independent of what the footer offers: never let
    // an empty profile reach the store, where it would shadow this pad's
    // builtin profile for every future bind resolution.
    if (!can_save()) return false;

    retroarch::PhysicalProfile profile;
    profile.name = device_name_;
    profile.style = style();
    profile.vid = vid_;
    profile.pid = pid_;
    profile.captured_at = "";
    profile.controls = captured_;

    // Read-modify-write the WHOLE store: other pads' profiles live in the
    // same file and must survive this save. The map key is authoritative
    // (see controller_profile.h), so key by the canonical vidpid_key.
    auto store = retroarch::load_profile_store();
    store[retroarch::vidpid_key(vid_, pid_)] = profile;
    retroarch::save_profile_store(store);
    return true;
}

}  // namespace ui
