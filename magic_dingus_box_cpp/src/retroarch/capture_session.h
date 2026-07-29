#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "capture_device_caps.h"
#include "controller_profile.h"
#include "logical_controls.h"

namespace retroarch {

// CaptureDeviceCaps -- what InputManager::device_caps() gathers about the
// target device -- lives in capture_device_caps.h so platform/ can include
// it without dragging in (or cycling through) this header. See the note at
// the top of that file.

// Pure state machine driving the controller-setup wizard: walks
// capture_steps(style) one LogicalControl at a time, deciding when a raw
// evdev-shaped event (ev_type/code/value) counts as a capture for the
// current step. No I/O, no device access: ui::ControllerWizard feeds it the
// events InputManager's raw-capture queue produces, renders its prompts, and
// persists results() through the profile store.
class CaptureSession {
public:
    CaptureSession(ControllerStyle style, CaptureDeviceCaps caps);

    enum class FeedResult { NONE, CAPTURED, DUPLICATE, DONE };
    // ev_type is EV_KEY (0x01) or EV_ABS (0x03); anything else -> NONE.
    FeedResult feed(uint16_t ev_type, uint16_t code, int32_t value);

    void skip();            // current control stays unbound, advance
    bool redo_last();       // step back one (false at the first step)
    bool done() const;
    LogicalControl current_control() const;   // undefined when done()
    size_t step_index() const;
    size_t step_count() const;
    LogicalControl last_duplicate_of() const; // valid after DUPLICATE
    // Only meaningful when done(): captured bindings with tokens filled in.
    std::map<LogicalControl, PhysicalBinding> results() const;

private:
    // Clears all per-step transient state. Used by skip(), redo_last(), and
    // feed() on every successful capture -- a gesture of one kind (e.g. a
    // held button) must not leak into the next step just because a
    // different kind (e.g. a hat) is what actually completed. Centralized
    // here so the three call sites cannot drift out of sync again.
    void reset_transient();

    // NB: no style_ member. The ctor uses its `style` PARAMETER to build
    // steps_, and nothing afterwards needs the style again -- steps_ IS the
    // style, resolved. A stored copy was an unread field (-Wunused-private-field).
    CaptureDeviceCaps caps_;
    std::vector<LogicalControl> steps_;
    size_t index_ = 0;
    std::map<LogicalControl, PhysicalBinding> captured_;
    LogicalControl duplicate_of_{};
    // per-step transient state
    int pressed_code_ = -1;          // button awaiting release
    int armed_abs_code_ = -1;        // axis/hat deflected, awaiting return
    int armed_direction_ = 0;
};

}  // namespace retroarch
