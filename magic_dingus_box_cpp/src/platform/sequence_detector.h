#pragma once

#include <cstdint>
#include <chrono>

namespace platform {

// Raw input events the detector cares about. Fed from gpio_manager.
enum class SeqInput : uint8_t {
    NONE,
    BTN1_PRESS,
    BTN2_PRESS,
    BTN3_PRESS,
    BTN4_PRESS,
    BTN1_BTN3_CHORD,   // both pressed within ~50ms window
    ROTARY_CLICK,      // distinct from controller SELECT
};

// Result of feeding an event into the detector.
enum class SeqResult : uint8_t {
    NO_MATCH,          // Event doesn't advance sequence; state was reset
    PROGRESS,          // Event advanced the sequence; not yet complete
    UNLOCKED,          // Sequence completed this event
};

// State machine for the Media Browser unlock sequence:
//   BTN1+BTN3 (chord), BTN2, BTN2, BTN2, RCLICK
//
// Thread-safety: not thread-safe. Call feed() from a single thread.
class SequenceDetector {
public:
    // Events must arrive within this window of the previous event, else
    // the sequence resets silently.
    static constexpr int TIMEOUT_MS = 2000;

    SequenceDetector();

    // Feed a raw input event. Returns whether this advanced or completed
    // the sequence. Timing is checked against the last event time.
    SeqResult feed(SeqInput input,
                   std::chrono::steady_clock::time_point now);

    // Reset state (e.g. on screen change). Also called internally on timeout.
    void reset();

    // How many events of the 5-step sequence have been matched so far.
    int progress() const { return step_; }

private:
    int step_ = 0;
    std::chrono::steady_clock::time_point last_event_time_;
};

}  // namespace platform
