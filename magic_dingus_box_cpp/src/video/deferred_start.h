#pragma once

#include <optional>

namespace video {

// A load_file(start > 0) seek, held until the pipeline has prerolled.
//
// A seek issued before preroll can be dropped by some demuxers, so the start
// offset has to wait for it — but waiting with a blocking get_state() froze
// the render thread for up to 3 s on every trimmed playlist item and every
// Media Browser resume. GstPlayer arms this in load_file() and drains it from
// the per-frame state poll instead. Pure logic so it can be tested off-Pi.
struct DeferredStart {
    // start <= 0 disarms: a load from the beginning has nothing to defer.
    void arm(double start_seconds) {
        target_ = start_seconds > 0.0 ? std::optional<double>(start_seconds)
                                      : std::nullopt;
    }
    // stop(), a new load, or an explicit user seek — all supersede it.
    void cancel() { target_.reset(); }
    bool armed() const { return target_.has_value(); }

    // The target exactly once, on the first poll that sees the pipeline
    // prerolled (PAUSED or PLAYING). Later polls return nothing.
    std::optional<double> take_if_prerolled(bool prerolled) {
        if (!prerolled || !target_) return std::nullopt;
        auto t = target_;
        target_.reset();
        return t;
    }

    // While armed, the position callers should see. The blocking version
    // returned from load_file() already at `start`; keeping that contract
    // stops an early read (e.g. a watch checkpoint) from recording 0.
    std::optional<double> position_override() const { return target_; }

private:
    std::optional<double> target_;
};

}  // namespace video
