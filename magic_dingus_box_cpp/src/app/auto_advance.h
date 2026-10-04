#pragma once

namespace app {

// Where a playlist item effectively ends: its `end:` trim when that falls
// inside the media, otherwise the real duration. A trim at or past the real
// end would never be reached (the pipeline hits EOS first), so it is ignored
// rather than left to hang the item. An unknown duration (<= 0, not yet
// prerolled) stays unknown — a trim must not make an unopened file look ended.
double effective_end(double duration, double item_end);

struct AutoAdvanceInput {
    double position = 0.0;
    double duration = 0.0;     // real media duration
    double item_end = 0.0;     // the item's `end:` trim, 0 = none
    bool playback_started = false;
    bool master_shuffle = false;
    int current_item = -1;
    int last_advanced_item = -1;
};

enum class AutoAdvance {
    None,        // not enough information, or inside the pre-end window
    Advance,     // at the effective end: load the next item now
    Held,        // at the end, but this item already advanced / not started
    ResetGuard,  // well before the end: clear the once-per-item guard
};

// The per-frame auto-advance decision for an active playlist item.
AutoAdvance decide_auto_advance(const AutoAdvanceInput& in);

}  // namespace app
