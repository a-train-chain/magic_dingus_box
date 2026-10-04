// Unit tests for kiosk_exit.h — the process exit code the kiosk reports
// when display initialization fails. Pure logic; runs on the Mac.
//
// Why the code matters: update.sh starts the freshly installed kiosk and
// rolls the whole update back if it does not come up. A box with no TV
// connected (or a TV switched to another input) used to exit 1 here — the
// same code as a genuinely broken build — so a perfectly good update was
// rolled back every time it ran on a box nobody was looking at. The
// distinct code lets update.sh tell "the binary runs, there is just no
// screen" apart from a real failure.

#include <catch2/catch_test_macros.hpp>

#include "platform/kiosk_exit.h"

using namespace platform;

TEST_CASE("no connected display maps to the dedicated exit code") {
    REQUIRE(exit_code_for_display_init_failure(DisplayInitFailure::NoConnectedDisplay)
            == kExitNoDisplay);
}

TEST_CASE("the no-display code is the documented 69 (EX_UNAVAILABLE)") {
    // update.sh hard-codes 69 (KIOSK_EXIT_NO_DISPLAY). Changing one
    // without the other silently re-arms the rollback-on-headless bug.
    REQUIRE(kExitNoDisplay == 69);
}

TEST_CASE("every other display failure stays a plain failure (exit 1)") {
    REQUIRE(exit_code_for_display_init_failure(DisplayInitFailure::NoDrmDevice) == 1);
    REQUIRE(exit_code_for_display_init_failure(DisplayInitFailure::NoDrmMaster) == 1);
    REQUIRE(exit_code_for_display_init_failure(DisplayInitFailure::NoCrtc) == 1);
    REQUIRE(exit_code_for_display_init_failure(DisplayInitFailure::Other) == 1);
}

TEST_CASE("a failure that was never classified is never reported as no-display") {
    // DrmDisplay starts out with None; if initialize() returns false
    // without classifying, the safe answer is the generic failure, so a
    // broken build can never masquerade as a missing TV.
    REQUIRE(exit_code_for_display_init_failure(DisplayInitFailure::None) == 1);
}

TEST_CASE("the no-display code avoids codes with other meanings") {
    REQUIRE(kExitNoDisplay != 0);    // success
    REQUIRE(kExitNoDisplay != 1);    // generic failure
    REQUIRE(kExitNoDisplay < 128);   // not a signal-style code
    REQUIRE(kExitNoDisplay != 75);   // named a clean exit in the kiosk unit
}
