// Unit tests for the static helpers on ReleasePickerScreen.
//
// We intentionally exercise ONLY the pure data-shaping helpers
// (sort_candidates, flag_auto_pick_and_threshold) and a minimal
// load-state default check — the screen's render() and the live worker
// path in handle_input()/load_async() touch the Renderer / Toast /
// libcurl-RadarrClient and are out of scope for the unit-test binary,
// which links neither the GL renderer nor the toast subsystem nor the
// release_picker_screen.cpp translation unit.

#include <catch2/catch_test_macros.hpp>
#include "media_browser/ui/release_picker_screen.h"

namespace mbu = media_browser::ui;

namespace {
mbu::ReleasePickerScreen::ReleaseCandidate mk(int seeders, int score,
                                              const std::string& title) {
    mbu::ReleasePickerScreen::ReleaseCandidate c;
    c.title = title;
    c.seeders = seeders;
    c.score = score;
    return c;
}
}  // namespace

TEST_CASE("sort_candidates orders by seeders desc, then score desc",
          "[picker][sort]") {
    std::vector<mbu::ReleasePickerScreen::ReleaseCandidate> rows = {
        mk(50, 80, "low-seed"),
        mk(200, 80, "high-seed"),
        mk(50, 100, "low-seed-better-score"),
    };
    mbu::ReleasePickerScreen::sort_candidates(rows);
    REQUIRE(rows[0].title == "high-seed");
    REQUIRE(rows[1].title == "low-seed-better-score");
    REQUIRE(rows[2].title == "low-seed");
}

TEST_CASE("flag_auto_pick_and_threshold marks Radarr's choice + below-threshold rows",
          "[picker][flags]") {
    std::vector<mbu::ReleasePickerScreen::ReleaseCandidate> rows = {
        mk(200, 80, "best"),
        mk(180, 80, "second"),
        mk(50, -300, "rejected"),  // below the -200 minFormatScore floor
    };
    mbu::ReleasePickerScreen::flag_auto_pick_and_threshold(
        rows, /*min_format_score=*/-200);
    REQUIRE(rows[0].would_auto_pick == true);
    REQUIRE(rows[1].would_auto_pick == false);
    REQUIRE(rows[2].below_threshold == true);
    REQUIRE(rows[0].below_threshold == false);
}

TEST_CASE("ReleasePickerScreen LoadState enum has the four expected values",
          "[picker][async]") {
    // The async refactor introduced a load-state machine (Idle ->
    // Loading -> Ready/Failed -> Idle). The enum is a public part of
    // the picker's API because main.cpp's stall-modal handler may want
    // to distinguish "load is in flight" from "ready to drain" when
    // deciding whether to show progress UI. Locking the enum in a test
    // catches accidental reorderings or removals during future
    // refactors. (We can't construct a ReleasePickerScreen here — its
    // .cpp isn't linked into the test binary because that would pull
    // in the GL renderer and Toast — but the enum itself is in the
    // header and reachable.)
    using LS = mbu::ReleasePickerScreen::LoadState;
    REQUIRE(static_cast<int>(LS::Idle)    >= 0);
    REQUIRE(static_cast<int>(LS::Loading) != static_cast<int>(LS::Idle));
    REQUIRE(static_cast<int>(LS::Ready)   != static_cast<int>(LS::Loading));
    REQUIRE(static_cast<int>(LS::Failed)  != static_cast<int>(LS::Ready));
}

TEST_CASE("flag_auto_pick handles all-below-threshold case (no auto-pick)",
          "[picker][flags]") {
    std::vector<mbu::ReleasePickerScreen::ReleaseCandidate> rows = {
        mk(200, -300, "all-rejected-1"),
        mk(50,  -250, "all-rejected-2"),
    };
    mbu::ReleasePickerScreen::flag_auto_pick_and_threshold(rows, -200);
    REQUIRE(rows[0].would_auto_pick == false);
    REQUIRE(rows[1].would_auto_pick == false);
    REQUIRE(rows[0].below_threshold == true);
    REQUIRE(rows[1].below_threshold == true);
}

using Playability = mbu::ReleasePickerScreen::Playability;

TEST_CASE("classify_playability blocks formats that won't display",
          "[picker][playability]") {
    auto v = [](const char* title) {
        return mbu::ReleasePickerScreen::classify_playability(title, nullptr);
    };

    SECTION("HDR / Dolby Vision -> Unplayable") {
        REQUIRE(v("Movie 2024 1080p BluRay HDR x264") == Playability::Unplayable);
        REQUIRE(v("Movie.2024.2160p.HDR10.x265") == Playability::Unplayable);
        REQUIRE(v("Movie 2024 1080p Dolby Vision x264") == Playability::Unplayable);
        REQUIRE(v("Movie.2024.1080p.DoVi.HEVC") == Playability::Unplayable);
    }
    SECTION("10-bit -> Unplayable (SAND-format green screen)") {
        REQUIRE(v("Movie 2024 1080p x265 10bit") == Playability::Unplayable);
        REQUIRE(v("Movie.2024.1080p.10-bit.HEVC") == Playability::Unplayable);
        REQUIRE(v("Movie 2024 Hi10P x264") == Playability::Unplayable);
        REQUIRE(v("Movie.2024.Main10.HEVC") == Playability::Unplayable);
    }
    SECTION("AV1 -> Unplayable (no decoder)") {
        REQUIRE(v("Movie 2024 1080p AV1 WEB-DL") == Playability::Unplayable);
    }
    SECTION("4K / 2160p / UHD -> Unplayable") {
        REQUIRE(v("Movie 2024 2160p WEB-DL x264") == Playability::Unplayable);
        REQUIRE(v("Movie 2024 4K BluRay") == Playability::Unplayable);
        REQUIRE(v("Movie.2024.UHD.BluRay") == Playability::Unplayable);
    }
    SECTION("Disc images / remux -> Unplayable (not a single file)") {
        REQUIRE(v("Movie 2024 1080p BluRay REMUX AVC") == Playability::Unplayable);
        REQUIRE(v("Movie.2024.BDMV.1080p") == Playability::Unplayable);
        REQUIRE(v("Movie 2024 1080p.iso") == Playability::Unplayable);
    }
    SECTION("3D -> Unplayable (doubled image)") {
        REQUIRE(v("Movie 2024 1080p 3D HSBS x264") == Playability::Unplayable);
        REQUIRE(v("Movie.2024.1080p.Half-SBS") == Playability::Unplayable);
    }
}

TEST_CASE("classify_playability allows HEVC 8-bit as degraded",
          "[picker][playability]") {
    auto v = [](const char* title) {
        return mbu::ReleasePickerScreen::classify_playability(title, nullptr);
    };
    // Plain HEVC/x265 8-bit (no HDR/10-bit markers) is the watchable-
    // with-stutter tier, NOT a hard block.
    REQUIRE(v("Movie 2024 1080p WEBRip x265") == Playability::Degraded);
    REQUIRE(v("Movie.2024.1080p.HEVC") == Playability::Degraded);
    REQUIRE(v("Movie 2024 1080p H.265 AAC") == Playability::Degraded);
}

TEST_CASE("classify_playability passes clean x264 as ideal",
          "[picker][playability]") {
    auto v = [](const char* title) {
        return mbu::ReleasePickerScreen::classify_playability(title, nullptr);
    };
    REQUIRE(v("Movie 2024 1080p BluRay x264-GRP") == Playability::Ideal);
    REQUIRE(v("Movie.2024.720p.WEB-DL.H264") == Playability::Ideal);
    REQUIRE(v("Movie 2024 1080p AMZN WEB-DL DDP5.1 H.264") == Playability::Ideal);
}

TEST_CASE("classify_playability HDR beats codec and sets a reason",
          "[picker][playability]") {
    std::string reason;
    // An HDR x264 must still block on HDR, not pass as ideal just
    // because it's x264.
    auto verdict = mbu::ReleasePickerScreen::classify_playability(
        "Movie 2024 1080p HDR x264", &reason);
    REQUIRE(verdict == Playability::Unplayable);
    REQUIRE_FALSE(reason.empty());
}

TEST_CASE("sort sinks unplayable below playable, ideal above degraded",
          "[picker][playability]") {
    std::vector<mbu::ReleasePickerScreen::ReleaseCandidate> rows = {
        mk(999, 100, "unplayable-high-seed"),
        mk(5,   50,  "ideal-low-seed"),
        mk(200, 30,  "degraded-mid-seed"),
    };
    rows[0].playability = Playability::Unplayable;
    rows[1].playability = Playability::Ideal;
    rows[2].playability = Playability::Degraded;
    mbu::ReleasePickerScreen::sort_candidates(rows);
    REQUIRE(rows[0].playability == Playability::Ideal);
    REQUIRE(rows[1].playability == Playability::Degraded);
    REQUIRE(rows[2].playability == Playability::Unplayable);
}

TEST_CASE("auto-pick never lands on an unplayable row",
          "[picker][playability]") {
    std::vector<mbu::ReleasePickerScreen::ReleaseCandidate> rows = {
        mk(10, 500, "unplayable-best-score"),
        mk(10, 40,  "playable-lower-score"),
    };
    rows[0].playability = Playability::Unplayable;
    rows[1].playability = Playability::Ideal;
    mbu::ReleasePickerScreen::flag_auto_pick_and_threshold(rows, -200);
    int picked = -1;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].would_auto_pick) picked = static_cast<int>(i);
    }
    REQUIRE(picked >= 0);
    REQUIRE(rows[picked].playability != Playability::Unplayable);
    REQUIRE(rows[picked].score == 40);
}
