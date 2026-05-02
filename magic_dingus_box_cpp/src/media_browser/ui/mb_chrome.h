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

#include "ui/theme.h"  // for ui::Color (passed by const& to draw_poster_card)

namespace ui { class Renderer; }

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

// "BAD RELEASE" red-bordered chip — shown when the queue item sits in
// trackedDownloadState=importBlocked (Radarr-side: the torrent finished
// but didn't contain a usable video file — usually a "movie" pack that
// turned out to be only trailers). Surfaces the stuck state directly
// to the user instead of misleadingly saying "DOWNLOADING" forever.
ChipRect draw_stuck_badge(::ui::Renderer& r, int x, int y);

// ---------- Composite chrome ----------

// Bordered-key footer hint, e.g. `[A] Open`. Returns the total width
// the hint consumed (key box + space + label + trailing margin).
//
// Retained for legacy / non-Marquee callers. Marquee screens should
// use the icon-based draw_footer_hints helper below instead.
int draw_keyhint(::ui::Renderer& r, int x, int y_baseline,
                 const std::string& key,
                 const std::string& action);

// ---- Footer hints (icon-based, v1.6.4+) ----
//
// Every Marquee screen surfaces ALL 6 inputs at the bottom of the
// frame so the operator never has to remember which buttons matter
// here. Inputs that have no meaning on the current screen render
// dimmed (icon at ~30% alpha + dim "—" label) so the absence reads
// as "this button does nothing here," not "I forgot to put it in."
//
// The icons are rendered as small square swatches in the BTN's own
// LED color (yellow / red / green / black-with-blue-ring) and a
// gray knob glyph for the rotary, so a glance at the footer mirrors
// what the operator's hands are looking at on the cabinet.
enum class HintIcon {
    Btn1Yellow,    // BTN1 — yellow square (gold accent stand-in for now)
    Btn2Red,       // BTN2 — red square (theme.highlight2)
    Btn3Green,     // BTN3 — green square (theme.highlight1)
    Btn4Black,     // BTN4 — black square + thin steel-blue ring
    RotaryNav,     // Rotary twist — gray octagon (≈ knob) with a notch
    RotaryPress,   // Rotary press — gray knob with a small center dot
};

struct Hint {
    HintIcon icon;
    std::string action;  // Short label, e.g. "Tab ←", "Back", or "—" for no-op.
};

// Draws a row of input-icon hints at the screen's footer band, left-
// aligned starting at `kSafeInset_px` from the screen's left edge.
// Hints with action == "—" (or empty) render the icon at ~30% alpha
// and the label dimmed, so the row tells the user at a glance which
// inputs are live for this screen.
void draw_footer_hints(::ui::Renderer& r,
                       int screen_w, int screen_h,
                       const std::vector<Hint>& hints);

// Same icon-based row, but anchored at an explicit (x, y_baseline)
// instead of the screen-wide footer band — used by the LibraryScreen
// slide-in overlay so the in-panel hint matches the chrome footer's
// visual style without leaking outside the panel's clip rect.
void draw_hint_row(::ui::Renderer& r, int x, int y_baseline,
                   const std::vector<Hint>& hints);

// Bordered action button — the design's `.btn` component. Mono 18 px
// label centered inside a 2 px bordered rect; border + label color
// derive from `kind`. Fill stays at theme.bg (no fill block) so the
// button reads as a chrome accent rather than a solid block. Focused
// buttons get the standard 2 px gold focus ring at +2 px offset.
//
// Sizing: caller provides x/y; the function measures the label, returns
// the button's full rect (so callers can lay out a row of buttons by
// chaining ButtonRect::w + a gap). Padding is 10 px vertical, 18 px
// horizontal — matches the design's .btn style block.
enum class ButtonKind {
    Neutral,  // dim border + fg label — fallback / "More info" style
    Ok,       // success (green) — Play, Add to Library
    Warn,     // hot (red) — Remove, Cancel
    Action,   // action (steel blue) — Search Releases, links
};

struct ButtonRect { int x, y, w, h; };

ButtonRect draw_button(::ui::Renderer& r, int x, int y,
                       const std::string& label,
                       ButtonKind kind = ButtonKind::Neutral,
                       bool focused = false);

// Renders a styled poster "card" matching the Marquee design's poster
// tile: solid colored fill (the tint), top + bottom dash accents in a
// lighter shade, the movie title rendered LARGE inside the card with
// a small year at the bottom-left. Optional IN LIBRARY badge (top-
// left) and downloading-progress badge are drawn over the card.
//
// This replaces the plain `mb_draw_poster_or_tint` placeholder with
// something that reads as a designed object even before the real TMDB
// poster art has loaded — at 9-col density the title-on-tint card is
// what you see for ~1 second per movie on cold load.
//
// Caller still manages focus_ring drawing externally if needed (so it
// can wrap the card OR an outer cell that includes the meta line).
//
// `poster_url` (optional) — when non-empty, the card renders the real
// TMDB poster image via mb_draw_poster_or_tint, falling back to the
// solid `tint` if the image hasn't loaded yet. When empty, the card
// just draws the tint (back-compat). The artwork-cache is keyed by URL,
// so the same URL on Browse / Search / Detail / Library all share one
// fetched texture (no double download).
void draw_poster_card(::ui::Renderer& r, int x, int y, int w, int h,
                      const std::string& title,
                      int year,
                      const ::ui::Color& tint,
                      bool in_library,
                      int download_pct,             // -1 = not downloading
                      const std::string& poster_url = "",
                      bool is_stuck = false);       // true => render BAD RELEASE
                                                    // badge instead of DOWNLOADING

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
