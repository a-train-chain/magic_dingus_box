#pragma once

#include <cstdint>
#include <string>

namespace ui {

struct Color {
    uint8_t r, g, b, a;
    
    Color() : r(0), g(0), b(0), a(255) {}
    Color(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) : r(r), g(g), b(b), a(a) {}
};

// ---------------------------------------------------------------------------
// Overlay-card interior spacing contract (2026-08-09 padding audit).
//
// Every bordered/scrimmed card the kiosk draws — modals, toasts, transient
// banners, the loading plate — takes its text insets from these named
// constants instead of per-site magic numbers. Values are authored against
// the 720-tall logical canvas (the same convention as every other layout
// constant in the app; the logical canvas is 1280x720 in Modern TV and
// 640x480 in CRT Native, so fixed-px insets scale visually with the mode
// exactly like the fonts do).
//
// The rules the field report earned:
//   * Text never touches a border: kMinTextInset is the absolute floor
//     between any glyph edge and a card border, all four sides.
//   * Multi-line blocks get symmetric top/bottom margins (kCardPadY).
//   * A card grows (or its text wraps/truncates) to honor the padding —
//     the container accommodates the text, never the reverse.
//   * Cards keep kScreenEdgeMargin from the canvas edge; on the 480
//     canvas this is what stops a 720p-sized card from sitting flush
//     against (or past) the screen border — clamp with clamped_card_w().
namespace overlay {

// Modal/dialog cards (exit modal, stall prompt, playback end-overlay).
inline constexpr int kCardPadX = 32;   // text inset from left/right borders
inline constexpr int kCardPadY = 26;   // first-line top / last-line bottom margin

// Compact pill banners (Detail/MB-Settings transient + inline status pills).
inline constexpr int kBannerPadX = 18; // banner horizontal text inset
inline constexpr int kBannerPadY = 12; // banner vertical text inset

// Gap between stacked text blocks inside one card.
inline constexpr int kTextGapY = 8;

// Minimum gap between a card and the canvas edge.
inline constexpr int kScreenEdgeMargin = 24;

// Absolute floor between any glyph and a card border (all four sides).
inline constexpr float kMinTextInset = 12.0f;

// Clamp an authored card width so the card keeps kScreenEdgeMargin from
// both canvas edges. On the 720p canvas this is a no-op for every card in
// the app; on the CRT 640-wide canvas it stops full-width cards (the
// playback end-overlay is authored 640 wide) from touching the bezel.
constexpr int clamped_card_w(int desired_w, int canvas_w) {
    const int max_w = canvas_w - 2 * kScreenEdgeMargin;
    return desired_w < max_w ? desired_w : max_w;
}

// Toast panel sizing: grows to fit the longest wrapped line plus card
// padding, never shrinks below the long-shipped 480x80 single-line look,
// never exceeds the canvas minus kScreenEdgeMargin per side.
inline constexpr int kToastMinW = 480;
inline constexpr int kToastMinH = 80;

constexpr int toast_panel_w(int longest_line_w, int canvas_w) {
    int w = longest_line_w + 2 * kCardPadX;
    if (w < kToastMinW) w = kToastMinW;
    const int max_w = canvas_w - 2 * kScreenEdgeMargin;
    return w < max_w ? w : max_w;
}

constexpr int toast_panel_h(int n_lines, int line_h) {
    // One line keeps the shipped 80 px panel bit-for-bit (its implied
    // 28 px vertical insets already exceed kCardPadY); only a wrapped
    // block computes its height from the line count.
    if (n_lines <= 1) return kToastMinH;
    const int h = n_lines * line_h + 2 * kCardPadY;
    return h > kToastMinH ? h : kToastMinH;
}

// The widest a toast line may render before wrapping: panel max width
// minus the interior padding.
constexpr int toast_max_line_w(int canvas_w) {
    return canvas_w - 2 * kScreenEdgeMargin - 2 * kCardPadX;
}

}  // namespace overlay

class Theme {
public:
    Theme();
    
    // Colors (matching Python Theme class)
    Color bg;           // Background: #1F191F
    Color bg_lift;      // Off-bg fill (+10% lift): #2A232A. Introduced with the Marquee
                        // design system for focused-row backgrounds, progress-bar
                        // troughs, etc. The ONLY allowed off-bg fill — don't add
                        // ad-hoc midtones outside this token.
    Color fg;           // Text: #F2E4D9
    Color highlight1;   // Green: #66DD7A — alias `success` in Marquee tokens
    Color highlight2;   // Red/orange: #EA3A27 — alias `hot` in Marquee tokens
    Color highlight3;   // Gold: #F5BF42 — alias `accent` in Marquee tokens
    Color action;       // Steel blue: #5884B1 (only cool color allowed)
    Color accent;       // Alias for highlight3 (gold; focus, highlight)
    Color accent2;      // Alias for action
    Color dim;          // Dim text: ~60% of fg, #968B85
    
    // Font sizes (matching Python Theme)
    int font_title_size;
    int font_heading_size;
    int font_large_size;
    int font_medium_size;
    int font_small_size;
    
    // Layout constants (matching Python UIRenderer)
    int margin_x;
    int margin_x_bezel;
    int title_y;
    int header_y;
    int playlist_item_height;
    int footer_y;
    
    // Get font path (will be set by font_manager)
    std::string get_font_path() const { return font_path_; }
    void set_font_path(const std::string& path) { font_path_ = path; }

private:
    std::string font_path_;
};

} // namespace ui

