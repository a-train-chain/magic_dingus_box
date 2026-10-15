#pragma once
#include <atomic>
#include <cstdint>
#include <functional>

namespace app {

// What the host must do around a RetroArch child process. Controller calls
// these on the main thread in this order:
//   release_input() → wake_controllers(after) → [launcher writes its script]
//   → release_display() → fork/exec → supervise
//   → reacquire_display(w, h) → reinit_input()
// wake_controllers's `after` callback (the loading plate's progress tick)
// fires once the wake is done — the Pi wakes js* then event* through
// platform::udev::wake_input_devices(), then ticks, as controller.cpp did.
// The Pi implementation releases DRM master and evdev grabs
// (platform/pi_game_session_hooks); a host with a window system needs none of
// that, so every method defaults to "nothing to do, and it worked".

// What reacquire_display() found. `attempted` is false when the host has
// no display to reacquire (the base default, and a Pi with no DrmDisplay):
// then nothing is stored into state and no return dissolve is painted,
// exactly as the pre-seam code behaved when display_ was null.
struct DisplayReacquire {
    bool attempted = false;
    bool acquired = false;
    bool mode_restored = false;
};

// True when the host tried to take the display back after a game and could
// not (the Pi: DRM master not re-acquired after 5 retries). The kiosk then
// exits non-zero so systemd restarts it (Controller::display_lost()). A host
// with no display to reacquire (attempted=false) never loses it.
inline bool display_was_lost(const DisplayReacquire& r) {
    return r.attempted && !r.acquired;
}

// What the controller does with reacquire_display()'s answer after a game:
// when a display was reacquired at all, store whether the kiosk mode came
// back (main.cpp's reset_display handler then skips its own set_mode), and
// report a lost display. A host with no display leaves the flag alone.
inline bool apply_display_reacquire(const DisplayReacquire& r,
                                    std::atomic<bool>& display_mode_restored) {
    if (r.attempted) display_mode_restored.store(r.mode_restored);
    return display_was_lost(r);
}

struct GameSessionHooks {
    virtual ~GameSessionHooks() = default;
    virtual void release_input() {}
    virtual void wake_controllers(const std::function<void()>& after) { (void)after; }
    virtual void release_display() {}
    virtual DisplayReacquire reacquire_display(uint32_t kiosk_w, uint32_t kiosk_h) {
        (void)kiosk_w; (void)kiosk_h;
        return {};
    }
    virtual bool reinit_input() { return true; }
};

}  // namespace app
