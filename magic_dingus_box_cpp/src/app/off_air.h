#pragma once

// The automatic return from overnight channel 00 parking. Facts come from
// in-memory schedules; identity resolution stays with the caller. Pass
// current UTC to honor early selected-film return suppression. No I/O.
#include "app_state.h"
#include <algorithm>
#include <optional>

namespace app {
inline std::optional<int> off_air_return_row(const AppState& s, int return_row,
                                            bool return_on_air, bool switch_pending, double now_utc = 0) {
    if (s.off_air_return_id.empty() || s.tuned_channel != 0 || return_row <= 0 ||
        !return_on_air || switch_pending || now_utc < s.off_air_return_after_utc ||
        std::find(s.guide_gave_up.begin(), s.guide_gave_up.end(), return_row) != s.guide_gave_up.end())
        return std::nullopt;
    return return_row;
}
}  // namespace app
