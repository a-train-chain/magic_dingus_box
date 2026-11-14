#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <set>

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

// Timing cancellation keeps ownership of still-live sources until their own
// release. Device loss retires only that source, without inventing a Tap.
class GuideSelectInput {
public:
    using Action = GuideSelectHold::Action;
    using Clock = GuideSelectHold::Clock;

    void press(uint64_t source, Clock::time_point now) {
        if (owns(source)) return;  // repeat or canceled press awaiting release
        cancel();
        owned_.insert(source);
        active_ = source;
        hold_.press(now);
    }
    bool owns(uint64_t source) const { return owned_.count(source) != 0; }
    Action release(uint64_t source, Clock::time_point now) {
        if (!owned_.erase(source) || active_ != source) return Action::None;
        active_.reset();
        return hold_.release(now);
    }
    void source_lost(uint64_t source) {
        owned_.erase(source);
        if (active_ == source) cancel();
    }
    void cancel() { hold_.cancel(); active_.reset(); }
    bool pending() const { return hold_.pending(); }
    Action tick(Clock::time_point now) { return hold_.tick(now); }

private:
    GuideSelectHold hold_;
    std::optional<uint64_t> active_;
    std::set<uint64_t> owned_;
};

}  // namespace app
