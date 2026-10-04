// app::PostGameGate — the return-from-game window. Regression for the
// Pi 5 2026-10-03 race: the session-end hook published screen=playlist
// with the pre-launch Settings snapshot before the main loop had
// force-closed that menu and run its post-game reset, so a SELECT aimed at
// "the open game list" landed on the main menu's Master Shuffle row.

#include <catch2/catch_test_macros.hpp>

#include "app/post_game_gate.h"

using app::PostGameGate;
using app::ScreenMode;

TEST_CASE("PostGameGate: idle gate is transparent", "[post_game_gate]") {
    PostGameGate g;
    CHECK_FALSE(g.settling());
    CHECK(g.accepts_input());
    CHECK(g.published_screen(ScreenMode::Settings) == ScreenMode::Settings);
    CHECK(g.published_screen(ScreenMode::Playlist) == ScreenMode::Playlist);
    // No session ended: nothing to become ready from, at any reset state.
    CHECK_FALSE(g.take_ready(false));
    CHECK_FALSE(g.take_ready(true));
}

TEST_CASE("PostGameGate: session end holds the menu status and drops stale input",
          "[post_game_gate]") {
    PostGameGate g;
    g.session_ended();
    CHECK(g.settling());
    // The rest of the batch that launched the game must not dispatch.
    CHECK_FALSE(g.accepts_input());
    // Whatever the live UI says (the stale snapshot claimed Settings), the
    // published screen stays RetroArch until the kiosk can take input.
    CHECK(g.published_screen(ScreenMode::Settings) == ScreenMode::RetroArch);
    CHECK(g.published_screen(ScreenMode::Playlist) == ScreenMode::RetroArch);
}

TEST_CASE("PostGameGate: not ready while the display reset is still pending",
          "[post_game_gate]") {
    PostGameGate g;
    g.session_ended();
    // The iteration the hook fires in has reset_display set and its reset
    // block has not run yet.
    CHECK_FALSE(g.take_ready(true));
    CHECK(g.settling());
    CHECK_FALSE(g.accepts_input());
    // Next iteration: the reset block ran and cleared the flag.
    CHECK(g.take_ready(false));
    CHECK_FALSE(g.settling());
    CHECK(g.accepts_input());
    CHECK(g.published_screen(ScreenMode::Playlist) == ScreenMode::Playlist);
}

TEST_CASE("PostGameGate: ready edge fires exactly once per session",
          "[post_game_gate]") {
    PostGameGate g;
    g.session_ended();
    CHECK(g.take_ready(false));
    // A second take must not re-drain input the user is now legitimately
    // producing against the visible menu.
    CHECK_FALSE(g.take_ready(false));
    CHECK_FALSE(g.take_ready(false));

    // A later game arms it again.
    g.session_ended();
    CHECK(g.settling());
    CHECK(g.take_ready(false));
    CHECK_FALSE(g.settling());
}

TEST_CASE("PostGameGate: launch failure before the handover is ready next iteration",
          "[post_game_gate]") {
    // No display handover happened, so no reset is ever pending — the
    // menu never left the screen, and holding it would only add latency.
    PostGameGate g;
    g.session_ended();
    CHECK(g.take_ready(false));
    CHECK(g.accepts_input());
}

TEST_CASE("PostGameGate: repeated session_ended before ready is one settle",
          "[post_game_gate]") {
    PostGameGate g;
    g.session_ended();
    g.session_ended();
    CHECK_FALSE(g.take_ready(true));
    CHECK(g.take_ready(false));
    CHECK_FALSE(g.take_ready(false));
}
