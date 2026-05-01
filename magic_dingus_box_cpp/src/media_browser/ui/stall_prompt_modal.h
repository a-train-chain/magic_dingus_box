#pragma once

// StallPromptModal — non-blocking prompt shown when DownloadWatchdog
// detects a stalled download. Two actions: "Pick another" (opens
// ReleasePickerScreen for the stalled movie) and "Dismiss" (snoozes
// the watchdog for that movie for 10 minutes).
//
// Lifecycle: main.cpp owns one instance; the watchdog tick() loop
// surfaces stall events into show(); during render() the active modal
// overlays the current screen; handle_input() consumes input events
// while active and emits the user's choice via the registered handlers.
//
// Visual / interaction conventions mirror media_browser::ui::ExitModal
// (the canonical two-button confirm modal): centered card with gold
// border, a title, two action buttons at the bottom, BTN1/BTN3 (PREV/
// NEXT) toggle focus, the rotary toggles focus, SELECT commits, BTN4
// (SETTINGS_MENU) is the Dismiss/Cancel shortcut.

#include "platform/input_manager.h"

#include <functional>
#include <string>
#include <vector>

namespace ui { class Renderer; }

namespace media_browser::ui {

class StallPromptModal {
public:
    using PickHandler    = std::function<void(int tmdb_id, const std::string& title)>;
    using DismissHandler = std::function<void(int tmdb_id)>;

    struct Pending {
        int         tmdb_id = 0;
        std::string title;
        std::string reason_label;  // e.g., "No peers connected after 60s"
    };

    void set_handlers(PickHandler on_pick, DismissHandler on_dismiss);

    // Open the modal for `p`. Resets focus to "Pick another" (the
    // primary action — stall events are most useful when the user
    // picks a different release).
    void show(Pending p);

    bool is_active() const { return active_; }

    // Per-frame render. Modal is overlaid on top of whatever screen is
    // currently active; the underlying screen keeps drawing so the user
    // can still see context behind the dim backdrop.
    void render(::ui::Renderer& r, int screen_w, int screen_h);

    // Process a frame of input events. Returns true if input was
    // consumed (caller should NOT pass to active screen). While active
    // the modal traps ALL input, so the return value is effectively
    // `is_active()` after dispatch — but callers should treat it as the
    // "consume" flag per event-loop convention.
    bool handle_input(const std::vector<platform::InputEvent>& events);

private:
    void commit_pick();
    void commit_dismiss();

    bool                active_ = false;
    Pending             pending_;
    int                 focus_ = 0;  // 0 = Pick another, 1 = Dismiss
    PickHandler         pick_handler_;
    DismissHandler      dismiss_handler_;
};

}  // namespace media_browser::ui
