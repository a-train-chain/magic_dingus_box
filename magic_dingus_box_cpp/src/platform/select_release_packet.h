#pragma once

#include "input_manager.h"
#include <array>

namespace platform {

// Mapped evdev actions awaiting their packet's SYN_REPORT. Kept per device
// so a partial read/EAGAIN never dispatches an unclassified Select release.
class SelectReleasePacket {
public:
    bool append(const InputEvent& event, std::vector<InputEvent>& output) {
        // Bound memory even if a broken source never sends SYN_REPORT. Once
        // full, stream ordinary actions and abandon Select until the next SYN.
        bool just_overflowed = false;
        if (!overflowed_ && size_ == events_.size()) {
            for (std::size_t i = 0; i < size_; ++i)
                if (events_[i].action != InputAction::SELECT)
                    output.push_back(events_[i]);
            size_ = 0;
            overflowed_ = just_overflowed = true;
        }
        if (overflowed_) {
            if (event.action != InputAction::SELECT) output.push_back(event);
        } else {
            events_[size_++] = event;
        }
        return just_overflowed;
    }
    bool finish(int syn_value, std::vector<InputEvent>& output) {
        // Linux teardown/reset synthesizes key ups followed by SYN_REPORT=1
        // before evdev necessarily reports ENODEV. SYN values are not a public
        // detach API: conservatively interrupt only an ambiguous Select UP.
        // Repeat-only SYN1 packets must keep a held Select alive.
        bool interrupted = overflowed_;
        if (syn_value != 0) {
            for (std::size_t i = 0; i < size_; ++i) {
                const auto& event = events_[i];
                if (event.action == InputAction::SELECT && !event.pressed)
                    interrupted = true;
            }
        }
        for (std::size_t i = 0; i < size_; ++i) {
            const auto& event = events_[i];
            if (!interrupted || event.action != InputAction::SELECT)
                output.push_back(event);
        }
        discard();
        return interrupted;
    }
    void discard() { size_ = 0; overflowed_ = false; }

private:
    std::array<InputEvent, 256> events_;
    std::size_t size_ = 0;
    bool overflowed_ = false;
};

} // namespace platform
