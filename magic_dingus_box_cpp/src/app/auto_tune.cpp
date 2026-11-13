#include "auto_tune.h"

#include <algorithm>

namespace app {

namespace {

bool contains(const std::vector<int>& v, int x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

}  // namespace

bool auto_tune_gates_open(const AutoTuneInput& in) {
    return in.guide_style && in.intro_complete && in.media_info_read && in.on_main_screen &&
           !in.video_active &&
           !in.switch_pending && !in.loading_game && !in.post_game_pending &&
           in.now_utc >= in.wait_until_utc;
}

std::optional<int> decide_auto_tune(const AutoTuneInput& in) {
    if (!auto_tune_gates_open(in)) return std::nullopt;

    const int n = static_cast<int>(in.tunable.size());
    // Has a schedule and was not given up on.
    const auto eligible = [&](int c) {
        return c >= 0 && c < n && in.tunable[c] && !contains(in.gave_up, c);
    };

    if (eligible(in.last_channel)) return in.last_channel;

    if (!in.gave_up.empty()) {
        // Move on from the most recent give-up. A give-up leaves the last
        // channel watched on the channel that failed, and guide_gave_up keeps
        // a channel given up again where it first was — so the last channel,
        // when it is one of them, is the most recent; else the newest entry.
        const int from =
            contains(in.gave_up, in.last_channel) ? in.last_channel : in.gave_up.back();
        for (int i = 1; i <= n; ++i) {
            const int c = ((from + i) % n + n) % n;
            if (eligible(c)) return c;
        }
        return std::nullopt;  // every channel that could play was given up on
    }

    if (eligible(1)) return 1;
    for (int c = 0; c < n; ++c) {
        if (eligible(c)) return c;
    }
    return std::nullopt;
}

}  // namespace app
