// Tests for utils::fuzzy_name_matches — the rule behind
// resolve_video_path's fuzzy fallback when a playlist references a file
// that is no longer on disk under its exact name.
//
// The old implementation accepted ANY entry whose name merely started
// with the target stem (same extension), so a playlist entry for
// "Mega Man.mp4" silently resolved to "Mega Man 2.mp4" or
// "Mega Man X.mp4" when the real file was missing — the kiosk played
// the wrong title, the log even reported a successful match, and WHICH
// wrong sibling won depended on unspecified directory-iteration order.
// These tests pin the boundary rule: exact stem, " (", " [", or "."
// after the stem; anything else is a different title.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "utils/path_resolver.h"

using utils::fuzzy_name_matches;

TEST_CASE("exact stem and extension matches") {
    REQUIRE(fuzzy_name_matches("Mega Man.mp4", "Mega Man", ".mp4"));
}

TEST_CASE("paren ID suffix matches — the designed 'Title (ID).mp4' pattern") {
    REQUIRE(fuzzy_name_matches("Mega Man (1990).mp4", "Mega Man", ".mp4"));
}

TEST_CASE("bracket ID suffix matches — yt-dlp names in shipped media") {
    REQUIRE(fuzzy_name_matches(
        "A DAY IN THE MALL - 1991 [VLX2_eOKev4].mp4",
        "A DAY IN THE MALL - 1991", ".mp4"));
}

TEST_CASE("dot variant suffix matches — 30fps encode variants") {
    REQUIRE(fuzzy_name_matches("intro.30fps.mov", "intro", ".mov"));
}

TEST_CASE("a numbered sequel is NOT the same title") {
    REQUIRE_FALSE(fuzzy_name_matches("Mega Man 2.mp4", "Mega Man", ".mp4"));
}

TEST_CASE("a longer word sharing the prefix is NOT the same title") {
    REQUIRE_FALSE(fuzzy_name_matches("Mega Mania.mp4", "Mega Man", ".mp4"));
    REQUIRE_FALSE(fuzzy_name_matches("Titles.mp4", "Title", ".mp4"));
}

TEST_CASE("a space-suffixed variant without bracket or paren is NOT a match") {
    REQUIRE_FALSE(fuzzy_name_matches("Mega Man X.mp4", "Mega Man", ".mp4"));
    REQUIRE_FALSE(fuzzy_name_matches("Mega Man Remastered.mp4", "Mega Man", ".mp4"));
}

TEST_CASE("extension must match exactly") {
    REQUIRE_FALSE(fuzzy_name_matches("Mega Man.mkv", "Mega Man", ".mp4"));
    REQUIRE_FALSE(fuzzy_name_matches("Mega Man (1990).mov", "Mega Man", ".mp4"));
}

TEST_CASE("an entry shorter than the stem never matches") {
    REQUIRE_FALSE(fuzzy_name_matches("Mega.mp4", "Mega Man", ".mp4"));
}

TEST_CASE("unrelated names never match") {
    REQUIRE_FALSE(fuzzy_name_matches("Contra.mp4", "Mega Man", ".mp4"));
}
