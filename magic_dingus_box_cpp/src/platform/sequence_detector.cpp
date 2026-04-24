#include "platform/sequence_detector.h"

namespace platform {

namespace {
// The target sequence: BTN1+BTN3 chord, BTN2, BTN2, BTN2, RCLICK.
// Indexed by step (0..4).
constexpr SeqInput EXPECTED[] = {
    SeqInput::BTN1_BTN3_CHORD,
    SeqInput::BTN2_PRESS,
    SeqInput::BTN2_PRESS,
    SeqInput::BTN2_PRESS,
    SeqInput::ROTARY_CLICK,
};
constexpr int SEQ_LEN = sizeof(EXPECTED) / sizeof(EXPECTED[0]);
}  // namespace

SequenceDetector::SequenceDetector() = default;

SeqResult SequenceDetector::feed(SeqInput input,
                                 std::chrono::steady_clock::time_point now) {
    // Timeout check (only after the first event)
    if (step_ > 0) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_event_time_).count();
        if (elapsed > TIMEOUT_MS) {
            step_ = 0;
        }
    }

    if (input == EXPECTED[step_]) {
        step_++;
        last_event_time_ = now;
        if (step_ >= SEQ_LEN) {
            step_ = 0;  // auto-reset for next time
            return SeqResult::UNLOCKED;
        }
        return SeqResult::PROGRESS;
    }

    // Wrong input: silent reset
    step_ = 0;
    return SeqResult::NO_MATCH;
}

void SequenceDetector::reset() {
    step_ = 0;
}

}  // namespace platform
