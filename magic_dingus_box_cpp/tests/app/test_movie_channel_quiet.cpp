#include <catch2/catch_test_macros.hpp>
#include "app/movie_channel_quiet.h"

TEST_CASE("movie-channel grace spans channel surfing and expires exactly at 120 seconds", "[movie_channel_quiet]") {
    app::MovieChannelQuiet q;
    using Cmd = app::MovieChannelQuiet::Cmd;
    CHECK(q.tick(false, 0) == Cmd::None);
    CHECK(q.tick(true, 1) == Cmd::Claim);
    CHECK(q.tick(false, 10) == Cmd::None);
    CHECK(q.tick(true, 40) == Cmd::None);
    CHECK(q.tick(false, 50) == Cmd::None);
    CHECK(q.tick(false, 169.999) == Cmd::None);
    CHECK(q.claimed());
    CHECK(q.tick(false, 170) == Cmd::Release);
    CHECK_FALSE(q.claimed());
    CHECK(q.tick(true, 171) == Cmd::Claim);
}
