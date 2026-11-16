// The CRT engine choice in the app state: the board gate applied at the last
// moment (never to the saved choice), and the old enhanced_crt_enabled flag
// kept in step for every reader that still uses it.

#include <catch2/catch_test_macros.hpp>

#include "app/app_state.h"

TEST_CASE("effective engine honours the gate without changing the choice", "[tube]") {
    using app::CrtEngine;
    using app::effective_crt_engine;
    REQUIRE(effective_crt_engine(CrtEngine::Tube, true) == CrtEngine::Tube);
    REQUIRE(effective_crt_engine(CrtEngine::Tube, false) == CrtEngine::Enhanced);
    REQUIRE(effective_crt_engine(CrtEngine::Enhanced, false) == CrtEngine::Enhanced);
    REQUIRE(effective_crt_engine(CrtEngine::Enhanced, true) == CrtEngine::Enhanced);
    REQUIRE(effective_crt_engine(CrtEngine::Classic, true) == CrtEngine::Classic);
    REQUIRE(effective_crt_engine(CrtEngine::Classic, false) == CrtEngine::Classic);
    static_assert(effective_crt_engine(CrtEngine::Tube, false) == CrtEngine::Enhanced,
                  "usable at compile time");
}

TEST_CASE("set_crt_engine keeps the old flag in step", "[tube]") {
    app::AppState::DisplaySettings d;
    REQUIRE(d.crt_engine == app::CrtEngine::Classic);
    REQUIRE_FALSE(d.enhanced_crt_enabled);
    REQUIRE(d.tube == tube::TubeSetting{});

    d.set_crt_engine(app::CrtEngine::Tube);
    REQUIRE(d.crt_engine == app::CrtEngine::Tube);
    REQUIRE(d.enhanced_crt_enabled);
    d.set_crt_engine(app::CrtEngine::Classic);
    REQUIRE(d.crt_engine == app::CrtEngine::Classic);
    REQUIRE_FALSE(d.enhanced_crt_enabled);
    d.set_crt_engine(app::CrtEngine::Enhanced);
    REQUIRE(d.crt_engine == app::CrtEngine::Enhanced);
    REQUIRE(d.enhanced_crt_enabled);
}
