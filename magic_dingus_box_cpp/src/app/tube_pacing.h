// app/tube_pacing.h - counts late Tube presents and decides when a look is too heavy
// for the box. Pure: time arrives as a parameter, nothing is read or written here.
//
// A present is LATE when the interval since the previous one exceeds the class's
// cadence (cadence_vblanks: the 60 fps frame cap's refreshes for Lean60 - one at 50/60
// Hz, two at 120 Hz - and utils::held_vblanks(hz) for Rich30) plus half a refresh. The first present ever, and a present after a gap over
// 250 ms (a menu, a pause, a mode change), says nothing about pacing: the first
// ever counts as a present that is not late; one after a gap counts nothing.
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

#include "tube/tube_frame_class.h"
#include "utils/frame_pacing.h"

namespace app {

class TubePacingMonitor {
public:
    using Clock = std::chrono::steady_clock;

    struct Report {
        std::uint64_t presents = 0, late = 0, video_presents = 0, video_late = 0;
        tube::FrameClass cls = tube::FrameClass::Lean60;
    };

    // Refreshes between two on-time presents of a class at `vrefresh_hz` (<= 0 reads as
    // 60). Rich30: the held flip's utils::held_vblanks (2 at 50/60 Hz, 4 at 120 Hz).
    // Lean60: the main loop's own 60 fps cap, utils::frame_pacing_for(60, hz) - one
    // refresh at 50/60 Hz but TWO at 120 Hz, where counting one made every lean present
    // late (and every lean look "too heavy" after 30 s of video). The tube never draws
    // while the Media Browser plays a movie (the loop's 30 fps cap), so 60 is the cap.
    static int cadence_vblanks(tube::FrameClass cls, int vrefresh_hz) {
        return cls == tube::FrameClass::Rich30 ? utils::held_vblanks(vrefresh_hz)
                                               : utils::frame_pacing_for(60, vrefresh_hz).vblanks_per_frame;
    }

    void on_present(Clock::time_point done, tube::FrameClass cls, int vrefresh_hz, bool video_playing) {
        using namespace std::chrono;
        report_.cls = cls;
        const bool had_previous = last_present_.has_value();
        const auto previous = last_present_.value_or(done);
        last_present_ = done;
        const auto interval = duration_cast<microseconds>(done - previous);
        if (had_previous && (interval > milliseconds(250) || interval < microseconds(0))) {
            video_window_.clear();  // the streak is broken
            video_late_ = 0;
            streak_start_.reset();
            return;
        }
        bool late = false;
        if (had_previous) {
            if (vrefresh_hz <= 0) vrefresh_hz = 60;
            const microseconds period{1000000 / vrefresh_hz};
            late = interval > period * cadence_vblanks(cls, vrefresh_hz) + period / 2;
        }
        ++report_.presents;
        if (late) ++report_.late;
        if (!video_playing) {
            video_window_.clear();
            video_late_ = 0;
            streak_start_.reset();
            return;
        }
        ++report_.video_presents;
        if (late) ++report_.video_late;
        if (!streak_start_) streak_start_ = done;
        video_window_.emplace_back(done, late);
        if (late) ++video_late_;
        while (!video_window_.empty() && done - video_window_.front().first > kWindow) {
            if (video_window_.front().second) --video_late_;
            video_window_.pop_front();
        }
        if (!heavy_ && window_too_heavy()) heavy_ = toast_pending_ = true;
    }

    // Every 60 s; the first call only starts the window.
    std::optional<Report> take_report(Clock::time_point now) {
        if (!window_start_) {  // the first call starts the window: earlier presents are dropped
            window_start_ = now;
            const auto cls = report_.cls;
            report_ = Report{};
            report_.cls = cls;
            return std::nullopt;
        }
        if (now - *window_start_ < std::chrono::seconds(60)) return std::nullopt;
        Report out = report_;
        report_ = Report{};
        report_.cls = out.cls;
        window_start_ = now;
        return out;
    }

    // Latched: once the last 30 s of video presents (spanning at least 30 s) were more
    // than 5% late, stays true until reset() - the status says so "until the setting or
    // the frame class changes", however the window moves on.
    bool too_heavy() const { return heavy_; }

private:
    bool window_too_heavy() const {
        if (!streak_start_ || video_window_.empty()) return false;
        if (video_window_.back().first - *streak_start_ < kWindow) return false;
        return video_late_ * 20 > video_window_.size();
    }

public:
    // True once per look/class: reset() re-arms it.
    bool take_toast() { return std::exchange(toast_pending_, false); }

    // A setting or frame-class change: the old evidence is about a different look,
    // so the window restarts and the toast may be shown again for the new one.
    void reset() {
        video_window_.clear();
        video_late_ = 0;
        streak_start_.reset();
        toast_pending_ = heavy_ = false;
        last_present_.reset();
    }

private:
    static constexpr std::chrono::seconds kWindow{30};
    Report report_;
    std::optional<Clock::time_point> last_present_, window_start_, streak_start_;
    std::deque<std::pair<Clock::time_point, bool>> video_window_;
    std::size_t video_late_ = 0;
    bool toast_pending_ = false, heavy_ = false;
};

}  // namespace app
