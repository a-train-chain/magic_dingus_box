#pragma once

#include <string>
#include <chrono>

namespace ui {

class Renderer;  // forward

// Transient on-screen notification — a centered panel with text that
// fades in, holds, and fades out over a total of 3 seconds.
//
// Usage:
//   Toast::show("Movie section unlocked");
//   // ... each frame:
//   Toast::render(renderer, screen_w, screen_h);
class Toast {
public:
    // Show a toast. Replaces any existing toast.
    static void show(std::string message);

    // Render the current toast (if any). No-op when no toast or when
    // the toast has expired.
    static void render(Renderer& r, int screen_w, int screen_h);

    // Clear any active toast immediately.
    static void clear();

    // Test-only: returns true if a toast is active right now.
    static bool is_active();

private:
    static std::string message_;
    static std::chrono::steady_clock::time_point shown_at_;
    static bool active_;
};

}  // namespace ui
