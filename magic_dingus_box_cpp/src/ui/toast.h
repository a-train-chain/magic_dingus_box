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
    // Show a toast. Replaces any existing toast. RENDER THREAD ONLY — the
    // members below are unsynchronized and render() reads them every frame.
    static void show(std::string message);

    // Thread-safe: queue a toast from ANY thread; the next render() (render
    // thread, every frame, app-wide) shows it via show(). For outcomes that
    // land after the screen that started them is gone — e.g. a similar-film
    // quick-add deferred until the service containers a FullPause movie
    // stopped are answering again, whose result can arrive 30-90 s after
    // PlaybackScreen stopped receiving update() ticks. Screens that are
    // still on screen keep their own worker -> drain -> show() idiom.
    static void post(std::string message);

    // Thread-safe: true while a post()ed message waits for render() to show
    // it. The main loop's redraw gate only renders when something can
    // change, and the mailbox drains INSIDE render() — this is how a queued
    // toast gets its frame instead of waiting for the gate's idle timer.
    static bool has_pending();

    // Render the current toast (if any). No-op when no toast or when
    // the toast has expired.
    static void render(Renderer& r, int screen_w, int screen_h);

    // Clear any active toast immediately.
    static void clear();

    // True while a toast is on screen (fading in, holding or fading out).
    // Tests use it, and the main loop's redraw gate keeps drawing while it
    // holds.
    static bool is_active();

private:
    static std::string message_;
    static std::chrono::steady_clock::time_point shown_at_;
    static bool active_;
};

}  // namespace ui
