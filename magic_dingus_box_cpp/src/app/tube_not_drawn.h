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

#include <optional>

#include "app/app_state.h"
#include "tube/tube_programs.h"
#include "tube/tube_setting.h"

namespace app {

inline tube::NotDrawn tube_not_drawn(const AppState& state) noexcept {
    if (!state.tube_render.drawing) return {};
    return tube::plan_passes_and_drops(tube::resolve(state.display_settings.tube), state.tube_render.picture)
        .not_drawn;
}

// The engine a full-screen film gets with the saved setting at the current output: Tube,
// or Enhanced when the look is too heavy for it (tube::tube_yields_to_enhanced, the same
// decision the renderer makes per frame, for the film picture main.cpp reports: the whole
// framebuffer). nullopt until the main loop has reported the output. Lets the controls warn
// BEFORE a film plays.
inline std::optional<CrtEngine> tube_film_engine(const AppState& state) noexcept {
    const tube::Picture& film = state.tube_render.film_picture;
    if (film.w <= 0.0 || film.h <= 0.0) return std::nullopt;
    return tube::tube_yields_to_enhanced(tube::resolve(state.display_settings.tube), film) ? CrtEngine::Enhanced
                                                                                          : CrtEngine::Tube;
}

// The engine drawing the picture on screen right now: the effective engine, except that
// a Tube that is not drawing because Enhanced draws instead - a film it yields
// (`yielding`), a tripped build guard or a failed build (`unavailable`) - reads Enhanced.
inline CrtEngine tube_engine_on_screen(const AppState& state) noexcept {
    const CrtEngine effective = effective_crt_engine(state.display_settings.crt_engine, tube_offered(state));
    if (effective == CrtEngine::Tube && (state.tube_render.yielding || !state.tube_render.unavailable.empty()))
        return CrtEngine::Enhanced;
    return effective;
}

}  // namespace app
