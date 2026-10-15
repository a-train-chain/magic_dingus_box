#pragma once

// Media Browser V2 screen dispatcher base class (Task 17).
//
// The dispatcher in main.cpp owns one instance of each concrete screen
// (Browse, Search, Detail, Queue, Library, MovieSettings). The active
// screen's handle_input() returns a Screen enum value telling the
// dispatcher whether to stay, transition to a sibling, or exit back to
// the kiosk main menu.
//
// Tasks 18-23 replace each stub screen's render() with real UI; the base
// class and dispatcher wiring stay stable.

#include <cstdint>
#include <vector>
#include "platform/input_manager.h"

namespace ui { class Renderer; }
namespace media_browser { class RadarrClient; }

namespace media_browser::ui {

enum class Screen {
    Browse,
    Search,
    Detail,
    SeriesDetail,    // TV series detail (Phase 2c-2). Radarr-free mirror
                     // of Detail; reached only from Browse in TV mode.
    ReleasePicker,   // Manual release-grab override of Radarr's auto-pick (v1.7.0).
    Queue,
    Library,
    Playback,        // Ad-hoc movie playback inside the Media Browser.
    MovieSettings,
    Exit   // Return to kiosk main menu (AppScreen::MainMenu).
};

class MbScreen {
public:
    virtual ~MbScreen() = default;

    // Called by the dispatcher when this screen becomes active. Default no-op.
    virtual void enter() {}

    // Called by the dispatcher when this screen is about to be replaced or
    // exited. Default no-op.
    virtual void leave() {}

    // Process a frame of input events. Return the screen to transition to
    // (Screen::Exit to return to the kiosk main menu), or this screen's own
    // enum value to stay put.
    virtual Screen handle_input(const std::vector<platform::InputEvent>& events) = 0;

    // Per-frame update hook (timers, animation, async result polling, etc.).
    // Default no-op.
    virtual void update() {}

    // Draw the screen. screen_w / screen_h are the current framebuffer
    // dimensions (may change if the display is resized).
    virtual void render(::ui::Renderer& r, int screen_w, int screen_h) = 0;

    // ── Redraw gate (app/redraw_gate.h) ──────────────────────────────
    // The main loop skips render/swap/flip on iterations where nothing on
    // screen can change. A screen opts in by returning false here — and
    // must then return true whenever ANYTHING time-based is visible
    // (spinner, loading dots, marquee/scrolling text, a timed fade or
    // countdown, a live-updating value) or a background result it would
    // otherwise only notice inside render() is pending. Default: draw
    // every vblank, exactly as before the gate.
    //
    // Consulted after update() each iteration; input already forces a
    // draw on the iteration it arrives, and the gate redraws at least
    // every 250 ms regardless (a missed dirty source costs latency, never
    // a frozen screen).
    virtual bool wants_continuous_redraw() const { return true; }

    // Hash of everything the screen draws that can change WITHOUT input
    // while wants_continuous_redraw() is false: async results landing
    // (catalogue pages, details, availability, posters), state set by
    // workers, ... A change draws exactly one frame. Only consulted when
    // the screen opted out of continuous drawing.
    virtual uint64_t redraw_signature() const { return 0; }
};

}  // namespace media_browser::ui
