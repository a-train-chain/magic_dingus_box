// app::cursor — sequential NEXT/PREV over one playlist. The end-of-playlist
// case is the one with history: with looping off it must route to
// app::stop_to_menu, never park the cursor on item 0 with no video (the
// renderer then drew nothing until reboot).

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "app/playlist_cursor.h"

namespace cur = app::cursor;

namespace {
app::Playlist playlist(const std::vector<std::string>& source_types, bool loop) {
    app::Playlist p;
    p.loop = loop;
    for (const auto& t : source_types) {
        app::PlaylistItem it;
        it.source_type = t;
        p.items.push_back(it);
    }
    return p;
}
}  // namespace

TEST_CASE("cursor: next walks forward", "[cursor]") {
    CHECK(cur::sequential_next(0, 3, false).index == 1);
    CHECK(cur::sequential_next(1, 3, false).index == 2);
    CHECK_FALSE(cur::sequential_next(1, 3, false).stop_to_menu);
    // Nothing played yet starts at the top.
    CHECK(cur::sequential_next(-1, 3, false).index == 0);
}

TEST_CASE("cursor: end of playlist wraps when looping", "[cursor]") {
    auto s = cur::sequential_next(2, 3, true);
    CHECK_FALSE(s.stop_to_menu);
    CHECK(s.index == 0);
    CHECK(cur::sequential_next(0, 1, true).index == 0);
}

TEST_CASE("cursor: end of playlist with loop off goes back to the menu", "[cursor]") {
    auto s = cur::sequential_next(2, 3, false);
    CHECK(s.stop_to_menu);
    CHECK(s.index == -1);
    CHECK(cur::sequential_next(0, 1, false).stop_to_menu);
    // A cursor past the end (playlist shrank under it) is also the end.
    CHECK(cur::sequential_next(7, 3, false).stop_to_menu);
}

TEST_CASE("cursor: an empty playlist has nowhere to go", "[cursor]") {
    CHECK(cur::sequential_next(-1, 0, true).stop_to_menu);
    CHECK(cur::previous_index(0, 0) == -1);
}

TEST_CASE("cursor: previous wraps at the top", "[cursor]") {
    CHECK(cur::previous_index(2, 3) == 1);
    CHECK(cur::previous_index(1, 3) == 0);
    CHECK(cur::previous_index(0, 3) == 2);
    CHECK(cur::previous_index(-1, 3) == 2);
    CHECK(cur::previous_index(5, 3) == 2);   // stale cursor stays in range
    CHECK(cur::previous_index(0, 1) == 0);
}

TEST_CASE("cursor: effective loop — video playlists use their own key", "[cursor]") {
    CHECK(cur::effective_loop(playlist({"local", "local"}, true), false));
    CHECK_FALSE(cur::effective_loop(playlist({"local"}, false), true));
    // Game playlists ignore `loop:` and follow the global setting.
    CHECK(cur::effective_loop(playlist({"emulated_game"}, false), true));
    CHECK_FALSE(cur::effective_loop(playlist({"emulated_game"}, true), false));
    // A mixed playlist is a video playlist.
    CHECK_FALSE(cur::effective_loop(playlist({"emulated_game", "local"}, false), true));
}

TEST_CASE("cursor: retry order after a failed load", "[cursor]") {
    // Item 1 was playing, item 2 failed: try 3, 4, 0 — and stop before
    // coming back round to 1.
    CHECK(cur::retry_order(2, 1, 5) == std::vector<int>{3, 4, 0});
    // Nothing was playing: every other item once, never the failed one again.
    CHECK(cur::retry_order(0, -1, 4) == std::vector<int>{1, 2, 3});
    // Two items, the other one is what was playing: nothing to retry.
    CHECK(cur::retry_order(1, 0, 2).empty());
    CHECK(cur::retry_order(0, -1, 1).empty());
    CHECK(cur::retry_order(0, -1, 0).empty());
}
