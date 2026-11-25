#pragma once

#include "app_state.h"

namespace app {

// Enhanced's single composite pass. Leave the blur passes and the extra
// convergence samples disabled so fine lettering and moving faces stay clear.
inline void apply_cable_tv_look(AppState::DisplaySettings& display) {
    display.set_crt_engine(CrtEngine::Enhanced);
    display.scanline_intensity = 0.25f;
    display.rgb_mask_intensity = 0.16f;
    display.warmth_intensity = 0.12f;
    display.bloom_intensity = 0.0f;
    display.glow_intensity = 0.0f;
    display.interlacing_intensity = 0.0f;
    display.flicker_intensity = 0.0f;
}

}  // namespace app
