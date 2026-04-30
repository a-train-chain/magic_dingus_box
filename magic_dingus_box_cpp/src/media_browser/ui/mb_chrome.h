#pragma once

// Shared visual primitives for Marquee (Media Browser) screens.
//
// Each Marquee screen (Browse, Library, Detail, Search, Queue, Playback)
// composes its layout from these helpers so visual cohesion stays
// automatic. The implementations target the renderer's immediate-mode
// 2D primitives — solid quads, textured quads, bitmap text, scissor
// rects — so they map 1:1 onto what the OpenGL ES backend can draw
// without any new shader work.
//
// Constants are in pixels at the kiosk's native 1280×720 resolution.
// The wood-frame asset (Phase 1) draws a ~40 px decorative cabinet at
// the screen edges; mb_chrome::kFrameInset_px reflects that, and
// mb_chrome::kSafeInset_px adds breathing room before content starts.

#include <cstdint>
#include <string>
#include <vector>

namespace ui { class Renderer; class Theme; }

namespace media_browser::ui::chrome {

// ---------- Layout constants ----------

// Wood-frame thickness baked into the marquee_frame.png asset.
// Content should never sit ON TOP of these pixels — they belong to
// the cabinet art.
inline constexpr int kFrameInset_px = 40;

// Total inset from screen edges to content's top-left. The 20 px gap
// between the frame's inner edge and content gives the eye breathing
// room and keeps the focus ring (2 px gold + 2 px offset) from
// touching the wood.
inline constexpr int kSafeInset_px = 60;

// Header band height (title + tab strip share this).
inline constexpr int kHeaderHeight_px = 60;

// Footer hints band height.
inline constexpr int kFooterHeight_px = 30;

// Standard focus-ring stroke + offset.
inline constexpr int kFocusBorder_px = 2;
inline constexpr int kFocusOffset_px = 2;

// Spacing scale (use these — don't invent new values).
inline constexpr int kPad1 = 4;
inline constexpr int kPad2 = 8;
inline constexpr int kPad3 = 16;
inline constexpr int kPad4 = 24;
inline constexpr int kPad5 = 40;

// ---------- Tab states ----------

enum class TabState {
    Inactive,  // Other tab is the active one
    Active,    // This is the currently-active tab (its content is showing)
};

// One tab descriptor for draw_tab_strip().
struct TabSpec {
    std::string label;
    TabState    state = TabState::Inactive;
};

// ---------- Drawing primitives ----------

// 2 px gold outline drawn `kFocusOffset_px` outside the given rect.
// Use to mark the keyboard-focused element on any Marquee screen.
void draw_focus_ring(::ui::Renderer& r, int x, int y, int w, int h);

// "IN LIBRARY" green-bordered chip overlay. Drawn at the top-left of
// a poster cell. Returns the rect drawn (so the caller knows the
// vertical space consumed if more chips need to stack).
struct ChipRect { int x, y, w, h; };
ChipRect draw_lib_badge(::ui::Renderer& r, int x, int y);

// "62%" red-bordered downloading-progress chip. Same anchor as the
// IN LIBRARY badge — only one shows at a time per poster.
ChipRect draw_dl_badge(::ui::Renderer& r, int x, int y, int percent_0_to_99);

// ---------- Composite chrome ----------

// Bordered-key footer hint, e.g. `[A] Open`. Returns the total width
// the hint consumed (key box + space + label + trailing margin).
int draw_keyhint(::ui::Renderer& r, int x, int y_baseline,
                 const std::string& key,
                 const std::string& action);

// Convenience: draws a row of keyhints at the screen's footer band,
// left-aligned starting at `kSafeInset_px` from the screen's left edge.
struct Hint { std::string key; std::string action; };
void draw_footer_hints(::ui::Renderer& r,
                       int screen_w, int screen_h,
                       const std::vector<Hint>& hints);

// Top-of-screen header: a screen title on the left, an N-tab strip on
// the right. `focused_on_tabs` toggles whether the active tab gets the
// focus ring (true when the user is navigating tabs with BTN1/BTN3 or
// D-pad LEFT/RIGHT) or just the active-state styling without focus.
//
// Returns the y-coordinate where content can start drawing below the
// header (i.e., the bottom of the header band).
int draw_screen_header(::ui::Renderer& r,
                       int screen_w,
                       const std::string& title,
                       const std::vector<TabSpec>& tabs,
                       int focused_tab_index,   // -1 = no tab focused
                       const std::string& sub_info = "");  // Optional right-side info text

}  // namespace media_browser::ui::chrome
