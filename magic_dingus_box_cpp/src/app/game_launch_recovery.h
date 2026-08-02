#pragma once

#include "app_state.h"

namespace app {

void prepare_kiosk_state_after_game(AppState& state);

// Reset the launch-plate state for a NEW game launch: plate fully opaque,
// any in-flight post-game fade cancelled, is_loading_game raised. Called
// from the game-session BEGIN hook so every launch route gets it.
void prepare_loading_state_for_launch(AppState& state);

// Pure ramp for the return dissolve: 1.0 at elapsed<=0 down to 0.0 at
// elapsed>=duration, clamped. duration<=0 returns 0 (treat a degenerate
// dissolve as already finished).
float return_dissolve_alpha(float elapsed_ms, float duration_ms);

}  // namespace app
