// tube_not_drawn.h - which effects the box is NOT drawing right now because the look
// cannot afford them at the picture it draws into (owner's decision, 2026-11-15: afterglow
// gives way first, then glow; no row's value changes).
//
// The decision is tube::plan_passes_and_drops() - the very function the renderer's
// FrameDriver plans with - for the saved setting and the picture the renderer reported
// this iteration (state.tube_render, filled by main.cpp from Renderer::tube_status()).
// Same inputs, same pure function, same answer: there is no second copy of the rule or
// the budget. The status writer, the TV menu and (through the status) the web page all
// read it from here. While the tube is not drawing (the Media Browser, another engine,
// before the first Tube frame) nothing is "not drawn": the result is empty.
//
// Pure: Renderer-free, I/O-free, header-only. A few microseconds: it runs per status
// refresh and per menu rebuild, never per frame.
#pragma once

#include "app/app_state.h"
#include "tube/tube_programs.h"
#include "tube/tube_setting.h"

namespace app {

inline tube::NotDrawn tube_not_drawn(const AppState& state) noexcept {
    if (!state.tube_render.drawing) return {};
    return tube::plan_passes_and_drops(tube::resolve(state.display_settings.tube), state.tube_render.picture)
        .not_drawn;
}

}  // namespace app
