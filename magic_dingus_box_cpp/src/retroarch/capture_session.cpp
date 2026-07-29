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
    : style_(style), caps_(std::move(caps)), steps_(capture_steps(style)) {}

bool CaptureSession::done() const { return index_ >= steps_.size(); }
LogicalControl CaptureSession::current_control() const { return steps_[index_]; }
size_t CaptureSession::step_index() const { return index_; }
size_t CaptureSession::step_count() const { return steps_.size(); }
LogicalControl CaptureSession::last_duplicate_of() const { return duplicate_of_; }
std::map<LogicalControl, PhysicalBinding> CaptureSession::results() const { return captured_; }

void CaptureSession::skip() {
    if (done()) return;
    pressed_code_ = -1; armed_abs_code_ = -1; armed_direction_ = 0;
    ++index_;
}

bool CaptureSession::redo_last() {
    if (index_ == 0) return false;
    --index_;
    captured_.erase(steps_[index_]);
    pressed_code_ = -1; armed_abs_code_ = -1; armed_direction_ = 0;
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
            pressed_code_ = -1;
        }
    } else if (ev_type == kEvAbs) {
        if (code >= kHatFirst && code <= kHatLast) {
            if (value != 0) { armed_abs_code_ = code; armed_direction_ = value < 0 ? -1 : +1; return FeedResult::NONE; }
            if (armed_abs_code_ == static_cast<int>(code)) {
                pending = {PhysicalBinding::Kind::HAT, code, armed_direction_, ""};
                complete = true;
                armed_abs_code_ = -1;
            }
        } else {
            const CaptureDeviceCaps::AxisRange* ax = nullptr;
            for (const auto& a : caps_.axes) if (a.code == code) { ax = &a; break; }
            if (!ax) return FeedResult::NONE;
            const int half = std::max(1, (ax->max - ax->min) / 2);
            const int delta = value - ax->rest;
            if (std::abs(delta) > half / 2) {           // >50% deflection arms
                armed_abs_code_ = code;
                armed_direction_ = delta < 0 ? -1 : +1;
                return FeedResult::NONE;
            }
            if (armed_abs_code_ == static_cast<int>(code) &&
                std::abs(delta) < half / 4) {           // <25% of half-range = back at rest
                pending = {PhysicalBinding::Kind::AXIS, code, armed_direction_, ""};
                complete = true;
                armed_abs_code_ = -1;
            }
        }
    } else {
        return FeedResult::NONE;
    }

    if (!complete) return FeedResult::NONE;

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
