// Regression guard for the hardware-HEVC decision. Pure logic.
//
// This test is documentation with teeth: the Pi 5's only hardware video
// decoder is the HEVC block, so "enable hardware HEVC" looks like an
// obvious win and has been attempted twice. It is not a win for THIS
// pipeline, and the numbers are in decoder_policy.h.

#include <catch2/catch_test_macros.hpp>

#include "video/decoder_policy.h"

TEST_CASE("hardware HEVC stays disabled — it is 2x MORE expensive here") {
    // Measured 2026-07-26, Pi 5 / Trixie / GStreamer 1.26.2, same file:
    //   -> RGBA : software 41% of 400%, HARDWARE 87%
    //   -> I420 : software 38% of 400%, HARDWARE 73%
    // The decoder itself is fast (6.79x realtime raw), but its SAND
    // tiled output must be detiled by videoconvert on the CPU, which
    // costs more than software decoding. Negotiation working (the
    // GStreamer 1.26 fix) is NOT sufficient reason to switch.
    REQUIRE_FALSE(video::hw_hevc_beneficial());
}

TEST_CASE("the decision is compile-time constant") {
    // constexpr so the disable can never depend on runtime state that
    // might differ between the probe and the pipeline build.
    static_assert(!video::hw_hevc_beneficial(),
                  "hardware HEVC must remain disabled; see decoder_policy.h");
    SUCCEED();
}
