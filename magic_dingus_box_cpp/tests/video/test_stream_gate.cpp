// Unit tests for StreamGate — the stale-frame suppressor between video
// streams. GstRenderer keeps the previous stream's last frame in its GL
// textures; without the gate, every new-stream start has a window
// (pipeline started, first frame not yet decoded) where render() paints
// that stale frame. Observed on hardware as the intro video's last
// frame flashing before a Media Browser movie starts.
//
// Pure logic, no GL/GStreamer — runs on the dev machine.

#include <catch2/catch_test_macros.hpp>

#include "video/stream_gate.h"

using video::StreamGate;

TEST_CASE("gate suppresses drawing before any stream has ever loaded") {
    // At boot the textures have never been uploaded; drawing them would
    // show allocation garbage. Generation 0 = "no load yet".
    StreamGate g;
    REQUIRE_FALSE(g.on_render_tick(/*generation=*/0, /*got_new_sample=*/false));
}

TEST_CASE("first frame of a stream enables drawing") {
    StreamGate g;
    REQUIRE_FALSE(g.on_render_tick(1, false));  // load happened, no frame yet
    REQUIRE(g.on_render_tick(1, true));         // first frame arrives
}

TEST_CASE("mid-stream repeat frames still draw") {
    // A 24/30fps video on a 60Hz render loop delivers a new sample only
    // every other tick — the in-between ticks MUST redraw the last
    // frame, or video would flicker black at half rate.
    StreamGate g;
    g.on_render_tick(1, true);
    REQUIRE(g.on_render_tick(1, false));
    REQUIRE(g.on_render_tick(1, false));
    REQUIRE(g.on_render_tick(1, true));
    REQUIRE(g.on_render_tick(1, false));
}

TEST_CASE("new stream suppresses the previous stream's stale frame") {
    // The bug this gate exists for: intro (gen 1) played and finished;
    // movie (gen 2) starts; until the movie's first frame decodes, the
    // renderer must NOT paint the intro's leftover frame.
    StreamGate g;
    g.on_render_tick(1, true);                  // intro playing
    REQUIRE_FALSE(g.on_render_tick(2, false));  // movie loading...
    REQUIRE_FALSE(g.on_render_tick(2, false));  // ...still decoding...
    REQUIRE(g.on_render_tick(2, true));         // movie's first frame
    REQUIRE(g.on_render_tick(2, false));        // repeat frames fine again
}

TEST_CASE("sample arriving on the same tick as the generation change draws") {
    // load_file() tears the old pipeline down to NULL first, which
    // flushes the appsink — a sample seen after a generation bump can
    // only be from the NEW stream, so it may draw immediately.
    StreamGate g;
    g.on_render_tick(1, true);
    REQUIRE(g.on_render_tick(2, true));
}

TEST_CASE("GL reset suppresses drawing until the next real frame") {
    // After RetroArch hands the GL context back, reset_gl() recreates
    // the textures — their contents are undefined (the historical
    // "green rectangle in the corner" artifact). Nothing may draw until
    // a fresh frame is uploaded, even though the generation is
    // unchanged.
    StreamGate g;
    g.on_render_tick(3, true);
    g.on_gl_reset();
    REQUIRE_FALSE(g.on_render_tick(3, false));
    REQUIRE(g.on_render_tick(3, true));
}
