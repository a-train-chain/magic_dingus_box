// app/tube_pacing.h - counts late Tube presents and decides when a look is too heavy
// for the box. Pure: time arrives as a parameter, nothing is read or written here.
//
// A present is LATE when the interval since the previous one exceeds the class's
// cadence (cadence_vblanks: the 60 fps frame cap's refreshes for Lean60 - one at 50/60
// Hz, two at 120 Hz - and utils::held_vblanks(hz) for Rich30) plus half a refresh. The first present ever, and a present after a gap over
// 250 ms (a menu, a pause, a mode change), says nothing about pacing: the first
// ever counts as a present that is not late; one after a gap counts nothing.
//
// TOO HEAVY (latched until reset()) needs video playing throughout - a gap over 250 ms,
// a negative interval, a stall (below) or a present without video breaks the streak and
// clears all evidence - and, on the streak's full minutes (60 s buckets counted from its
// start, a minute closed by the first present of the next), either:
//  (a) FAST: each of the last 3 consecutive minutes had MORE THAN HALF of its presents
//      late; a minute at or under half resets this count; or
//  (b) SUSTAINED: each of the last 10 consecutive minutes had more than 5% of its
//      presents late; a minute at or under 5% resets this count (and (a)'s).
// Three generations of the rule, each replaced by what a Pi 5 at 1080p measured
// (2026-11-16):
//  1. "5% late over 30 s" fired on a busy box: a SHIPPED look ran 12-13% late for two
//     minutes while the Media Browser's containers started again after a game, 20% for
//     half a minute after a game ended, against 0.3-0.8% in quiet minutes.
//  2. "half late over 30 s" fired while containers restarted - PVM at 40% then 20% for
//     a minute each, and even Monitor, the LIGHTEST look (17 of 23 ms), at 38% for one
//     minute of unexplained box load (Docker logged an exec error on one container;
//     nothing restarted), with no flip over 100 ms to call a stall: more than half its
//     frames were late for some 30 s inside that minute.
//  3. Hence persistence: no measured busy-box minute reached half late and no burst
//     lasted over three minutes, while a look that really is too heavy is late on
//     nearly every frame, every minute it is shown (77%: the TV look on a full-screen
//     film at 20 fps). (a) catches that in three minutes, (b) a look that is only
//     somewhat over budget, all the time.
// A STALL - an interval over 100 ms (and at most 250 ms) - counts in the report as a
// late present but clears the streak, and the next video present starts a new one: on
// the same Pi 5 (2026-11-16) the PVM look (19 of 23 ms) latched rule 2 while the Media
// Browser's containers restarted - page flips waited 48-514 ms, 475 of 1173 presents
// late in one minute. A look that is really too heavy presents every 50 ms (20 fps), at
// worst 66-83 ms (the heaviest measured, Everything on a film, is 33 ms of GPU); an
// interval over 100 ms is the flip itself waiting, not the look's drawing.
#pragma once

#include <chrono>
#include <cstdint>
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
        bool first = false;   // the first report since construction or reset()
    };

    // The journal level for a report, as main.cpp's UI draw-call line: INFO for the first
    // report after the tube starts drawing or its look/class changes (both reset()), and
    // for any report with a late present while video played (the evidence the too-heavy
    // toast judges); DEBUG otherwise, so a still menu - which keeps presenting through
    // the redraw gate's safety redraw - is not one INFO line a minute forever.
    static bool report_at_info(const Report& r) { return r.first || r.video_late > 0; }

    // Refreshes between two on-time presents of a class at `vrefresh_hz` (<= 0 reads as
    // 60). Rich30: the held flip's utils::held_vblanks (2 at 50/60 Hz, 4 at 120 Hz).
    // Lean60: the main loop's own 60 fps cap, utils::frame_pacing_for(60, hz) - one
    // refresh at 50/60 Hz but TWO at 120 Hz, where counting one made every lean present
    // late (and every lean look "too heavy" after 30 s of video under the first rule). The tube never draws
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
            clear_streak();  // the streak is broken
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
        const bool stall = had_previous && interval > kStall;
        if (!video_playing || stall) {
            if (video_playing) {  // a stall: in the report, but evidence about the box, not the look
                ++report_.video_presents;
                ++report_.video_late;
            }
            clear_streak();
            return;
        }
        ++report_.video_presents;
        if (late) ++report_.video_late;
        if (!streak_start_) streak_start_ = done;
        // A present in a later minute closes the current one. (No minute of a streak is
        // ever empty - a 250 ms gap breaks it - but one would count as quiet.)
        const auto minute = (done - *streak_start_) / kMinute;
        if (minute != minute_) {
            const bool next = minute == minute_ + 1;
            const bool half = next && minute_late_ * 2 > minute_presents_;      // rule (a)
            const bool some = next && minute_late_ * 20 > minute_presents_;     // rule (b)
            fast_minutes_ = half ? fast_minutes_ + 1 : 0;
            sustained_minutes_ = some ? sustained_minutes_ + 1 : 0;
            minute_ = minute;
            minute_presents_ = minute_late_ = 0;
        }
        ++minute_presents_;
        if (late) ++minute_late_;
        if (!heavy_ && (fast_minutes_ >= kFastMinutes || sustained_minutes_ >= kSustainedMinutes))
            heavy_ = toast_pending_ = true;
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
        out.first = std::exchange(first_report_pending_, false);
        report_ = Report{};
        report_.cls = out.cls;
        window_start_ = now;
        return out;
    }

    // Latched: once rule (a) or (b) above holds, stays true until reset() - the status
    // says so "until the setting or the frame class changes", however the window moves on.
    bool too_heavy() const { return heavy_; }

private:
    void clear_streak() {
        streak_start_.reset();
        minute_ = 0;
        minute_presents_ = minute_late_ = 0;
        fast_minutes_ = sustained_minutes_ = 0;
    }

public:
    // True once per look/class: reset() re-arms it.
    bool take_toast() { return std::exchange(toast_pending_, false); }

    // A setting or frame-class change: the old evidence is about a different look,
    // so the window restarts and the toast may be shown again for the new one.
    void reset() {
        clear_streak();
        toast_pending_ = heavy_ = false;
        last_present_.reset();
        first_report_pending_ = true;
    }

private:
    static constexpr std::chrono::milliseconds kStall{100};  // longer: the box stalled, not the look
    static constexpr std::chrono::seconds kMinute{60};      // the rules' buckets
    static constexpr int kFastMinutes = 3;                  // rule (a): more than half late in each
    static constexpr int kSustainedMinutes = 10;            // rule (b): more than 5% late in each
    Report report_;
    std::optional<Clock::time_point> last_present_, window_start_, streak_start_;
    std::int64_t minute_ = 0;            // the streak's current minute
    std::size_t minute_presents_ = 0, minute_late_ = 0;
    int fast_minutes_ = 0;               // consecutive full minutes more than half late
    int sustained_minutes_ = 0;          // consecutive full minutes more than 5% late
    bool toast_pending_ = false, heavy_ = false, first_report_pending_ = true;
};

}  // namespace app
