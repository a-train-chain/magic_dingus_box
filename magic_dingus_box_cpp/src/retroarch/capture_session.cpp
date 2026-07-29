#include "capture_session.h"

#include <algorithm>
#include <cstdlib>

#include "joydev_index.h"

namespace retroarch {

namespace {
constexpr uint16_t kEvKey = 0x01;
constexpr uint16_t kEvAbs = 0x03;
constexpr uint16_t kHatFirst = 0x10, kHatLast = 0x17;
}  // namespace

CaptureSession::CaptureSession(ControllerStyle style, CaptureDeviceCaps caps)
    : caps_(std::move(caps)), steps_(capture_steps(style)) {}

bool CaptureSession::done() const { return index_ >= steps_.size(); }
LogicalControl CaptureSession::current_control() const { return steps_[index_]; }
size_t CaptureSession::step_index() const { return index_; }
size_t CaptureSession::step_count() const { return steps_.size(); }
LogicalControl CaptureSession::last_duplicate_of() const { return duplicate_of_; }
std::map<LogicalControl, PhysicalBinding> CaptureSession::results() const { return captured_; }

void CaptureSession::reset_transient() {
    pressed_code_ = -1;
    armed_abs_code_ = -1;
    armed_direction_ = 0;
}

void CaptureSession::skip() {
    if (done()) return;
    reset_transient();
    ++index_;
}

bool CaptureSession::redo_last() {
    if (index_ == 0) return false;
    --index_;
    captured_.erase(steps_[index_]);
    reset_transient();
    return true;
}

CaptureSession::FeedResult CaptureSession::feed(uint16_t ev_type, uint16_t code,
                                                int32_t value) {
    if (done()) return FeedResult::NONE;

    PhysicalBinding pending;
    bool complete = false;

    if (ev_type == kEvKey) {
        if (value == 1) { pressed_code_ = code; return FeedResult::NONE; }
        if (value == 0 && pressed_code_ == static_cast<int>(code)) {
            pending = {PhysicalBinding::Kind::BUTTON, code, 0, ""};
            complete = true;
        }
    } else if (ev_type == kEvAbs) {
        if (code >= kHatFirst && code <= kHatLast) {
            if (value != 0) { armed_abs_code_ = code; armed_direction_ = value < 0 ? -1 : +1; return FeedResult::NONE; }
            if (armed_abs_code_ == static_cast<int>(code)) {
                pending = {PhysicalBinding::Kind::HAT, code, armed_direction_, ""};
                complete = true;
            }
        } else {
            const CaptureDeviceCaps::AxisRange* ax = nullptr;
            for (const auto& a : caps_.axes) if (a.code == code) { ax = &a; break; }
            if (!ax) return FeedResult::NONE;
            // Threshold is directional: use the room actually available
            // between rest and the extreme being moved toward, not a fixed
            // half-range. A pad resting off-center (e.g. a worn 8-bit stick
            // sitting at 200 of 0..255) has far less travel toward one
            // extreme than the other; a fixed half-range threshold can
            // exceed the room available in that direction entirely, making
            // it un-armable no matter how hard the user pushes.
            // std::max(1, ...) guards a degenerate axis (rest pinned at an
            // extreme, or min == max with no travel at all): it keeps both
            // thresholds computable without a zero/negative range, and the
            // capture threshold below is separately floored so a direction
            // that does arm always has *some* reachable return-to-rest
            // window (a raw quarter of a 1-3 unit range would floor to 0,
            // which no delta is ever "less than").
            const int avail_pos = std::max(1, ax->max - ax->rest);
            const int avail_neg = std::max(1, ax->rest - ax->min);
            const int delta = value - ax->rest;
            if (delta > 0 && delta > avail_pos / 2) {    // >50% of room toward max arms +1
                armed_abs_code_ = code;
                armed_direction_ = +1;
                return FeedResult::NONE;
            }
            if (delta < 0 && -delta > avail_neg / 2) {   // >50% of room toward min arms -1
                armed_abs_code_ = code;
                armed_direction_ = -1;
                return FeedResult::NONE;
            }
            if (armed_abs_code_ == static_cast<int>(code)) {
                const int avail = armed_direction_ > 0 ? avail_pos : avail_neg;
                const int capture_threshold = std::max(1, avail / 4);
                if (std::abs(delta) < capture_threshold) {  // back near rest, that side's room
                    pending = {PhysicalBinding::Kind::AXIS, code, armed_direction_, ""};
                    complete = true;
                }
            }
        }
    } else {
        return FeedResult::NONE;
    }

    if (!complete) return FeedResult::NONE;
    reset_transient();  // gesture completed: clear ALL transient state, not
                         // just the field owned by this gesture's kind --
                         // otherwise a stray press of a different kind left
                         // over from this step can bleed into the next one.

    for (const auto& [control, b] : captured_) {
        if (b.kind == pending.kind && b.code == pending.code &&
            b.direction == pending.direction) {
            duplicate_of_ = control;
            return FeedResult::DUPLICATE;
        }
    }

    std::vector<uint16_t> abs_codes;
    for (const auto& a : caps_.axes) abs_codes.push_back(a.code);
    pending.token = bind_token(caps_.key_codes, abs_codes, pending.kind,
                               pending.code, pending.direction);
    captured_[steps_[index_]] = pending;
    ++index_;
    return done() ? FeedResult::DONE : FeedResult::CAPTURED;
}

}  // namespace retroarch
