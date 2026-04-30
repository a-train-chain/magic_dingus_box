#include "media_browser/ui/mb_chrome.h"

#include "ui/renderer.h"
#include "ui/theme.h"

#include <algorithm>
#include <cstdio>
#include <string>

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
constexpr int kTabHorizPad       = kPad3;       // Inside-tab horizontal padding
constexpr int kTabVertPad        = kPad2;       // Inside-tab vertical padding
constexpr int kTabGap            = kPad4;       // Gap between adjacent tabs
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

ChipRect draw_dl_badge(::ui::Renderer& r, int x, int y, int percent_0_to_99) {
    const auto& th = r.mb_theme();
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d%%", std::clamp(percent_0_to_99, 0, 99));
    const std::string label = buf;
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

void draw_footer_hints(::ui::Renderer& r,
                       int /*screen_w*/, int screen_h,
                       const std::vector<Hint>& hints) {
    const int x_start = kSafeInset_px;
    // Footer baseline sits ~10 px above the wood frame's inner edge.
    const int y_baseline = screen_h - kFrameInset_px - kPad3;
    int x = x_start;
    for (const auto& h : hints) {
        x += draw_keyhint(r, x, y_baseline, h.key, h.action);
    }
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
        case ButtonKind::Action:  color = th.action;     break;  // steel blue
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
// Poster card — colored tint + title overlay + accents + badges
// =====================================================================
void draw_poster_card(::ui::Renderer& r, int x, int y, int w, int h,
                      const std::string& title,
                      int year,
                      const ::ui::Color& tint,
                      bool in_library,
                      int download_pct) {
    const auto& th = r.mb_theme();

    // Solid tint fill — the dominant visual on the card.
    r.mb_fill_rect(static_cast<float>(x), static_cast<float>(y),
                   static_cast<float>(w), static_cast<float>(h),
                   tint);

    // Top dash accent (small horizontal line, ~30% of width, near the
    // top edge with breathing room). Color is a lightened version of
    // the tint approximated by the foreground cream — this matches the
    // design's "small light line at top of poster" idiom.
    const float dash_w = static_cast<float>(w) * 0.30f;
    const float dash_y = static_cast<float>(y) + 16.0f;
    const float dash_x = static_cast<float>(x) + 16.0f;
    r.mb_draw_line(dash_x, dash_y,
                   dash_x + dash_w, dash_y,
                   2.0f, th.fg, 0.7f);

    // Title overlay: ZenDots, ~24 px, drawn near the upper-third of the
    // card. Word-wrap to 2 lines max (split on space if title is long).
    constexpr int kTitleFontPx = 22;
    const int title_x = x + 16;
    int title_baseline = y + 16 + 18 + kTitleFontPx;  // dash_y + spacing + font

    // Crude word wrap: split title at the space nearest the midpoint if
    // it's longer than ~10 chars. Sufficient for the kiosk's poster
    // density; the renderer doesn't yet have a measure-and-wrap helper.
    std::string line1 = title;
    std::string line2;
    if (title.size() > 10) {
        size_t mid = title.find(' ', title.size() / 2);
        if (mid == std::string::npos) mid = title.rfind(' ');
        if (mid != std::string::npos) {
            line1 = title.substr(0, mid);
            line2 = title.substr(mid + 1);
        }
    }
    r.mb_draw_title_text(line1,
                         static_cast<float>(title_x),
                         static_cast<float>(title_baseline),
                         kTitleFontPx, th.fg);
    if (!line2.empty()) {
        title_baseline += kTitleFontPx + 4;
        r.mb_draw_title_text(line2,
                             static_cast<float>(title_x),
                             static_cast<float>(title_baseline),
                             kTitleFontPx, th.fg);
    }

    // Year (bottom-left small label).
    if (year > 0) {
        char yr_buf[8];
        std::snprintf(yr_buf, sizeof(yr_buf), "%d", year);
        const int year_baseline = y + h - 16;
        r.mb_draw_text(yr_buf,
                       static_cast<float>(x + 16),
                       static_cast<float>(year_baseline),
                       12, th.fg, 0.85f);
    }

    // Bottom dash accent (matches top dash, smaller, at bottom-right
    // corner). Visual breadcrumb that ties the card together.
    const float bottom_dash_w = static_cast<float>(w) * 0.25f;
    const float bottom_dash_y = static_cast<float>(y) + static_cast<float>(h) - 14.0f;
    const float bottom_dash_x = static_cast<float>(x) + static_cast<float>(w)
                              - 16.0f - bottom_dash_w;
    r.mb_draw_line(bottom_dash_x, bottom_dash_y,
                   bottom_dash_x + bottom_dash_w, bottom_dash_y,
                   2.0f, th.fg, 0.7f);

    // Status badges (only one shows at a time per design).
    if (download_pct >= 0) {
        draw_dl_badge(r, x + kPad1, y + kPad1, download_pct);
    } else if (in_library) {
        draw_lib_badge(r, x + kPad1, y + kPad1);
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

// Draw a single tab. Caller positions the rect; we draw label + state
// styling. Active tab uses fg color; inactive uses dim. Focused tab
// gets the gold focus ring around the tab's bounding box.
void draw_one_tab(::ui::Renderer& r,
                  int x, int y, int w, int h,
                  const TabSpec& tab,
                  bool focused) {
    const auto& th = r.mb_theme();
    const ::ui::Color text_color = (tab.state == TabState::Active) ? th.fg : th.dim;

    // Label: ZenDots, vertically centered in the tab box.
    const int label_w = r.mb_title_text_width(tab.label, kTabFontPx);
    const int label_x = x + (w - label_w) / 2;
    // Approx ZenDots baseline: top + (h - font_px)/2 + font_px - small_descent.
    const int label_y = y + (h + kTabFontPx) / 2 - 4;
    r.mb_draw_title_text(tab.label,
                         static_cast<float>(label_x),
                         static_cast<float>(label_y),
                         kTabFontPx, text_color);

    if (focused) {
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

}  // namespace media_browser::ui::chrome
