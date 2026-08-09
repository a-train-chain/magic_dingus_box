// Tests for the ui::overlay spacing contract in ui/theme.h — the shared
// interior-padding constants and the pure card-sizing helpers every bordered
// overlay card converged onto in the 2026-08-09 padding audit.
//
// These are the numbers that keep text off card borders on BOTH logical
// canvases (Modern TV 1280x720, CRT Native 640x480), so each helper is
// checked at both widths. The CRT cases are load-bearing: the playback
// end-overlay is authored 640 wide — exactly the CRT canvas width — and the
// pairing screen has already shipped one overlap bug on the 480 canvas.

#include <catch2/catch_test_macros.hpp>

#include "ui/theme.h"

using namespace ui::overlay;

// =====================================================================
// clamped_card_w — cards keep kScreenEdgeMargin from the canvas edges
// =====================================================================

TEST_CASE("clamped_card_w is a no-op for every authored card on the 720p canvas") {
    // Every card in the app, widest first: playback end-overlay 640,
    // stall prompt 568, toast max growth, exit modal 448.
    CHECK(clamped_card_w(640, 1280) == 640);
    CHECK(clamped_card_w(568, 1280) == 568);
    CHECK(clamped_card_w(448, 1280) == 448);
}

TEST_CASE("clamped_card_w pulls the full-width end-overlay off the CRT canvas edges") {
    // Authored 640 wide == the CRT canvas width. Unclamped, the card sat
    // flush against both screen borders (cx = 0).
    CHECK(clamped_card_w(640, 640) == 640 - 2 * kScreenEdgeMargin);
    // Cards narrower than the clamp are untouched.
    CHECK(clamped_card_w(448, 640) == 448);
    // The stall prompt still fits with margin to spare.
    CHECK(clamped_card_w(568, 640) == 568);
}

// =====================================================================
// Toast panel sizing — grows to the text, never below the shipped look,
// never past the canvas margin
// =====================================================================

TEST_CASE("short toast keeps the long-shipped 480x80 panel exactly") {
    // "Playlists updated" at 24 px is ~200 px wide — well under minimum.
    CHECK(toast_panel_w(200, 1280) == kToastMinW);
    CHECK(toast_panel_h(1, 31) == kToastMinH);
}

TEST_CASE("long toast grows the panel to text plus the card pads") {
    // "Controller setup reset — using built-in mapping" measures ~560 px
    // at 24 px — wider than the old fixed 480 panel (the field report's
    // text-through-the-border case).
    CHECK(toast_panel_w(560, 1280) == 560 + 2 * kCardPadX);
}

TEST_CASE("toast panel clamps at the canvas edge margin on both canvases") {
    CHECK(toast_panel_w(5000, 1280) == 1280 - 2 * kScreenEdgeMargin);
    CHECK(toast_panel_w(5000, 640) == 640 - 2 * kScreenEdgeMargin);
    // A text run at exactly the wrap budget still fits inside the
    // clamped panel with full kCardPadX insets on both sides.
    CHECK(toast_max_line_w(640) + 2 * kCardPadX == toast_panel_w(5000, 640));
}

TEST_CASE("wrapped toast grows vertically with symmetric margins") {
    CHECK(toast_panel_h(2, 31) == 2 * 31 + 2 * kCardPadY);
    CHECK(toast_panel_h(3, 31) == 3 * 31 + 2 * kCardPadY);
    // Two 31 px lines + 2x26 pads = 114 > the 80 px minimum, so the
    // minimum never truncates a wrapped block.
    CHECK(toast_panel_h(2, 31) > kToastMinH);
}

TEST_CASE("toast wrap budget stays positive and sane on the CRT canvas") {
    // 640 - 2*24 (edge) - 2*32 (pads) = 528 px of usable line width.
    CHECK(toast_max_line_w(640) == 528);
    CHECK(toast_max_line_w(1280) == 1168);
}

// =====================================================================
// Contract sanity — the constants themselves
// =====================================================================

TEST_CASE("overlay spacing constants preserve the audited relationships") {
    // The modal standard matches the playback end-overlay's shipped
    // 32/26 look (the other modals grew to meet it, not vice versa).
    CHECK(kCardPadX == 32);
    CHECK(kCardPadY == 26);
    // Banner pads clear the minimum glyph-to-border floor.
    CHECK(static_cast<float>(kBannerPadY) >= kMinTextInset);
    CHECK(static_cast<float>(kBannerPadX) >= kMinTextInset);
    // Card pads dominate banner pads (dialogs breathe more than pills).
    CHECK(kCardPadX > kBannerPadX);
    CHECK(kCardPadY > kBannerPadY);
}
