#include "auto_advance.h"

namespace app {

double effective_end(double duration, double item_end) {
    if (duration > 0.0 && item_end > 0.0 && item_end < duration) {
        return item_end;
    }
    return duration;
}

AutoAdvance decide_auto_advance(const AutoAdvanceInput& in) {
    const double end = effective_end(in.duration, in.item_end);
    if (!(end > 0.0)) return AutoAdvance::None;

    // 0.5 s tolerance: position polling is per-frame and EOS can land a hair
    // short of the reported duration.
    if (in.position >= end - 0.5) {
        // playback_started gates both modes: right after a load the position
        // can still be the previous item's end, which would skip this item.
        if (!in.playback_started) return AutoAdvance::Held;
        if (in.master_shuffle) return AutoAdvance::Advance;
        return in.current_item != in.last_advanced_item ? AutoAdvance::Advance
                                                        : AutoAdvance::Held;
    }
    if (in.position < end - 1.0) return AutoAdvance::ResetGuard;
    return AutoAdvance::None;
}

}  // namespace app
