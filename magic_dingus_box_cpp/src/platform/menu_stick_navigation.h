#pragma once

#include <cstdint>

namespace platform {

// One menu-navigation axis. Times are monotonic milliseconds. update() emits
// direction transitions only; poll() supplies repeats without fresh reports.
class MenuStickNavigation {
public:
    int update(int32_t minimum, int32_t maximum, int32_t value, int64_t now_ms) {
        if (maximum <= minimum) {
            reset();
            return 0;
        }
        // Widen before subtraction: even full int32 evdev ranges are safe.
        const double centre = (static_cast<int64_t>(minimum) + maximum) / 2.0;
        const double deflection = static_cast<double>(value) - centre;
        const double half = deflection >= 0 ? maximum - centre : centre - minimum;
        const double normalized = deflection / half;
        int next = 0;
        if (normalized >= 0.50) next = 1;
        else if (normalized <= -0.50) next = -1;
        else if (direction_ == 1 && normalized > 0.35) next = 1;
        else if (direction_ == -1 && normalized < -0.35) next = -1;

        if (next == direction_) return 0;
        direction_ = next;
        press_ms_ = last_fire_ms_ = now_ms;
        return next;
    }

    int poll(int64_t now_ms) {
        const int64_t elapsed = now_ms - press_ms_;
        if (!direction_ || elapsed < 400) return 0;
        // Match the physical D-pad: 8 Hz after the initial delay, then
        // 20 Hz after another second. Never catch up with a burst of steps.
        const int64_t interval = elapsed >= 1400 ? 50 : 125;
        if (now_ms - last_fire_ms_ < interval) return 0;
        last_fire_ms_ = now_ms;
        return direction_;
    }

    void reset() { direction_ = 0; press_ms_ = last_fire_ms_ = 0; }

private:
    int direction_ = 0;
    int64_t press_ms_ = 0;
    int64_t last_fire_ms_ = 0;
};

} // namespace platform
