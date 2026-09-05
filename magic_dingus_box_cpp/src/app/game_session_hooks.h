#pragma once
#include <cstdint>
#include <functional>

namespace app {

// What the host must do around a RetroArch child process. Controller calls
// these on the main thread in this order:
//   release_input() → wake_controllers(between) → [launcher writes its script]
//   → release_display() → fork/exec → waitpid
//   → reacquire_display(w, h) → reinit_input()
// wake_controllers's `between` callback fires between the two udev triggers
// it runs internally (js* then event*) — see
// platform::PiGameSessionHooks::wake_controllers — reproducing the original
// controller.cpp's run_udevadm(js*) / progress_callback() /
// run_udevadm(event*) ordering exactly.
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

struct GameSessionHooks {
    virtual ~GameSessionHooks() = default;
    virtual void release_input() {}
    virtual void wake_controllers(const std::function<void()>& between) { (void)between; }
    virtual void release_display() {}
    virtual DisplayReacquire reacquire_display(uint32_t kiosk_w, uint32_t kiosk_h) {
        (void)kiosk_w; (void)kiosk_h;
        return {};
    }
    virtual bool reinit_input() { return true; }
};

}  // namespace app
