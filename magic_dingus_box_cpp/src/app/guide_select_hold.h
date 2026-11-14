#pragma once

#include <chrono>
#include <optional>

namespace app {

inline constexpr std::chrono::milliseconds kGuideSelectHoldThreshold{500};

// One Select gesture, timed by the caller's monotonic clock. Keep the
// gesture pending after a Hold so repeats and its release cannot become Tap.
class GuideSelectHold {
public:
    using Clock = std::chrono::steady_clock;
    enum class Action { None, Tap, Hold };

    void press(Clock::time_point now) {
        if (started_) return;
        started_ = now;
        held_ = false;
    }
    Action release(Clock::time_point now) {
        if (!started_) return Action::None;
        const Action action = held_ ? Action::None
            : now - *started_ >= kGuideSelectHoldThreshold ? Action::Hold : Action::Tap;
        cancel();
        return action;
    }
    Action tick(Clock::time_point now) {
        if (!started_ || held_ || now - *started_ < kGuideSelectHoldThreshold)
            return Action::None;
        held_ = true;
        return Action::Hold;
    }
    void cancel() { started_.reset(); held_ = false; }
    bool pending() const { return started_.has_value(); }

private:
    std::optional<Clock::time_point> started_;
    bool held_ = false;
};

}  // namespace app
