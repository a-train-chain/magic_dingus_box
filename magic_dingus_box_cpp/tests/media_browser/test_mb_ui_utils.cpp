// Tests for media_browser/ui/mb_ui_utils — the four helpers that used to be
// copy-pasted into every Marquee screen.
//
// Why these are testable at all: mb_ui_utils deliberately holds NO reference to
// ::ui::Renderer. Renderer drags in GLES, which does not exist in this binary,
// so every previous copy of these functions lived in an anonymous namespace in
// a .cpp that compiles only into the kiosk target and could not be asserted on
// from anywhere. truncate_to_width is parameterized on a measuring callable for
// exactly that reason; the Renderer-shaped convenience overload lives in
// mb_chrome and is a one-line forward into the core tested here.
//
// The measure callables below are deterministic (fixed pixels per byte) so the
// assertions are exact rather than font-metric-dependent.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "media_browser/ui/mb_ui_utils.h"
#include "ui/theme.h"

namespace mbu = media_browser::ui;

namespace {

// 10 px per byte, ignoring font_size. Makes budgets trivially countable:
// max_w = 100 fits exactly 10 bytes.
float measure_10px_per_byte(const std::string& s, int /*font_size*/) {
    return 10.0f * static_cast<float>(s.size());
}

// Scales with font_size so a test can prove the size argument is actually
// forwarded to the measurer rather than swallowed.
float measure_size_scaled(const std::string& s, int font_size) {
    return static_cast<float>(font_size) * static_cast<float>(s.size());
}

bool same_color(const ::ui::Color& a, const ::ui::Color& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

}  // namespace

// =====================================================================
// truncate_to_width
// =====================================================================

TEST_CASE("truncate_to_width returns text unchanged when it fits",
          "[mb_ui_utils][truncate]") {
    // 5 bytes * 10 px = 50 px, budget 100 px.
    REQUIRE(mbu::truncate_to_width("Alien", 16, 100.0f, measure_10px_per_byte)
            == "Alien");

    // Exactly-at-budget is a fit: the original uses <= and callers depend on
    // a string that measures precisely to the column width not gaining "...".
    REQUIRE(mbu::truncate_to_width("Alien", 16, 50.0f, measure_10px_per_byte)
            == "Alien");
}

TEST_CASE("truncate_to_width appends an ellipsis that stays inside the budget",
          "[mb_ui_utils][truncate]") {
    // "Blade Runner 2049" is 17 bytes = 170 px, budget 100 px => 10 bytes of
    // output total, of which 3 are the "...", so 7 bytes of text survive.
    const std::string out =
        mbu::truncate_to_width("Blade Runner 2049", 16, 100.0f,
                               measure_10px_per_byte);
    REQUIRE(out == "Blade R...");
    REQUIRE(out.size() == 10);
    REQUIRE(measure_10px_per_byte(out, 16) <= 100.0f);
    // The point of the helper: it must not overflow the column it was given.
    REQUIRE(out.substr(out.size() - 3) == "...");
}

TEST_CASE("truncate_to_width keeps one character when only four fit",
          "[mb_ui_utils][truncate]") {
    // Budget 40 px = 4 bytes: one character plus the three-dot ellipsis.
    REQUIRE(mbu::truncate_to_width("Nosferatu", 16, 40.0f,
                                   measure_10px_per_byte) == "N...");
}

TEST_CASE("truncate_to_width returns the bare ellipsis when nothing fits",
          "[mb_ui_utils][truncate]") {
    // 30 px = 3 bytes: not even "X..." fits, so every candidate is rejected
    // and the helper falls through to the ellipsis alone. NOTE this result
    // does NOT fit the budget either -- it is 3 bytes = 30 px against 20 --
    // and callers draw it anyway. Pinned deliberately: it is the pre-existing
    // observable behavior of all six copies and this refactor must not
    // silently change what a squeezed column renders.
    REQUIRE(mbu::truncate_to_width("Nosferatu", 16, 20.0f,
                                   measure_10px_per_byte) == "...");
    REQUIRE(mbu::truncate_to_width("Nosferatu", 16, 0.0f,
                                   measure_10px_per_byte) == "...");
}

TEST_CASE("truncate_to_width handles the empty string",
          "[mb_ui_utils][truncate]") {
    // Empty measures 0 px, so any non-negative budget takes the fits-early
    // return and gives back "" -- NOT "...", which is what a naive rewrite
    // that dropped the leading check would produce.
    REQUIRE(mbu::truncate_to_width("", 16, 100.0f, measure_10px_per_byte)
            == "");
    REQUIRE(mbu::truncate_to_width("", 16, 0.0f, measure_10px_per_byte) == "");

    // A negative budget cannot be satisfied by anything, and with no bytes to
    // trim the loop body never runs: the ellipsis fallback is the only exit.
    REQUIRE(mbu::truncate_to_width("", 16, -1.0f, measure_10px_per_byte)
            == "...");
}

TEST_CASE("truncate_to_width forwards font_size to the measurer",
          "[mb_ui_utils][truncate]") {
    const std::string text = "Metropolis";  // 10 bytes

    // At 8 px/byte the whole thing is 80 px and fits a 100 px budget.
    REQUIRE(mbu::truncate_to_width(text, 8, 100.0f, measure_size_scaled)
            == text);
    // At 20 px/byte it is 200 px and must shrink to 5 bytes (100/20), three of
    // which are the ellipsis.
    REQUIRE(mbu::truncate_to_width(text, 20, 100.0f, measure_size_scaled)
            == "Me...");
}

// =====================================================================
// stable_tint_for_id
// =====================================================================

TEST_CASE("stable_tint_for_id is deterministic", "[mb_ui_utils][tint]") {
    for (int id : {0, 1, 550, 27205, 123456, -1}) {
        REQUIRE(same_color(mbu::stable_tint_for_id(id),
                           mbu::stable_tint_for_id(id)));
    }
}

TEST_CASE("stable_tint_for_id pins the exact hash output",
          "[mb_ui_utils][tint]") {
    // Derived by evaluating the pre-refactor formula (identical in all six
    // copies) by hand for these ids:
    //   h = uint32_t(id) * 2654435761u   (wrapping)
    //   r = 64 + ((h >>  0) & 0x7F)
    //   g = 40 + ((h >>  8) & 0x5F)
    //   b = 80 + ((h >> 16) & 0x7F)
    // These exist so that changing a constant, a shift, or a mask fails a
    // test instead of quietly recoloring every poster placeholder in the
    // kiosk -- the drift that made these six copies worth merging.
    struct Case { int id; uint8_t r, g, b; };
    const Case cases[] = {
        // id 0 is the additive floor: h == 0, so every channel is its base.
        {0,          64,  40,  80},
        {1,         113, 129, 135},   // h = 0x9E3779B1
        {550,       134, 122, 127},   // h = 0xEB2F7246
        {27205,     117,  62, 167},   // h = 0x9D5716B5
        // Negative ids are reinterpreted, not clamped: the cast to uint32_t
        // is modular. -1 -> 0xFFFFFFFF * 2654435761u = 0x61C8864F.
        {-1,        143,  46, 152},
    };
    for (const Case& c : cases) {
        const ::ui::Color got = mbu::stable_tint_for_id(c.id);
        INFO("id = " << c.id);
        CHECK(static_cast<int>(got.r) == static_cast<int>(c.r));
        CHECK(static_cast<int>(got.g) == static_cast<int>(c.g));
        CHECK(static_cast<int>(got.b) == static_cast<int>(c.b));
        CHECK(static_cast<int>(got.a) == 255);
    }
}

TEST_CASE("stable_tint_for_id stays inside the palette's channel ranges",
          "[mb_ui_utils][tint]") {
    // The bases + masks are chosen so the tint is always a mid-dark,
    // slightly purple-leaning placeholder -- never black, never a blown-out
    // white that would swallow the title text drawn on top of it.
    for (int id = -200; id <= 200; ++id) {
        const ::ui::Color c = mbu::stable_tint_for_id(id);
        CHECK(c.r >= 64);
        CHECK(c.r <= 64 + 0x7F);
        CHECK(c.g >= 40);
        CHECK(c.g <= 40 + 0x5F);
        CHECK(c.b >= 80);
        CHECK(c.b <= 80 + 0x7F);
        CHECK(c.a == 255);
    }
}

TEST_CASE("stable_tint_for_id spreads adjacent ids apart",
          "[mb_ui_utils][tint]") {
    // Consecutive tmdb ids land next to each other in a grid, so the hash
    // earns its keep only if neighbours look different. Not a claim of
    // injectivity (24 usable bits, so collisions exist) -- a claim that
    // sequential ids do not collide, which is the case that actually shows
    // up on screen.
    std::vector<uint32_t> packed;
    for (int id = 1000; id < 1064; ++id) {
        const ::ui::Color c = mbu::stable_tint_for_id(id);
        packed.push_back((static_cast<uint32_t>(c.r) << 16) |
                         (static_cast<uint32_t>(c.g) << 8) |
                         static_cast<uint32_t>(c.b));
    }
    for (size_t i = 1; i < packed.size(); ++i) {
        INFO("adjacent ids " << (1000 + i - 1) << " and " << (1000 + i));
        CHECK(packed[i] != packed[i - 1]);
    }
}

// =====================================================================
// easing
// =====================================================================

TEST_CASE("easing curves are pinned at both endpoints",
          "[mb_ui_utils][easing]") {
    // The slide-in animations drive panel X from closed to open by
    // multiplying a travel distance by these. An endpoint that is not
    // exactly 0/1 leaves the panel a fraction of a pixel off its resting
    // position, which reads as a shimmering edge.
    CHECK(mbu::ease_in_cubic(0.0f) == 0.0f);
    CHECK(mbu::ease_in_cubic(1.0f) == 1.0f);
    CHECK(mbu::ease_out_cubic(0.0f) == 0.0f);
    CHECK(mbu::ease_out_cubic(1.0f) == 1.0f);
}

TEST_CASE("easing curves are monotonic across the unit interval",
          "[mb_ui_utils][easing]") {
    float prev_in = mbu::ease_in_cubic(0.0f);
    float prev_out = mbu::ease_out_cubic(0.0f);
    for (int step = 1; step <= 100; ++step) {
        const float t = static_cast<float>(step) / 100.0f;
        const float in = mbu::ease_in_cubic(t);
        const float out = mbu::ease_out_cubic(t);
        INFO("t = " << t);
        CHECK(in >= prev_in);
        CHECK(out >= prev_out);
        prev_in = in;
        prev_out = out;
    }
}

TEST_CASE("ease_out_cubic is the mirror of ease_in_cubic",
          "[mb_ui_utils][easing]") {
    // ease_out(t) == 1 - ease_in(1 - t). Library and the filter overlay each
    // shipped their own pair; the mirror identity is what guarantees the
    // slide-out retraces the slide-in instead of using a subtly different
    // curve, which is visible as a "heavier" close than open.
    //
    // Asserted to a tolerance, not bit-exactly, and the reason is worth
    // knowing: `return 1.0f - u * u * u;` compiles to a FUSED multiply-add
    // (verified on arm64: fnmul + fmadd), so it rounds ONCE, while computing
    // u*u*u in a separate call and subtracting rounds twice. The two differ by
    // up to 1 ulp -- e.g. at t = 0.45, 0.83363 vs 0.83362. A margin of 1e-6 is
    // ~8 ulp at this magnitude: loose enough for the fusion, still far tighter
    // than any different curve could sneak through.
    for (int step = 0; step <= 100; ++step) {
        const float t = static_cast<float>(step) / 100.0f;
        INFO("t = " << t);
        CHECK(mbu::ease_out_cubic(t) ==
              Catch::Approx(1.0f - mbu::ease_in_cubic(1.0f - t))
                  .margin(1e-6));
    }
}

TEST_CASE("ease_in_cubic really is t^3", "[mb_ui_utils][easing]") {
    CHECK(mbu::ease_in_cubic(0.5f) == 0.125f);
    CHECK(mbu::ease_in_cubic(0.25f) == 0.015625f);
    CHECK(mbu::ease_out_cubic(0.5f) == 0.875f);
}

// =====================================================================
// format_bytes
// =====================================================================

TEST_CASE("format_bytes picks the right unit at each boundary",
          "[mb_ui_utils][bytes]") {
    // Boundaries are binary (1024), not decimal.
    CHECK(mbu::format_bytes(1023) == "1023 B");
    CHECK(mbu::format_bytes(1024) == "1.0 KB");

    CHECK(mbu::format_bytes(1024LL * 1024 - 1) == "1024 KB");
    CHECK(mbu::format_bytes(1024LL * 1024) == "1.0 MB");

    CHECK(mbu::format_bytes(1024LL * 1024 * 1024 - 1) == "1024 MB");
    CHECK(mbu::format_bytes(1024LL * 1024 * 1024) == "1.0 GB");
}

TEST_CASE("format_bytes drops the decimal at 100 of the chosen unit",
          "[mb_ui_utils][bytes]") {
    // The one intentional behavior change in this refactor. QueueScreen and
    // ReleasePickerScreen switched %.1f -> %.0f at 100; LibraryScreen always
    // printed one decimal. The 2-of-3 majority wins, so LibraryScreen's
    // storage readout now says "500 MB" where it used to say "500.0 MB".
    // Pinned here so nobody "tidies" it back.
    CHECK(mbu::format_bytes(static_cast<int64_t>(99.9 * 1024 * 1024))
          == "99.9 MB");
    CHECK(mbu::format_bytes(100LL * 1024 * 1024) == "100 MB");
    CHECK(mbu::format_bytes(500LL * 1024 * 1024) == "500 MB");

    // Same transition in every other unit.
    CHECK(mbu::format_bytes(99LL * 1024) == "99.0 KB");
    CHECK(mbu::format_bytes(100LL * 1024) == "100 KB");
    CHECK(mbu::format_bytes(99) == "99.0 B");
    CHECK(mbu::format_bytes(100) == "100 B");
}

TEST_CASE("format_bytes renders realistic movie and disk sizes",
          "[mb_ui_utils][bytes]") {
    // A 1080p x264 grab -- the size QueueScreen shows on every download row.
    CHECK(mbu::format_bytes(2400LL * 1024 * 1024) == "2.3 GB");
    // A library big enough to exercise the >= 100 GB path on the movie SSD.
    CHECK(mbu::format_bytes(175LL * 1024 * 1024 * 1024) == "175 GB");
    CHECK(mbu::format_bytes(1LL * 1024 * 1024 * 1024 * 1024) == "1024 GB");
}

TEST_CASE("format_bytes is defined but not meaningful at or below zero",
          "[mb_ui_utils][bytes]") {
    // The shared helper deliberately does NOT own the <= 0 case: callers
    // disagree about what it MEANS. ReleasePicker renders "?" (release size
    // unknown), Queue renders "0 B" (genuinely nothing transferred yet), and
    // Library renders "0 B" for an empty library. Collapsing those would
    // destroy information, so each call site keeps its own guard and this
    // function only promises a defined, non-crashing result -- never UB, and
    // never a value a caller is expected to display.
    CHECK(mbu::format_bytes(0) == "0 B");
    CHECK(mbu::format_bytes(-1) == "0 B");
    CHECK(mbu::format_bytes(-1024LL * 1024) == "0 B");
}
