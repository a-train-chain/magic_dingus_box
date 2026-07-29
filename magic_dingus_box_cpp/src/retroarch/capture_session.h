#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "controller_profile.h"
#include "logical_controls.h"

namespace retroarch {

// Everything the session needs to know about the target device, gathered
// once by InputManager when the wizard picks it (Task 9).
struct CaptureDeviceCaps {
    uint16_t vid = 0, pid = 0;
    std::string name;
    std::vector<uint16_t> key_codes;                  // ascending EV_KEY codes
    struct AxisRange { uint16_t code; int min, max, rest; };
    std::vector<AxisRange> axes;                      // ascending ABS codes (incl. hats)
};

// Pure state machine driving the controller-setup wizard: walks
// capture_steps(style) one LogicalControl at a time, deciding when a raw
// evdev-shaped event (ev_type/code/value) counts as a capture for the
// current step. No I/O, no device access -- Task 9 feeds it real evdev
// events, Task 10 renders its prompts and persists results() via the
// existing profile store.
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
    ControllerStyle style_;
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
