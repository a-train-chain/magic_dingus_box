#include "media_browser/ui/mb_chrome.h"

#include "media_browser/ui/mb_ui_utils.h"
#include "ui/renderer.h"
#include "ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace media_browser::ui {

// One-line forward into the tested core. See mb_chrome.h for why this overload
// exists and why it lives here rather than in mb_ui_utils.
std::string truncate_to_width(::ui::Renderer& r, const std::string& text,
                              int font_size, float max_w) {
    return truncate_to_width(text, font_size, max_w,
                             [&r](const std::string& s, int px) {
                                 return static_cast<float>(
                                     r.mb_text_width(s, px));
                             });
}

}  // namespace media_browser::ui

namespace media_browser::ui::chrome {

namespace {
// Layout constants used inside this translation unit only — kept private
// so screens depend on the public draw_* helpers, not the magic numbers.
constexpr int kKeyHintFontPx     = 14;
constexpr int kKeyHintBoxPadX    = 6;
constexpr int kKeyHintBoxPadY    = 1;
constexpr int kKeyHintGap_px     = kPad5;     // Gap between consecutive hints
constexpr int kKeyHintLabelGap   = 6;          // Gap between the [key] and its action label
constexpr int kBadgeFontPx       = 12;
constexpr int kBadgePadX         = 6;
constexpr int kBadgePadY         = 2;
constexpr int kTabFontPx         = 24;          // ZenDots section size
constexpr int kTabHorizPad       = 10;          // was kPad3 (16) — 7-chip strip fit
constexpr int kTabVertPad        = kPad2;       // Inside-tab vertical padding
constexpr int kTabGap            = 16;          // was kPad4 (24) — 7-chip strip fit
constexpr int kTitleFontPx       = 32;          // ZenDots screen title
}  // namespace

// =====================================================================
// Focus ring
// =====================================================================
void draw_focus_ring(::ui::Renderer& r, int x, int y, int w, int h) {
    const auto& th = r.mb_theme();
    const float fx = static_cast<float>(x - kFocusOffset_px);
    const float fy = static_cast<float>(y - kFocusOffset_px);
    const float fw = static_cast<float>(w + 2 * kFocusOffset_px);
    const float fh = static_cast<float>(h + 2 * kFocusOffset_px);
    r.mb_stroke_rect(fx, fy, fw, fh, static_cast<float>(kFocusBorder_px), th.accent);
}

// =====================================================================
// Badges (top-left of poster cells)
// =====================================================================
ChipRect draw_lib_badge(::ui::Renderer& r, int x, int y) {
    const auto& th = r.mb_theme();
    const std::string label = "IN LIBRARY";
    const int text_w = r.mb_text_width(label, kBadgeFontPx);
    const int box_w  = text_w + 2 * kBadgePadX;
    const int box_h  = kBadgeFontPx + 2 * kBadgePadY;
    // Solid bg fill so the chip stays readable over any poster art.
    r.mb_fill_rect(static_cast<float>(x), static_cast<float>(y),
                   static_cast<float>(box_w), static_cast<float>(box_h),
                   th.bg);
    // Green border = "in library" success state.
    r.mb_stroke_rect(static_cast<float>(x), static_cast<float>(y),
                     static_cast<float>(box_w), static_cast<float>(box_h),
                     static_cast<float>(kFocusBorder_px), th.highlight1);
    // Text baseline ≈ box_top + box_h - kBadgePadY - descent.
    // Approximate baseline at top + font_px (close enough for our font).
    r.mb_draw_text(label,
                   static_cast<float>(x + kBadgePadX),
                   static_cast<float>(y + kBadgePadY + kBadgeFontPx - 2),
                   kBadgeFontPx, th.highlight1);
    return {x, y, box_w, box_h};
}

ChipRect draw_dl_badge(::ui::Renderer& r, int x, int y,
                       [[maybe_unused]] int percent_0_to_99) {
    const auto& th = r.mb_theme();
    // percent_0_to_99 is retained in the signature so call sites don't need
    // to change, but we show a fixed label — reliable percentage data isn't
    // available from Radarr's queue endpoint.
    const std::string label = "DOWNLOADING";
    const int text_w = r.mb_text_width(label, kBadgeFontPx);
    const int box_w  = text_w + 2 * kBadgePadX;
    const int box_h  = kBadgeFontPx + 2 * kBadgePadY;
    r.mb_fill_rect(static_cast<float>(x), static_cast<float>(y),
                   static_cast<float>(box_w), static_cast<float>(box_h),
                   th.bg);
    r.mb_stroke_rect(static_cast<float>(x), static_cast<float>(y),
                     static_cast<float>(box_w), static_cast<float>(box_h),
                     static_cast<float>(kFocusBorder_px), th.highlight2);
    r.mb_draw_text(label,
                   static_cast<float>(x + kBadgePadX),
                   static_cast<float>(y + kBadgePadY + kBadgeFontPx - 2),
                   kBadgeFontPx, th.highlight2);
    return {x, y, box_w, box_h};
}

ChipRect draw_importing_badge(::ui::Renderer& r, int x, int y) {
    const auto& th = r.mb_theme();
    // "Download finished, Radarr is copying it into the library" — a brief,
    // benign in-progress state. Styled like DOWNLOADING (solid bg + colored
    // border/text) but in the accent/gold "armed" color, NOT the red
    // warning palette DOWNLOADING/BAD RELEASE use, so it reads as
    // imminent-success rather than a problem.
    const std::string label = "IMPORTING";
    const int text_w = r.mb_text_width(label, kBadgeFontPx);
    const int box_w  = text_w + 2 * kBadgePadX;
    const int box_h  = kBadgeFontPx + 2 * kBadgePadY;
    r.mb_fill_rect(static_cast<float>(x), static_cast<float>(y),
                   static_cast<float>(box_w), static_cast<float>(box_h),
                   th.bg);
    r.mb_stroke_rect(static_cast<float>(x), static_cast<float>(y),
                     static_cast<float>(box_w), static_cast<float>(box_h),
                     static_cast<float>(kFocusBorder_px), th.accent);
    r.mb_draw_text(label,
                   static_cast<float>(x + kBadgePadX),
                   static_cast<float>(y + kBadgePadY + kBadgeFontPx - 2),
                   kBadgeFontPx, th.accent);
    return {x, y, box_w, box_h};
}

ChipRect draw_stuck_badge(::ui::Renderer& r, int x, int y) {
    const auto& th = r.mb_theme();
    const std::string label = "BAD RELEASE";
    const int text_w = r.mb_text_width(label, kBadgeFontPx);
    const int box_w  = text_w + 2 * kBadgePadX;
    const int box_h  = kBadgeFontPx + 2 * kBadgePadY;
    // Fill the badge with the warning color so it pops harder than
    // the bordered DOWNLOADING chip — stuck items should jump out
    // at a glance.
    r.mb_fill_rect(static_cast<float>(x), static_cast<float>(y),
                   static_cast<float>(box_w), static_cast<float>(box_h),
                   th.highlight2);
    r.mb_stroke_rect(static_cast<float>(x), static_cast<float>(y),
                     static_cast<float>(box_w), static_cast<float>(box_h),
                     static_cast<float>(kFocusBorder_px), th.highlight2);
    r.mb_draw_text(label,
                   static_cast<float>(x + kBadgePadX),
                   static_cast<float>(y + kBadgePadY + kBadgeFontPx - 2),
                   kBadgeFontPx, th.bg);
    return {x, y, box_w, box_h};
}

// =====================================================================
// Footer keyhints
// =====================================================================
int draw_keyhint(::ui::Renderer& r, int x, int y_baseline,
                 const std::string& key,
                 const std::string& action) {
    const auto& th = r.mb_theme();

    // [KEY] box: bordered, white text inside.
    const int key_text_w = r.mb_text_width(key, kKeyHintFontPx);
    const int box_w = key_text_w + 2 * kKeyHintBoxPadX;
    const int box_h = kKeyHintFontPx + 2 * kKeyHintBoxPadY + 2;  // +2 for visual breathing
    const int box_y = y_baseline - kKeyHintFontPx - kKeyHintBoxPadY + 1;
    r.mb_stroke_rect(static_cast<float>(x), static_cast<float>(box_y),
                     static_cast<float>(box_w), static_cast<float>(box_h),
                     static_cast<float>(kFocusBorder_px), th.dim);
    r.mb_draw_text(key,
                   static_cast<float>(x + kKeyHintBoxPadX),
                   static_cast<float>(y_baseline),
                   kKeyHintFontPx, th.fg);

    // Action label, dim-colored.
    const int action_x = x + box_w + kKeyHintLabelGap;
    r.mb_draw_text(action,
                   static_cast<float>(action_x),
                   static_cast<float>(y_baseline),
                   kKeyHintFontPx, th.dim);
    const int action_w = r.mb_text_width(action, kKeyHintFontPx);
    return (action_x + action_w + kKeyHintGap_px) - x;
}

// =====================================================================
// Icon-based footer hints (v1.6.4+)
// =====================================================================
namespace {

constexpr int kHintIconSize     = 14;   // Square / glyph dimension
constexpr int kHintIconLabelGap = 6;    // Gap between icon and label
constexpr int kHintRowGap       = kPad4; // Gap between consecutive hints
constexpr int kHintFontPx       = 14;
// Steel-blue used for the BTN4 ring; theme.action is the canonical
// Marquee steel blue.
inline ::ui::Color btn4_ring_color(const ::ui::Theme& th) { return th.action; }

// Draws a small filled regular octagon centered at (cx, cy) with bounding
// dimension `size` (== diameter of inscribed circle). Used as a stand-in
// for a true filled circle since the renderer doesn't expose mb_fill_circle
// — visually indistinguishable at 14 px on the kiosk display.
void draw_filled_octagon(::ui::Renderer& r, float cx, float cy, float size,
                         const ::ui::Color& color, float alpha = 1.0f) {
    const float radius = size * 0.5f;
    // 8 points around a unit circle, at angles 22.5° + i*45° so the
    // octagon presents a flat top edge (looks more like a knob than
    // a diamond rotated 22.5°).
    constexpr int kSides = 8;
    constexpr float kPi = 3.14159265358979323846f;
    float px[kSides], py[kSides];
    for (int i = 0; i < kSides; ++i) {
        const float a = (kPi / kSides) + (2.0f * kPi * static_cast<float>(i)
                                          / static_cast<float>(kSides));
        px[i] = cx + radius * cosf(a);
        py[i] = cy + radius * sinf(a);
    }
    // Fan triangulation from vertex 0.
    for (int i = 1; i + 1 < kSides; ++i) {
        r.mb_fill_triangle(px[0], py[0],
                           px[i], py[i],
                           px[i + 1], py[i + 1],
                           color, alpha);
    }
}

// Draw one hint icon. (x, y) is the top-left of the icon's 14 px box.
// `dimmed` is true when the input is a no-op on the current screen —
// the icon then renders at ~30% alpha so the absence reads correctly.
void draw_hint_icon(::ui::Renderer& r, int x, int y, HintIcon icon, bool dimmed) {
    const auto& th = r.mb_theme();
    const float alpha = dimmed ? 0.30f : 1.0f;
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    const float fs = static_cast<float>(kHintIconSize);
    const float cx = fx + fs * 0.5f;
    const float cy = fy + fs * 0.5f;

    switch (icon) {
        case HintIcon::Btn1Yellow:
            // No literal yellow in the palette; gold (accent) is the
            // closest hue and matches the rest of the chrome's
            // gold-accent vocabulary.
            r.mb_fill_rect(fx, fy, fs, fs, th.accent, alpha);
            break;
        case HintIcon::Btn2Red:
            r.mb_fill_rect(fx, fy, fs, fs, th.highlight2, alpha);
            break;
        case HintIcon::Btn3Green:
            r.mb_fill_rect(fx, fy, fs, fs, th.highlight1, alpha);
            break;
        case HintIcon::Btn4Black:
            // Black face + 1 px steel-blue ring. The fill needs a
            // visible color for the dimmed state too, so we use bg
            // (matches the kiosk background) and rely on the ring
            // for shape.
            r.mb_fill_rect(fx, fy, fs, fs, th.bg, alpha);
            r.mb_stroke_rect(fx, fy, fs, fs, 1.0f, btn4_ring_color(th), alpha);
            break;
        case HintIcon::RotaryNav: {
            // Gray octagonal "knob" + a small notch at the top edge.
            draw_filled_octagon(r, cx, cy, fs, th.dim, alpha);
            // Notch: 2 px wide, 4 px tall vertical bar drawn in bg
            // from the top of the knob toward the center.
            const float notch_w = 2.0f;
            const float notch_h = 4.0f;
            r.mb_fill_rect(cx - notch_w * 0.5f,
                           fy + 1.0f,
                           notch_w, notch_h,
                           th.bg, alpha);
            break;
        }
        case HintIcon::RotaryPress: {
            // Same knob; center dot instead of a notch.
            draw_filled_octagon(r, cx, cy, fs, th.dim, alpha);
            const float dot = 3.0f;
            r.mb_fill_rect(cx - dot * 0.5f, cy - dot * 0.5f,
                           dot, dot, th.bg, alpha);
            break;
        }
    }
}

// Single icon-style hint: icon + label. Returns the consumed width.
int draw_one_icon_hint(::ui::Renderer& r, int x, int y_baseline,
                       const Hint& h) {
    const auto& th = r.mb_theme();
    const bool noop = h.action.empty() || h.action == "—";
    // Icon top so its vertical center sits roughly on the cap-height of
    // the label baseline. With 14 px font + 14 px icon, the icon's top
    // ≈ baseline - font_px + 1.
    const int icon_top = y_baseline - kHintFontPx + 1;
    draw_hint_icon(r, x, icon_top, h.icon, noop);
    const int label_x = x + kHintIconSize + kHintIconLabelGap;
    const ::ui::Color label_color = th.dim;
    const float label_alpha = noop ? 0.45f : 1.0f;
    r.mb_draw_text(h.action,
                   static_cast<float>(label_x),
                   static_cast<float>(y_baseline),
                   kHintFontPx, label_color, label_alpha);
    const int label_w = r.mb_text_width(h.action, kHintFontPx);
    return (label_x + label_w + kHintRowGap) - x;
}

}  // namespace

void draw_hint_row(::ui::Renderer& r, int x_start, int y_baseline,
                   const std::vector<Hint>& hints) {
    int x = x_start;
    for (const auto& h : hints) {
        x += draw_one_icon_hint(r, x, y_baseline, h);
    }
}

void draw_footer_hints(::ui::Renderer& r,
                       int /*screen_w*/, int screen_h,
                       const std::vector<Hint>& hints) {
    const int x_start = kSafeInset_px;
    // Footer baseline sits ~10 px above the wood frame's inner edge.
    const int y_baseline = screen_h - kFrameInset_px - kPad3;
    draw_hint_row(r, x_start, y_baseline, hints);
}

// =====================================================================
// Bordered action button — the design's .btn component
// =====================================================================
ButtonRect draw_button(::ui::Renderer& r, int x, int y,
                       const std::string& label,
                       ButtonKind kind,
                       bool focused) {
    constexpr int kBtnFontPx = 18;
    constexpr int kBtnPadX   = 18;
    constexpr int kBtnPadY   = 10;

    const auto& th = r.mb_theme();
    ::ui::Color color;
    switch (kind) {
        case ButtonKind::Ok:      color = th.highlight1; break;  // green
        case ButtonKind::Warn:    color = th.highlight2; break;  // red
        case ButtonKind::Action:  color = th.dim;     break;  // steel blue
        case ButtonKind::Neutral:
        default:                  color = th.dim;        break;
    }

    const int tw = r.mb_text_width(label, kBtnFontPx);
    const int w  = tw + 2 * kBtnPadX;
    const int h  = kBtnFontPx + 2 * kBtnPadY;

    // Background: solid theme bg (matches the design's .btn fill).
    r.mb_fill_rect(static_cast<float>(x), static_cast<float>(y),
                   static_cast<float>(w), static_cast<float>(h),
                   th.bg);
    // Border: 2 px in the kind color.
    r.mb_stroke_rect(static_cast<float>(x), static_cast<float>(y),
                     static_cast<float>(w), static_cast<float>(h),
                     static_cast<float>(kFocusBorder_px), color);
    // Label: matching color, vertically centered.
    const int label_x = x + kBtnPadX;
    const int label_baseline = y + kBtnPadY + kBtnFontPx - 2;
    r.mb_draw_text(label,
                   static_cast<float>(label_x),
                   static_cast<float>(label_baseline),
                   kBtnFontPx, color);

    if (focused) {
        draw_focus_ring(r, x, y, w, h);
    }

    return {x, y, w, h};
}

// =====================================================================
// Poster card — colored tint background + status badges + year pill
// =====================================================================
// Renders a poster cell with minimal in-card chrome so real TMDB
// artwork (when loaded later) isn't visually obscured by overlay
// elements that don't fit. Only three things draw on top of the tint:
//
//   1. IN LIBRARY badge (top-left, green border)  — shows for owned
//      movies. Mutually exclusive with #2.
//   2. Download-progress badge (top-left, red border, "62%") — shows
//      for movies actively downloading.
//   3. Year pill (bottom-right) — small year label on a ~70%-opaque
//      dark-bg backing rect so it stays readable regardless of the
//      poster's underlying color (a full-bleed white poster like a
//      faded "Drive" sleeve was previously eating the year text).
//
// The title is NOT rendered inside the card. Caller is responsible
// for drawing the title BELOW the card in the cell's meta area —
// this keeps the card itself uncluttered when real artwork loads.
void draw_poster_card(::ui::Renderer& r, int x, int y, int w, int h,
                      const std::string& /*title*/,
                      int year,
                      const ::ui::Color& tint,
                      bool in_library,
                      int download_pct,
                      const std::string& poster_url,
                      bool is_stuck,
                      bool is_importing) {
    const auto& th = r.mb_theme();

    // Background — real TMDB image if `poster_url` is set (with tint as
    // fallback while it's still loading from the artwork cache), or a
    // solid tint fill if the caller has no URL. mb_draw_poster_or_tint
    // is idempotent and side-effect-only-once: calling it every frame
    // for the same URL is cheap, and the first call kicks off a
    // background fetch that completes asynchronously.
    if (!poster_url.empty()) {
        r.mb_draw_poster_or_tint(poster_url,
                                 static_cast<float>(x), static_cast<float>(y),
                                 static_cast<float>(w), static_cast<float>(h),
                                 tint, 1.0f);
    } else {
        r.mb_fill_rect(static_cast<float>(x), static_cast<float>(y),
                       static_cast<float>(w), static_cast<float>(h),
                       tint);
    }

    // Status badges (only one shows at a time per design).
    // Precedence: stuck/bad-release > importing > downloading > in-library.
    // A stuck item is technically also "in library" from Radarr's POV, but
    // the user needs to see the failure state. Importing sits above
    // downloading because it's the later, more specific stage (the file
    // finished downloading and is being copied into the library).
    if (is_stuck) {
        draw_stuck_badge(r, x + kPad1, y + kPad1);
    } else if (is_importing) {
        draw_importing_badge(r, x + kPad1, y + kPad1);
    } else if (download_pct >= 0) {
        draw_dl_badge(r, x + kPad1, y + kPad1, download_pct);
    } else if (in_library) {
        draw_lib_badge(r, x + kPad1, y + kPad1);
    }

    // Year pill — bottom-right, with a semi-transparent dark backing
    // rect so the year stays legible on any underlying poster color
    // (white sleeves, bright tints, or photographic backdrops once
    // real artwork loads in this slot).
    if (year > 0) {
        constexpr int kYearFontPx = 12;
        constexpr int kYearPadX   = 6;
        constexpr int kYearPadY   = 3;
        char yr_buf[8];
        std::snprintf(yr_buf, sizeof(yr_buf), "%d", year);
        const std::string yr = yr_buf;
        const int text_w = r.mb_text_width(yr, kYearFontPx);
        const int box_w = text_w + 2 * kYearPadX;
        const int box_h = kYearFontPx + 2 * kYearPadY;
        const int box_x = x + w - kPad2 - box_w;
        const int box_y = y + h - kPad2 - box_h;
        // 70%-opaque theme.bg behind the year — dark plum-black,
        // consistent with the rest of the chrome's color language.
        r.mb_fill_rect(static_cast<float>(box_x), static_cast<float>(box_y),
                       static_cast<float>(box_w), static_cast<float>(box_h),
                       th.bg, 0.70f);
        // Year text itself: cream fg at near-full opacity.
        r.mb_draw_text(yr,
                       static_cast<float>(box_x + kYearPadX),
                       static_cast<float>(box_y + kYearPadY + kYearFontPx - 1),
                       kYearFontPx, th.fg, 0.95f);
    }
}

// =====================================================================
// Header (title + tab strip)
// =====================================================================
namespace {
// Compute total width of a tab-strip layout so we can right-align it.
int tab_strip_total_width(::ui::Renderer& r, const std::vector<TabSpec>& tabs) {
    int total = 0;
    for (size_t i = 0; i < tabs.size(); ++i) {
        const int label_w = r.mb_title_text_width(tabs[i].label, kTabFontPx);
        total += label_w + 2 * kTabHorizPad;
        if (i + 1 < tabs.size()) total += kTabGap;
    }
    return total;
}

// Draw a single tab. Caller positions the rect; we draw the active /
// inactive state plus the optional focus ring.
//
// Active tab visual (per the Marquee design):
//   - 2 px gold (accent) border drawn around the full tab bounding box
//     — the "you are here" indicator the user sees at a glance.
//   - Tab label rendered in gold (accent) too, so the active section
//     reads as one cohesive gold-on-bg unit rather than a bordered
//     block with text in a different color.
//
// Inactive tab: dim label, no border.
//
// Focused tab (only meaningful if tab navigation has its own focus
// state — Marquee's BTN1/BTN3 grammar makes activation = focus, so
// `focused` is effectively unused on Marquee screens but we keep the
// focus-ring path for any future screen that wants the distinction).
void draw_one_tab(::ui::Renderer& r,
                  int x, int y, int w, int h,
                  const TabSpec& tab,
                  bool focused) {
    const auto& th = r.mb_theme();
    const bool is_active = (tab.state == TabState::Active);
    const ::ui::Color text_color = is_active ? th.accent : th.dim;

    // Active: gold border around the tab box.
    if (is_active) {
        r.mb_stroke_rect(static_cast<float>(x), static_cast<float>(y),
                         static_cast<float>(w), static_cast<float>(h),
                         static_cast<float>(kFocusBorder_px), th.accent);
    }

    // Label: ZenDots, vertically centered in the tab box. Same family
    // the kiosk's main playlist UI uses for its title and section
    // headers, so transitioning between Browse/Library and the home
    // menu doesn't look like two different products.
    const int label_w = r.mb_title_text_width(tab.label, kTabFontPx);
    const int label_x = x + (w - label_w) / 2;
    // Approx ZenDots baseline: top + (h - font_px)/2 + font_px - small_descent.
    const int label_y = y + (h + kTabFontPx) / 2 - 4;
    r.mb_draw_title_text(tab.label,
                         static_cast<float>(label_x),
                         static_cast<float>(label_y),
                         kTabFontPx, text_color);

    if (focused && !is_active) {
        // Focused-but-not-active is rare in Marquee (BTN1/3 promotes
        // immediately) but we still draw a ring for completeness.
        draw_focus_ring(r, x, y, w, h);
    }
}
}  // namespace

int draw_screen_header(::ui::Renderer& r,
                       int screen_w,
                       const std::string& title,
                       const std::vector<TabSpec>& tabs,
                       int focused_tab_index,
                       const std::string& sub_info) {
    const auto& th = r.mb_theme();
    const int header_top = kSafeInset_px;
    const int header_h   = kHeaderHeight_px;

    // --- Title (left) ---
    const int title_x = kSafeInset_px;
    // ZenDots baseline approximation — tweak to match the title in the
    // playlist UI which already lives at the same vertical position.
    const int title_baseline = header_top + kTitleFontPx - 4;
    if (!title.empty()) {
        r.mb_draw_title_text(title,
                             static_cast<float>(title_x),
                             static_cast<float>(title_baseline),
                             kTitleFontPx, th.fg);
    }

    // --- Tab strip (right) OR sub-info text ---
    if (!tabs.empty()) {
        const int strip_w = tab_strip_total_width(r, tabs);
        // Right-align inside the safe area.
        const int strip_right = screen_w - kSafeInset_px;
        int x = strip_right - strip_w;
        // Vertically center each tab on the header band so it lines up
        // with the title's optical center.
        const int tab_h = kTabFontPx + 2 * kTabVertPad;
        const int tab_y = header_top + (header_h - tab_h) / 2 - 2;
        for (size_t i = 0; i < tabs.size(); ++i) {
            const int label_w = r.mb_title_text_width(tabs[i].label, kTabFontPx);
            const int tab_w   = label_w + 2 * kTabHorizPad;
            const bool focused = (static_cast<int>(i) == focused_tab_index);
            draw_one_tab(r, x, tab_y, tab_w, tab_h, tabs[i], focused);
            x += tab_w + kTabGap;
        }
    } else if (!sub_info.empty()) {
        // Right-aligned monospace info string (used by Queue / Detail).
        constexpr int sub_font = 14;
        const int sw = r.mb_text_width(sub_info, sub_font);
        const int sx = screen_w - kSafeInset_px - sw;
        const int sy = header_top + kTitleFontPx - 8;
        r.mb_draw_text(sub_info,
                       static_cast<float>(sx),
                       static_cast<float>(sy),
                       sub_font, th.dim);
    }

    // Return y-coord where content can start drawing below the header band.
    return header_top + header_h;
}

const std::vector<std::string>& marquee_tab_labels() {
    // Display order, left to right. Must stay in sync with BrowseScreen's
    // kVisibleTabs (which maps these to Category values for input
    // dispatch) — that array is the navigation source of truth, this is
    // the rendering one, and they describe the same strip.
    static const std::vector<std::string> kLabels = {
        "Popular", "Top Rated", "For You", "Search", "Library", "Queue", "Settings",
    };
    return kLabels;
}

std::vector<TabSpec> marquee_tabs(const std::string& active_label) {
    const auto& labels = marquee_tab_labels();
    std::vector<TabSpec> tabs;
    tabs.reserve(labels.size());
    for (const auto& label : labels) {
        TabSpec t;
        t.label = label;
        t.state = (label == active_label) ? TabState::Active : TabState::Inactive;
        tabs.push_back(std::move(t));
    }
    return tabs;
}

}  // namespace media_browser::ui::chrome
