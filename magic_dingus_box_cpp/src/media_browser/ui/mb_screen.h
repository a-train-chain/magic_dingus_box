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

#include <vector>
#include "platform/input_manager.h"

namespace ui { class Renderer; }
namespace media_browser { class RadarrClient; }

namespace media_browser::ui {

enum class Screen {
    Browse,
    Search,
    Detail,
    Queue,
    Library,
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
};

}  // namespace media_browser::ui
