#pragma once
#include <cstdint>

namespace app {

// What the host must do around a RetroArch child process. Controller calls
// these on the main thread in this order:
//   release_input() → wake_controllers() → [launcher writes its script]
//   → release_display() → fork/exec → waitpid
//   → reacquire_display(w, h) → reinit_input()
// The Pi implementation releases DRM master and evdev grabs
// (platform/pi_game_session_hooks); a host with a window system needs none of
// that, so every method defaults to "nothing to do, and it worked".
struct GameSessionHooks {
    virtual ~GameSessionHooks() = default;
    virtual void release_input() {}
    virtual void wake_controllers() {}
    virtual void release_display() {}
    // Returns true when the kiosk display mode was restored (the Pi reports
    // this into state.display_mode_restored).
    virtual bool reacquire_display(uint32_t kiosk_w, uint32_t kiosk_h) {
        (void)kiosk_w; (void)kiosk_h;
        return true;
    }
    virtual bool reinit_input() { return true; }
};

}  // namespace app
