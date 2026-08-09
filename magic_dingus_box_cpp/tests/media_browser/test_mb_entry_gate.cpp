// The Media Browser display-mode entry gate.
//
// Every MB screen is authored against the fixed 1280x720 logical canvas;
// CRT_NATIVE runs a 640x480 canvas, where the Library/filter slide-in
// panels (x=740..1220, closed at x=1280) sit entirely off-screen. The
// shipped fix is a gate — CRT mode blocks entry with an explanatory row
// — rather than a canvas-aware re-layout, so what these tests pin down
// is the DECISION: which display modes may enter, and where the display
// blocker sits in the Movies row's precedence order.
//
// The gate is pure constexpr logic over config::display::logical_canvas()
// (the same source of truth main.cpp sizes the renderer with), so it runs
// on the Mac with no kiosk, no GL, no display.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "media_browser/mb_entry_gate.h"

using media_browser::MoviesRowState;
using media_browser::display_supports_media_browser;
using media_browser::movies_row_state;

TEST_CASE("Modern TV's canvas hosts the Media Browser", "[media_browser][gate]") {
    // MODERN_TV pins the 1280x720 logical canvas the MB layout was
    // authored for.
    REQUIRE(display_supports_media_browser(/*crt_native=*/false));
}

TEST_CASE("CRT Native's canvas cannot host the Media Browser",
          "[media_browser][gate]") {
    // CRT_NATIVE's 640x480 canvas ends left of the filter panel's OPEN
    // position (x=740) — the panels would render entirely off-screen.
    REQUIRE_FALSE(display_supports_media_browser(/*crt_native=*/true));
}

TEST_CASE("the gate derives from logical_canvas(), not a hardcoded mode check",
          "[media_browser][gate]") {
    // If logical_canvas() ever changes for a mode, the gate must follow
    // it. Assert the linkage the gate relies on: the predicate is exactly
    // "this mode's canvas == the UI 720p canvas".
    for (bool crt : {false, true}) {
        const auto c = config::display::logical_canvas(crt);
        const bool canvas_is_ui =
            c.w == config::display::UI_LOGICAL_WIDTH &&
            c.h == config::display::UI_LOGICAL_HEIGHT;
        REQUIRE(display_supports_media_browser(crt) == canvas_is_ui);
    }
}

TEST_CASE("all layers green on Modern TV -> Ready", "[media_browser][gate]") {
    REQUIRE(movies_row_state(false, true, true, true) == MoviesRowState::Ready);
}

TEST_CASE("CRT mode blocks the row regardless of every other layer",
          "[media_browser][gate]") {
    // The display blocker is checked FIRST: in CRT mode the feature
    // cannot open at all, so pointing the owner at VPN config or the
    // drive would misstate what is actually in the way. Sweep all 8
    // combinations of the other flags.
    for (bool vpn_cfg : {false, true}) {
        for (bool drive : {false, true}) {
            for (bool healthy : {false, true}) {
                REQUIRE(movies_row_state(true, vpn_cfg, drive, healthy) ==
                        MoviesRowState::NeedsModernTv);
            }
        }
    }
}

TEST_CASE("Modern TV keeps the shipped precedence: VPN config, then drive, "
          "then tunnel health", "[media_browser][gate]") {
    // Unconfigured VPN wins over everything downstream of it.
    REQUIRE(movies_row_state(false, false, false, false) ==
            MoviesRowState::NeedsVpnConfig);
    REQUIRE(movies_row_state(false, false, true, true) ==
            MoviesRowState::NeedsVpnConfig);
    // Drive before tunnel health: a missing drive also takes Radarr down
    // (both flags trip), but "connect the drive" is the actionable cause.
    REQUIRE(movies_row_state(false, true, false, false) ==
            MoviesRowState::NeedsDrive);
    REQUIRE(movies_row_state(false, true, false, true) ==
            MoviesRowState::NeedsDrive);
    // Tunnel down hides the row (the toast is the signal).
    REQUIRE(movies_row_state(false, true, true, false) ==
            MoviesRowState::HiddenTunnelDown);
}

TEST_CASE("blocked-display row label matches OWNER_GUIDE section 10",
          "[media_browser][gate]") {
    // The exact string the owner reads on the TV, documented in
    // OWNER_GUIDE.md's message table and asserted against the shipped
    // binary with `strings`. Changing it means updating both.
    REQUIRE(std::string(media_browser::kMoviesNeedsModernTvLabel) ==
            "Movies (needs Modern TV display)");
    REQUIRE_FALSE(std::string(media_browser::kMoviesNeedsModernTvSublabel).empty());
    REQUIRE_FALSE(std::string(media_browser::kMoviesNeedsModernTvToast).empty());
    REQUIRE_FALSE(
        std::string(media_browser::kMoviesClosedByDisplaySwitchToast).empty());
}
